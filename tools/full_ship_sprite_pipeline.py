#!/usr/bin/env python3
"""full_ship_sprite_pipeline.py — one command, mesh → engine-ready atlas.

The pipeline:

    inputs:
        --ship   <slug>                 # e.g. centurion
        --mesh   <path/to/ship.obj>     # source mesh
        --canonical-ref <path>          # AI conditioning ref(s); repeatable

    steps (each can be skipped via flags so re-runs are cheap):
        1. install canonical refs    (copy into assets/ships/<ship>/)
        2. write atlas-capture system  (assets/systems/<ship>_atlas.json)
        3. render reference views     (engine in --capture-clean mode +
                                       tools/render_ship_atlas.py)
        4. AI-generate primaries      (tools/batch_generate_ship_sprites.py
                                       --symmetric; mirror tool fires inside)
        5. build engine atlas manifest (tools/build_ship_atlas_manifest.py)
        6. center cells               (tools/center_ship_atlas_cells.py)
        7. feather edges              (tools/feather_sprite_edges.py)

What the orchestrator OWNS (vs the underlying tools):

    - launching/stopping the engine for the render-capture step
    - placing inputs on the conventional asset paths the rest of the
      pipeline expects (mesh → assets/meshes/ships/, canonical refs →
      assets/ships/<ship>/)
    - writing the atlas-capture system JSON for the mesh
    - chaining all seven tools with sane defaults

Sanity rails:

    - won't clobber an existing atlas-capture system unless --force-system
    - won't re-render references unless --redo-renders or renders/manifest.json
      is missing
    - won't regenerate AI sprites unless --redo-generation or no sprites yet
    - the underlying batch_generate tool already has its own skip-existing
      via --force, so we honour that downward

Usage:

    python3 tools/full_ship_sprite_pipeline.py \\
        --ship centurion \\
        --mesh assets/meshes/ships/centurion.obj \\
        --canonical-ref assets/ships/centurion/canonical_reference.png \\
        --length-meters 30 --orbit-radius 85 \\
        --quality high --parallel 2 --symmetric

    # re-run later, only redoing the AI step (e.g. after tweaking SHIP_DESIGN_BRIEFS):
    python3 tools/full_ship_sprite_pipeline.py --ship centurion --mesh ... \\
        --redo-generation
"""

from __future__ import annotations

import argparse
import json
import os
import shlex
import shutil
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]


# ---------------------------------------------------------------------------
# Step 1: install canonical reference images.
# ---------------------------------------------------------------------------


def install_canonical_refs(ship: str, refs: list[Path]) -> list[Path]:
    """Copy the user-supplied canonical refs into
    `assets/ships/<ship>/canonical_reference[_<i>].png` so
    batch_generate_ship_sprites.py picks them up via its convention-fallback.
    Returns the list of installed paths (handy for logging).

    First ref → `canonical_reference.png` (back-compat with existing CANONICAL_REFS).
    Subsequent refs → `canonical_reference_2.png`, `_3.png`, ... (alphabetical
    glob order matches the numbered order so attention weighting is stable).
    """
    if not refs:
        return []
    ship_dir = REPO / "assets" / "ships" / ship
    ship_dir.mkdir(parents=True, exist_ok=True)
    installed: list[Path] = []
    for i, src in enumerate(refs, 1):
        if not src.is_file():
            print(f"  ! canonical ref missing: {src}", file=sys.stderr)
            sys.exit(2)
        # Preserve the source extension for non-PNG inputs (rare but tools/PIL
        # handles them transparently). The downstream tools glob on .png so
        # warn if the user passed a non-PNG.
        ext = src.suffix.lower()
        if ext != ".png":
            print(f"  ! canonical ref {src} is {ext}, not .png — "
                  f"batch_generate's glob expects PNG. Converting.",
                  file=sys.stderr)
            from PIL import Image
            dst = ship_dir / (
                "canonical_reference.png" if i == 1
                else f"canonical_reference_{i}.png"
            )
            Image.open(src).convert("RGBA").save(dst)
        else:
            dst = ship_dir / (
                "canonical_reference.png" if i == 1
                else f"canonical_reference_{i}.png"
            )
            shutil.copyfile(src, dst)
        installed.append(dst)
        print(f"  installed canonical ref: {dst.relative_to(REPO)}")
    return installed


# ---------------------------------------------------------------------------
# Step 2: write the atlas-capture system JSON.
# ---------------------------------------------------------------------------


def write_atlas_system(
    ship: str,
    mesh_rel: str,
    length_meters: float,
    orbit_radius: float,
    force: bool,
) -> Path:
    """Generate `assets/systems/<ship>_atlas.json` — the capture scene the
    engine loads with --capture-clean. The mesh sits at world origin with
    clay_mode on (strips placeholder textures so the AI sprite generator
    sees pure shape), studio_lighting on (parks the sun far off-axis for
    even directional light), and the player parked at +Z so the camera
    starts behind the ship.

    `mesh_rel` should be the path relative to assets/, e.g.
    'meshes/ships/centurion.obj'. The engine resolves obj paths under
    assets/ at runtime.
    """
    out_path = REPO / "assets" / "systems" / f"{ship}_atlas.json"
    if out_path.is_file() and not force:
        print(f"  atlas-capture system already exists: {out_path.relative_to(REPO)}\n"
              f"  (use --force-system to overwrite)")
        return out_path
    cfg = {
        "name": f"{ship.title()} Atlas Capture",
        "description": (
            f"Atlas-capture scene for the {ship.title()}. clay_mode strips "
            "per-mesh diffuse/spec/glow textures so the AI sprite generator "
            "sees pure shape + Lambertian shading instead of placeholder "
            "patterns. Auto-generated by tools/full_ship_sprite_pipeline.py."
        ),
        "skybox_seed": "troy",
        "star": {"preset": "yellow"},
        "studio_lighting": True,
        "asteroid_fields": [],
        "placed_sprites": [],
        "placed_ship_sprites": [],
        "placed_meshes": [
            {
                "obj":            mesh_rel,
                "position":       [0, 0, 0],
                "euler_deg":      [0, 0, 0],
                "length_meters":  float(length_meters),
                "tint":           [0.85, 0.85, 0.88],
                "double_sided":   True,
                "ambient_floor":  0.25,
                "spec":           0.4,
                "clay_mode":      True,
            }
        ],
        "nav_points": [],
        "player_start": {"position": [0, 0, max(orbit_radius * 0.7, 30.0)]},
    }
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(cfg, indent=2) + "\n", encoding="utf-8")
    print(f"  wrote atlas-capture system: {out_path.relative_to(REPO)}")
    return out_path


# ---------------------------------------------------------------------------
# Step 3: render reference views.
#
# Owns the full engine lifecycle for the render-capture step:
#   - kill any running game (avoid two instances stomping the same dev_remote port)
#   - launch ./build/new_privateer --system <ship>_atlas --capture-clean
#   - wait for dev_remote on 127.0.0.1:47001 to accept connections
#   - run tools/render_ship_atlas.py
#   - stop the game
# ---------------------------------------------------------------------------


def install_mesh(mesh_src: Path, ship: str) -> str:
    """Install the input mesh under assets/meshes/ships/<ship>.obj if it's
    not already there. Returns the engine-relative path (sans 'assets/').
    """
    target_rel = f"meshes/ships/{ship}.obj"
    target_abs = REPO / "assets" / target_rel
    src = mesh_src.resolve()
    target_abs.parent.mkdir(parents=True, exist_ok=True)

    if src == target_abs:
        # Already in place.
        if not target_abs.is_file():
            print(f"  ! mesh path equals target but file missing: {target_abs}",
                  file=sys.stderr)
            sys.exit(2)
        print(f"  mesh already installed: {target_abs.relative_to(REPO)}")
        return target_rel
    if not src.is_file():
        print(f"  ! input mesh missing: {src}", file=sys.stderr)
        sys.exit(2)
    shutil.copyfile(src, target_abs)
    # Try to copy associated .mtl + materials.json + diffuse texture if they
    # exist next to the source — meshes commonly ship as a triplet.
    for ext in (".mtl", ".materials.json", ".png"):
        sib = src.with_suffix(ext)
        if sib.is_file():
            dst = target_abs.with_suffix(ext)
            shutil.copyfile(sib, dst)
            print(f"  installed mesh sibling: {dst.relative_to(REPO)}")
    print(f"  installed mesh: {target_abs.relative_to(REPO)}")
    return target_rel


def build_engine_if_needed() -> None:
    """Run cmake build if the binary isn't there. Doesn't force-rebuild — the
    user can run cmake themselves if they want a clean build.
    """
    binary = REPO / "build" / "new_privateer"
    if binary.is_file():
        return
    print("  building engine (./build/new_privateer not found)...")
    subprocess.run(["cmake", "--build", "build", "-j8"], check=True, cwd=REPO)


def _wait_for_port(host: str, port: int, timeout_s: float) -> bool:
    """Poll a TCP port until it accepts connections, or timeout. The engine's
    dev_remote opens this port once the system is fully loaded; using it as
    a readiness check avoids the previous sleep-and-pray approach.
    """
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        try:
            with socket.create_connection((host, port), timeout=0.5):
                return True
        except OSError:
            time.sleep(0.25)
    return False


def render_references(
    ship: str,
    radius: float,
    az_count: int,
    elevations: str,
    settle: float,
) -> None:
    binary = REPO / "build" / "new_privateer"
    if not binary.is_file():
        print(f"  ! engine binary not found at {binary}", file=sys.stderr)
        sys.exit(2)

    # Kill any prior instance — common when the user has been hand-running
    # the game in the foreground. Two instances would race on dev_remote.
    pid_file = Path("/tmp/new_privateer_game.pid")
    if pid_file.is_file():
        try:
            old = int(pid_file.read_text().strip())
            os.kill(old, signal.SIGTERM)
            time.sleep(0.5)
            os.kill(old, signal.SIGKILL)
        except (ProcessLookupError, ValueError):
            pass

    log_path = Path(f"/tmp/new_privateer_{ship}_capture.log")
    log_handle = open(log_path, "w")
    proc = subprocess.Popen(
        [str(binary), "--system", f"{ship}_atlas", "--capture-clean"],
        cwd=REPO,
        stdout=log_handle,
        stderr=subprocess.STDOUT,
    )
    pid_file.write_text(str(proc.pid))
    print(f"  engine pid={proc.pid}  log={log_path}")

    try:
        if not _wait_for_port("127.0.0.1", 47001, timeout_s=15.0):
            log_tail = log_path.read_text()[-2000:]
            print(f"  ! engine didn't open dev_remote within 15s. last log:\n{log_tail}",
                  file=sys.stderr)
            sys.exit(2)
        print("  dev_remote ready, starting render orbit...")
        outdir = REPO / "assets" / "ships" / ship / "renders"
        cmd = [
            sys.executable, str(REPO / "tools" / "render_ship_atlas.py"),
            "--ship", ship,
            "--radius", str(radius),
            "--outdir", str(outdir),
            "--az-count", str(az_count),
            "--elevations", elevations,
            "--settle", str(settle),
        ]
        print(f"  render: {' '.join(shlex.quote(c) for c in cmd)}")
        subprocess.run(cmd, check=True, cwd=REPO)
    finally:
        # Always stop the engine, even on render failure — leaving it running
        # confuses the next step (pixelart will try to read assets the engine
        # has open) and burns the user's GPU time for no reason.
        try:
            proc.terminate()
            proc.wait(timeout=3.0)
        except subprocess.TimeoutExpired:
            proc.kill()
        finally:
            log_handle.close()
            if pid_file.is_file():
                pid_file.unlink()
        print("  engine stopped.")


# ---------------------------------------------------------------------------
# Step 4: AI generation. Always uses --use-pixelart-batch so the existing
# parallelism path is reused; --symmetric flips the (180, 360) mirror half
# at the end of the wave automatically.
# ---------------------------------------------------------------------------


def generate_sprites(
    ship: str,
    quality: str,
    parallel: int,
    symmetric: bool,
    prompt_suffix_file: Path | None,
    force: bool,
) -> None:
    cmd = [
        sys.executable, str(REPO / "tools" / "batch_generate_ship_sprites.py"),
        "--ship", ship,
        "--use-pixelart-batch",
        "--parallel", str(parallel),
        "--quality", quality,
        "--clean", "--preview",
    ]
    if symmetric:
        cmd.append("--symmetric")
    if force:
        cmd.append("--force")
    if prompt_suffix_file:
        cmd += ["--prompt-suffix-file", str(prompt_suffix_file)]
    print(f"  generate: {' '.join(shlex.quote(c) for c in cmd)}")
    subprocess.run(cmd, check=True, cwd=REPO)


# ---------------------------------------------------------------------------
# Steps 5 / 6 / 7: post-processing. Trivial subprocess wrappers — the tools
# do the work, we just chain them.
# ---------------------------------------------------------------------------


def build_atlas_manifest(ship: str) -> None:
    cmd = [sys.executable, str(REPO / "tools" / "build_ship_atlas_manifest.py"),
           "--ship", ship]
    print(f"  manifest: {' '.join(shlex.quote(c) for c in cmd)}")
    subprocess.run(cmd, check=True, cwd=REPO)


def center_cells(ship: str) -> None:
    cmd = [sys.executable, str(REPO / "tools" / "center_ship_atlas_cells.py"),
           "--ship", ship, "--preview"]
    print(f"  cells: {' '.join(shlex.quote(c) for c in cmd)}")
    subprocess.run(cmd, check=True, cwd=REPO)


def feather_edges(ship: str) -> None:
    cmd = [sys.executable, str(REPO / "tools" / "feather_sprite_edges.py"),
           ship]
    print(f"  feather: {' '.join(shlex.quote(c) for c in cmd)}")
    # Feathering tracks its own .feather_applied sentinel; if it's been done
    # already the tool will fail loudly, which is fine — we surface that.
    rc = subprocess.run(cmd, cwd=REPO).returncode
    if rc == 1:
        print("  feather already applied — skipping (delete .feather_applied "
              "to re-feather).")
    elif rc != 0:
        sys.exit(rc)


# ---------------------------------------------------------------------------
# Top-level orchestration.
# ---------------------------------------------------------------------------


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("--ship", required=True,
                    help="Ship slug (lowercase, e.g. 'centurion').")
    ap.add_argument("--mesh", type=Path, required=True,
                    help="Path to the source OBJ. Will be installed at "
                         "assets/meshes/ships/<ship>.obj if not already there.")
    ap.add_argument("--canonical-ref", type=Path, action="append", default=[],
                    help="Path to a canonical reference image (paint scheme, "
                         "design sheet, etc). Repeatable; the first becomes "
                         "canonical_reference.png, subsequent ones get "
                         "_2, _3, ... suffixes. Optional but strongly "
                         "recommended.")
    ap.add_argument("--length-meters", type=float, default=30.0,
                    help="Mesh length in the atlas-capture scene. The AI "
                         "doesn't see this directly — the orbit-radius / "
                         "length ratio just sets framing. Default 30.")
    ap.add_argument("--orbit-radius", type=float, default=85.0,
                    help="Camera orbit radius in the render step. Default 85 "
                         "(works for 30m-rendered ships).")
    ap.add_argument("--az-count", type=int, default=16)
    ap.add_argument("--elevations", default="-90,-60,-30,0,30,60,90")
    ap.add_argument("--settle", type=float, default=0.15)
    ap.add_argument("--quality", default="high",
                    choices=["low", "medium", "high", "auto"])
    ap.add_argument("--parallel", type=int, default=2)
    ap.add_argument("--symmetric", action=argparse.BooleanOptionalAction,
                    default=True,
                    help="Bilateral-symmetry shortcut: generate only the "
                         "primary 47 frames and PIL-flip the mirrored 35. "
                         "Default ON. Disable for ships with asymmetric "
                         "mounts/sensors/decals.")
    ap.add_argument("--prompt-suffix-file", type=Path, default=None,
                    help="Forwarded to batch_generate_ship_sprites.py.")

    # Skip-flags for re-runs.
    ap.add_argument("--skip-mesh-install", action="store_true")
    ap.add_argument("--skip-canonical-install", action="store_true")
    ap.add_argument("--skip-system-write", action="store_true")
    ap.add_argument("--force-system", action="store_true",
                    help="Overwrite an existing assets/systems/<ship>_atlas.json.")
    ap.add_argument("--skip-renders", action="store_true",
                    help="Don't re-render reference views. Implies engine "
                         "isn't launched. Use when renders/ already exists.")
    ap.add_argument("--redo-renders", action="store_true",
                    help="Force re-render even if renders/manifest.json exists.")
    ap.add_argument("--skip-generation", action="store_true")
    ap.add_argument("--force-generation", action="store_true",
                    help="Pass --force to batch_generate so existing sprites "
                         "are regenerated.")
    ap.add_argument("--skip-manifest", action="store_true")
    ap.add_argument("--skip-cells", action="store_true")
    ap.add_argument("--skip-feather", action="store_true",
                    help="Don't run feather_sprite_edges. Default skips it "
                         "automatically if .feather_applied exists.")
    ap.add_argument("--feather", action="store_true",
                    help="Force-run feathering. Without this flag it's "
                         "skipped (it's a one-way visual operation; better "
                         "to opt in once the atlas looks final).")

    args = ap.parse_args()

    print(f"[pipeline] ship={args.ship}")

    # ---- 1. canonical refs ------------------------------------------------
    if not args.skip_canonical_install:
        print("[pipeline] step 1/7: install canonical refs")
        install_canonical_refs(args.ship, args.canonical_ref)
    else:
        print("[pipeline] step 1/7: SKIP canonical refs")

    # ---- 2. mesh install -------------------------------------------------
    if not args.skip_mesh_install:
        print("[pipeline] step 2/7: install mesh")
        mesh_rel = install_mesh(args.mesh, args.ship)
    else:
        mesh_rel = f"meshes/ships/{args.ship}.obj"
        print(f"[pipeline] step 2/7: SKIP mesh install; assuming {mesh_rel}")

    # ---- 3. atlas-capture system ----------------------------------------
    if not args.skip_system_write:
        print("[pipeline] step 3/7: write atlas-capture system")
        write_atlas_system(args.ship, mesh_rel, args.length_meters,
                           args.orbit_radius, args.force_system)
    else:
        print("[pipeline] step 3/7: SKIP atlas-capture system")

    # ---- 4. render references --------------------------------------------
    renders_manifest = (REPO / "assets" / "ships" / args.ship /
                        "renders" / "manifest.json")
    if args.skip_renders:
        print("[pipeline] step 4/7: SKIP renders (--skip-renders)")
    elif renders_manifest.is_file() and not args.redo_renders:
        print(f"[pipeline] step 4/7: SKIP renders ({renders_manifest.relative_to(REPO)} "
              f"exists; pass --redo-renders to re-capture)")
    else:
        print("[pipeline] step 4/7: render reference views (engine launches)")
        build_engine_if_needed()
        render_references(args.ship, args.orbit_radius, args.az_count,
                          args.elevations, args.settle)

    # ---- 5. AI generation ------------------------------------------------
    if args.skip_generation:
        print("[pipeline] step 5/7: SKIP AI generation")
    else:
        print("[pipeline] step 5/7: AI generate sprites "
              f"(symmetric={args.symmetric}, parallel={args.parallel})")
        generate_sprites(args.ship, args.quality, args.parallel,
                         args.symmetric, args.prompt_suffix_file,
                         args.force_generation)

    # ---- 6. atlas manifest -----------------------------------------------
    if args.skip_manifest:
        print("[pipeline] step 6/7: SKIP atlas manifest")
    else:
        print("[pipeline] step 6/7: build atlas manifest")
        build_atlas_manifest(args.ship)

    # ---- 7. center cells -------------------------------------------------
    if args.skip_cells:
        print("[pipeline] step 7/7: SKIP center cells")
    else:
        print("[pipeline] step 7/7: center cells")
        center_cells(args.ship)

    # ---- optional: feather edges -----------------------------------------
    if args.feather and not args.skip_feather:
        print("[pipeline] optional: feather sprite edges")
        feather_edges(args.ship)
    else:
        print("[pipeline] feather: skipped (pass --feather to run)")

    print(f"[pipeline] done. inspect assets/ships/{args.ship}/contact_sheets/ "
          f"or load `{args.ship}_sprite_atlas` (if you've authored one) to review.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env -S uv run --quiet
# /// script
# requires-python = ">=3.10"
# dependencies = ["numpy"]
# ///
"""auto_author_engine_lights.py — generate per-cell `.lights.json` sidecars
by projecting authored 3D ship-local light positions through each atlas
frame's orbit camera.

Why projection instead of pixel detection
-----------------------------------------
Earlier attempt: scan each cell PNG for bright thruster cores. Failed —
the AI stylization paints recognisable navy diamonds on the rear stern
view only, and renders the same engine bells as featureless gray
cylinders from every other angle. With no consistent visual landmark,
detection fizzles to 2/80 cells.

This tool flips the problem: author the lights ONCE in 3D ship-local
space (the engine bell tips, cockpit windows, whatever), then for every
sample in the atlas manifest project the 3D point through the same
orbit-camera math `render_ship_atlas.py` used to render that cell. The
projected (u, v) lands on the right hull feature regardless of how
artistic the stylized cell turned out, with one caveat: the AI nudges
silhouettes around so per-cell pixel-precision isn't guaranteed. Use F2
to nudge any cell that drifted.

Backface culling
----------------
A light on the stern of the ship shouldn't render when the camera is
looking at the nose. We detect this by computing the camera's forward
direction (always pointing at origin from the orbit position) and
dotting it with the light's outward direction from origin — if the
light is on the far side of the ship (dot > threshold), skip. This
isn't physically correct (it ignores hull self-occlusion) but it's
adequate for the common case of "is this light visible from this view".

Usage
-----
    tools/auto_author_engine_lights.py
    tools/auto_author_engine_lights.py --dry-run
    tools/auto_author_engine_lights.py --overwrite
    tools/auto_author_engine_lights.py --ship tarsus
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np


# ---------------------------------------------------------------------------
# Authored 3D lights (ship-local OBJ coords)
# ---------------------------------------------------------------------------
#
# Coordinate frame: forward axis = +Z, up = +Y. OBJ space is ~unit-scale
# (the Tarsus extends z ∈ [-1.0, +1.0]).
#
# Reality check: the Tarsus has TWO engine pods (port + stbd), each a
# single cylindrical bell. The AI cell renderer hallucinates a 4-bell
# vertical-pair layout in the stern view — that's stylization, not
# geometry. Pixel-detection on the reference renders (pre-AI) confirms
# 2 blobs, not 4. We project the 2 real pods; if you want the AI's
# 4-bell look, F2-split each pod into upper/lower in stern cells.
#
# Pod XY came from clustering tarsus.obj's rear band into 2 groups; Z
# is just inside the rear bell openings. Y is below the ship's centroid
# because the engine cluster sits in the lower hull section.

@dataclass
class AuthoredLight:
    name: str
    pos: tuple[float, float, float]   # ship-local 3D
    color: tuple[int, int, int] = (140, 200, 255)
    size: float = 4.0                  # world units (Tarsus is ~85m)
    hz: float = 0.0
    phase: float = 0.0
    kind: str = "steady"
    # Lights only render when the line from origin to this point points
    # roughly toward the camera (dot(point_dir, cam_forward) <= threshold).
    # Engines on the stern are a great fit; omnidirectional nav strobes
    # set this False.
    backface_cull: bool = True


TARSUS_LIGHTS: list[AuthoredLight] = [
    AuthoredLight("engine_port", (-0.252, -0.082, -0.913),
                  color=(140, 220, 255), size=4.5, hz=0.0, phase=0.00),
    AuthoredLight("engine_stbd", (+0.252, -0.082, -0.913),
                  color=(140, 220, 255), size=4.5, hz=0.0, phase=0.50),
]


SHIP_LIGHT_PRESETS: dict[str, list[AuthoredLight]] = {
    "tarsus": TARSUS_LIGHTS,
}


# ---------------------------------------------------------------------------
# Camera / projection math (mirrors render_ship_atlas.py)
# ---------------------------------------------------------------------------

def orbit_position(az_deg: float, el_deg: float, radius: float = 35.0
                   ) -> np.ndarray:
    """Camera world-space position. Same convention as render_ship_atlas."""
    az = math.radians(az_deg)
    el = math.radians(el_deg)
    return np.array([
        radius * math.cos(el) * math.sin(az),
        radius * math.sin(el),
        radius * math.cos(el) * math.cos(az),
    ])


def view_basis(cam_pos: np.ndarray) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Camera-space basis vectors {right, up, forward} for a camera at
    `cam_pos` looking at the origin with world-up = +Y.

    Forward points FROM camera TO origin (= -normalize(cam_pos)). Right is
    the normalised cross of world-up and forward, ordered so +X in camera
    space corresponds to "to the right of frame" (standard right-handed
    view). Up is forward × right to complete the basis.
    """
    forward = -cam_pos / np.linalg.norm(cam_pos)
    world_up = np.array([0.0, 1.0, 0.0])
    right = np.cross(world_up, forward)
    rn = np.linalg.norm(right)
    if rn < 1e-6:
        # Camera straight up/down: pick any horizontal right. Atlas only
        # samples |el| ≤ 60° so we don't actually hit this.
        right = np.array([1.0, 0.0, 0.0])
    else:
        right = right / rn
    up = np.cross(forward, right)
    return right, up, forward


# Empirical OBJ→world scale that matches the AI cell framing for the
# Tarsus. Derived by measuring the port engine pod's u-displacement on
# the az=180 stern cell (u=0.121 ⇒ ndc_x=-0.758) and solving for `scale`
# in `ndc_x = -0.252*scale / (35 - 0.913*scale) / 0.20`. The Y axis is
# slightly compressed in the AI's framing relative to X — accepting ~10%
# vertical drift on stern cells; F2 fine-tunes.
DEFAULT_OBJ_SCALE = 13.5
# Half-FOV tangent in CELL UV space: vertical FOV 40° (engine default),
# central crop = 0.55. Cells are square so X uses the same value.
DEFAULT_HALF_FOV_TAN_CELL = math.tan(math.radians(40.0) / 2.0) * 0.55


def project_to_uv(point_obj: np.ndarray,
                  cam_pos: np.ndarray,
                  obj_scale: float = DEFAULT_OBJ_SCALE,
                  half_fov_tan: float = DEFAULT_HALF_FOV_TAN_CELL,
                  ) -> tuple[float, float, float]:
    """Project a world-space point through the orbit camera to UV in [0,1]^2.

    Returns (u, v, dot_with_forward). u/v use the F2 editor convention:
    u=0 left, u=1 right, v=0 TOP of cell PNG as stored on disk, v=1
    BOTTOM. This matches what the F2 editor stores for a click at that
    pixel — the runtime spot pipeline interprets the same way (and
    handles the cell's vertical flip internally).

    `dot_with_forward` > 0 when the point's outward direction from origin
    points away from the camera, i.e. the point is on the far side of
    the ship — used as the backface-cull signal.
    """
    right, up, forward = view_basis(cam_pos)
    point_world = point_obj * obj_scale
    rel = point_world - cam_pos
    x_cam = float(np.dot(rel, right))
    y_cam = float(np.dot(rel, up))
    z_cam = float(np.dot(rel, forward))   # positive = in front of camera

    if z_cam <= 1e-6:
        return (math.nan, math.nan, -1.0)

    ndc_x = (x_cam / z_cam) / half_fov_tan
    ndc_y = (y_cam / z_cam) / half_fov_tan

    # NDC [-1,+1]² → UV [0,1]². u/v match the F2 editor's storage:
    # ls.v = mouse_uv.y from ImGui's top-down click coord, so v=0 is the
    # top of the editor display (= top of the rendered ship in-game) and
    # v=1 is the bottom. A 3D point with ndc_y > 0 lands at the top of
    # the screen, so v = (1 - ndc_y) / 2.
    u = (ndc_x + 1.0) * 0.5
    v = (1.0 - ndc_y) * 0.5

    p_norm = float(np.linalg.norm(point_obj))
    backface_dot = (0.0 if p_norm < 1e-6
                    else float(np.dot(point_obj / p_norm, forward)))
    return (u, v, backface_dot)


# ---------------------------------------------------------------------------
# Sidecar emission
# ---------------------------------------------------------------------------

def light_to_json_dict(light: AuthoredLight, u: float, v: float) -> dict:
    return {
        "u": round(u, 4),
        "v": round(v, 4),
        "color": list(light.color),
        "size": round(light.size, 2),
        "hz": light.hz,
        "phase": light.phase,
        "kind": light.kind,
    }


def write_sidecar(path: Path, lights: list[dict]) -> None:
    """Match the exact format `sprite_light_editor::save_lights_sidecar` writes."""
    lines = ["["]
    for i, l in enumerate(lights):
        suffix = "," if i + 1 < len(lights) else ""
        c = l["color"]
        lines.append(
            f'  {{ "u": {l["u"]}, "v": {l["v"]}, '
            f'"color": [{c[0]}, {c[1]}, {c[2]}], '
            f'"size": {l["size"]}, "hz": {l["hz"]}, '
            f'"phase": {l["phase"]}, "kind": "{l["kind"]}" }}{suffix}'
        )
    lines.append("]")
    path.write_text("\n".join(lines) + "\n")


# ---------------------------------------------------------------------------
# Driver
# ---------------------------------------------------------------------------

@dataclass
class CellSidecarPlan:
    sprite_rel: str       # path under assets/, .png stripped
    az_deg: float
    el_deg: float
    lights: list[dict] = field(default_factory=list)
    skipped_for_backface: int = 0


def plan_for_manifest(manifest: dict, lights: list[AuthoredLight],
                      backface_threshold: float) -> list[CellSidecarPlan]:
    """Walk every (az, el) sample and project all authored lights."""
    plans: list[CellSidecarPlan] = []
    for sample in manifest["samples"]:
        az = float(sample["az"])
        el = float(sample["el"])
        sprite = sample["sprite"]
        rel = sprite[:-4] if sprite.endswith(".png") else sprite

        cam = orbit_position(az, el)
        plan = CellSidecarPlan(sprite_rel=rel, az_deg=az, el_deg=el)
        for light in lights:
            u, v, bf_dot = project_to_uv(np.array(light.pos), cam)
            if math.isnan(u):
                continue
            if light.backface_cull and bf_dot > backface_threshold:
                plan.skipped_for_backface += 1
                continue
            if not (0.0 <= u <= 1.0 and 0.0 <= v <= 1.0):
                # Off-cell projection. Drop rather than clamp so F2 gizmos
                # don't pile up on the cell border.
                continue
            plan.lights.append(light_to_json_dict(light, u, v))
        plans.append(plan)
    return plans


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ship", default="tarsus", choices=sorted(SHIP_LIGHT_PRESETS),
                    help="which preset light list to project (default: tarsus)")
    ap.add_argument("--manifest", default=None,
                    help="atlas manifest path "
                         "(default: assets/ships/<ship>/atlas_manifest.json)")
    ap.add_argument("--overwrite", action="store_true",
                    help="replace existing .lights.json sidecars "
                         "(otherwise only new files)")
    ap.add_argument("--dry-run", action="store_true",
                    help="report what would be written, touch nothing")
    ap.add_argument("--backface-threshold", type=float, default=0.0,
                    help="cull lights whose outward-from-origin dot with "
                         "cam-forward exceeds this; 0.0 culls anything on "
                         "the far hemisphere (default 0.0)")
    args = ap.parse_args()

    repo_root = Path(__file__).resolve().parent.parent
    manifest_path = (Path(args.manifest) if args.manifest
                     else repo_root / f"assets/ships/{args.ship}/atlas_manifest.json")
    if not manifest_path.exists():
        print(f"manifest not found: {manifest_path}", file=sys.stderr)
        return 2

    manifest = json.loads(manifest_path.read_text())
    lights = SHIP_LIGHT_PRESETS[args.ship]
    print(f"projecting {len(lights)} authored 3D light(s) into "
          f"{len(manifest['samples'])} atlas cell(s)…", file=sys.stderr)

    plans = plan_for_manifest(manifest, lights, args.backface_threshold)

    written = 0
    skipped_existing = 0
    for plan in plans:
        sidecar_path = repo_root / "assets" / (plan.sprite_rel + ".lights.json")
        if sidecar_path.exists() and not args.overwrite:
            skipped_existing += 1
            continue
        if not plan.lights:
            print(f"  [{plan.az_deg:6.1f},{plan.el_deg:+5.1f}] "
                  f"all {plan.skipped_for_backface} light(s) culled — "
                  f"no sidecar", file=sys.stderr)
            continue
        msg = (f"  [{plan.az_deg:6.1f},{plan.el_deg:+5.1f}] "
               f"{len(plan.lights)} light(s)")
        if plan.skipped_for_backface:
            msg += f"  ({plan.skipped_for_backface} culled)"
        msg += f"  → {sidecar_path.name}"
        if args.dry_run:
            print("  would write " + msg.lstrip(), file=sys.stderr)
        else:
            write_sidecar(sidecar_path, plan.lights)
            written += 1
            print(msg, file=sys.stderr)

    cells_with_lights = sum(1 for p in plans if p.lights)
    total_lights = sum(len(p.lights) for p in plans)
    print(
        f"\nsummary: {cells_with_lights}/{len(plans)} cells will receive lights, "
        f"{total_lights} sidecar-light(s) total"
        + (f", {written} file(s) written" if not args.dry_run else "")
        + (f", {skipped_existing} existing file(s) preserved (use --overwrite)"
           if skipped_existing else ""),
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())

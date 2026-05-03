#!/usr/bin/env python3
"""finetune_ship_sprite.py — iterate on an existing AI ship-sprite frame.

Different beast from batch_generate_ship_sprites.py:

  batch_generate: starts from scratch each time. Reference images = clay
                  render(s) + canonical design sheet. Useful for the
                  initial 82-frame wave generation.

  finetune (this): starts from an EXISTING generated sprite and applies
                   small targeted edits described in a regen note. The
                   prior sprite is the dominant reference; the clay
                   render is a secondary pose anchor; the canonical
                   sheet stays as a paint/style reference.

Concretely we call the same UC `pixelart.generate_sprite` plugin, but with:

    reference_image = [prior_sprite, per_angle_render, *canonical_refs]
    reference_strength = "strict"

and a finetune-flavored `subject` prompt that explicitly tells the model
to iterate on the prior sprite rather than treat it as an additional
style reference. This matches the F4 atlas grid viewer's per-frame
"Regen note" + "Regen now (bg)" workflow: human spots a flaw → types a
note → engine writes a regen script → script invokes this tool.

The writes-and-cleans path is identical to batch_generate so a refined
sprite drops into the existing atlas pipeline (atlas_manifest →
center_ship_atlas_cells → engine) with no extra steps.

Usage:
    python3 tools/finetune_ship_sprite.py \
        --ship centurion \
        --az 0 --el 90 \
        --prompt-suffix-file /tmp/note.txt \
        --quality high --clean --preview
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import shlex
import subprocess
import sys
from pathlib import Path

# Reuse the design briefs / prompt helpers from the batch tool. Keeps the
# style block, ship-specific silhouette language, canonical-ref resolution,
# and angle-naming logic in ONE place — DRY for the prompt mechanics, with
# a finetune-specific prompt body wrapping it.
REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tools"))
from batch_generate_ship_sprites import (  # noqa: E402
    EXTRA_STYLE,
    SHIP_DESIGN_BRIEFS,
    az_tag,
    angle_tag,
    _angle_orientation_hint,
    resolve_canonical_refs,
)


# ---------------------------------------------------------------------------
# Pixelart plugin loader — copied from batch_generate so this script doesn't
# require batch_generate's CLI to be invoked first. Cached at module scope so
# repeated calls in a session don't re-import.
# ---------------------------------------------------------------------------

# Mirror batch_generate's resolution: prefer the in-repo copy under
# tools/pixelart/, fall back to the user's plugin install. The in-repo
# copy is the source of truth for this project so generation behaviour
# stays reproducible across machines.
PIXELART_GENERATE = REPO / "tools" / "pixelart" / "generate_sprite.py"
_GENERATE_SPRITE = None


def _load_pixelart_generate_sprite():
    global _GENERATE_SPRITE
    if _GENERATE_SPRITE is not None:
        return _GENERATE_SPRITE
    if not PIXELART_GENERATE.is_file():
        raise RuntimeError(
            f"pixelart plugin not found at {PIXELART_GENERATE}. "
            f"Expected the in-repo copy under tools/pixelart/generate_sprite.py."
        )
    spec = importlib.util.spec_from_file_location(
        "_pixelart_generate_sprite_finetune", PIXELART_GENERATE
    )
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Could not load pixelart plugin at {PIXELART_GENERATE}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    _GENERATE_SPRITE = module.generate_sprite
    return _GENERATE_SPRITE


# ---------------------------------------------------------------------------
# Prior-sprite discovery.
#
# We prefer the .raw.png because it's the model's own untouched 1024-px
# output, which it can iterate on cleanly without compounded pixelization
# artifacts. Fall back to the post-pixelization .png (still 1024-ish), and
# finally to the alpha-cleaned _clean.png if neither raw is around (rare —
# means batch_generate ran with save_raw=False at some point in the past).
# ---------------------------------------------------------------------------

PRIOR_PRECEDENCE = (
    "{stem}.raw.png",         # original untouched 1024 output (best for finetune)
    "{stem}.png",             # downscaled / pixelized version (still good)
    "{stem}_clean.png",       # alpha-keyed (last resort)
)


def find_prior_sprite(sprite_dir: Path, stem: str) -> Path | None:
    for tmpl in PRIOR_PRECEDENCE:
        p = sprite_dir / tmpl.format(stem=stem)
        if p.is_file():
            return p
    return None


# ---------------------------------------------------------------------------
# Prompt construction.
#
# Crucial difference from batch_generate's build_prompt: the FIRST reference
# image is the prior sprite (not the clay render). The model attention
# leaders treat the first image as the dominant signal, so we put what we
# want it to iterate on at the front. The clay render comes next as a pose
# anchor — needed because the prior sprite alone could drift into a
# different (az, el) if the user's edit notes are vague. Canonical sheet
# trails for paint consistency.
# ---------------------------------------------------------------------------


def build_finetune_prompt(
    ship: str,
    prior_sprite: Path,
    clay_render: Path,
    canonical_refs: tuple[Path, ...],
    az: float,
    el: float,
    user_note: str,
) -> str:
    pretty_ship = ship.replace("_", " ").title()
    brief = SHIP_DESIGN_BRIEFS.get(ship, {
        "role": "spacecraft",
        "silhouette": "as defined by the prior sprite",
        "palette": "as defined by the prior sprite",
    })
    angle_hint = _angle_orientation_hint(az, el)

    canon_lines = "\n".join(f"  - {p}" for p in canonical_refs) or "  (none)"

    user_block = (
        f"USER REGEN NOTE — apply these changes:\n{user_note}\n"
        if user_note.strip()
        else "USER REGEN NOTE: (none provided — refine quality, fix any\n"
             "obvious AI artifacts, otherwise preserve the prior sprite.)\n"
    )

    return f"""\
Subject: REFINE an existing pixel-art sprite of a {brief['role']} ({pretty_ship})
viewed from {angle_hint}.

THIS IS A FINE-TUNE PASS, NOT A FROM-SCRATCH GENERATION.
Your job is to take the FIRST reference image — the previously-generated
sprite — and apply small targeted edits described below. Preserve everything
that the user does not explicitly ask to change: silhouette, pose,
palette, panel detailing, light placement, scale, framing.

{user_block}

REFERENCE IMAGES:
  PRIMARY (the prior sprite — iterate on this; preserve unless explicitly
  edited by the regen note above):
  - {prior_sprite}

  POSE ANCHOR (low-fidelity 3D clay render of THIS exact (az, el); use only
  to confirm the camera angle is correct and the silhouette hasn't drifted
  — do NOT copy its placeholder textures):
  - {clay_render}

  CANONICAL DESIGN SHEET (paint scheme, hull colour, material finish; use
  ONLY for material/colour fidelity, not pose):
{canon_lines}

DESIGN BRIEF (for context — the prior sprite already obeys this):
  Silhouette: {brief['silhouette']}
  Palette:    {brief['palette']}

OUTPUT REQUIREMENTS:
- Same view angle as the prior sprite (az={az:g}, el={el:g}).
- Same canvas size and framing.
- Same pixel-art style and palette as the prior sprite.
- Apply the user's regen note edits surgically; leave everything else alone.

{EXTRA_STYLE}
"""


# ---------------------------------------------------------------------------
# Main entry.
# ---------------------------------------------------------------------------


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--ship", required=True)
    ap.add_argument("--az", type=float, required=True)
    ap.add_argument("--el", type=float, required=True)
    ap.add_argument("--prompt-suffix-file", type=Path, default=None,
                    help="Text file containing the user's regen notes "
                         "(per-frame and/or ship-wide). Empty/missing = "
                         "minimal-edit refinement pass.")
    ap.add_argument("--prompt-suffix", default="",
                    help="Inline alternative to --prompt-suffix-file.")
    ap.add_argument("--quality", default="high",
                    choices=["low", "medium", "high", "auto"])
    ap.add_argument("--reference-strength", default="strict",
                    choices=["strict", "loose"],
                    help="Passed through to pixelart.generate_sprite. "
                         "Strict = stick close to references; loose = more freedom.")
    ap.add_argument("--clean", action="store_true",
                    help="Run the alpha cleaner after generation.")
    ap.add_argument("--preview", action="store_true",
                    help="When cleaning, also write the *_on_black.png preview.")
    ap.add_argument("--no-rebuild-cells", action="store_true",
                    help="Skip the post-clean atlas-cell rebuild. Default is to "
                         "run center_ship_atlas_cells.py for this ship so the "
                         "engine-facing _cell.png is in sync with the new clean "
                         "sprite. Disable if you're chaining multiple finetune "
                         "runs and want to rebuild cells once at the end.")
    ap.add_argument("--symmetric", action="store_true",
                    help="Bilateral-symmetry mode. After finetuning a primary "
                         "frame (az ∈ [0, 180]), horizontally flip its mirror "
                         "partner (az' = 360 - az) so both halves stay in sync. "
                         "Editing a mirror frame directly is rejected — edit the "
                         "primary instead.")
    ap.add_argument("--timeout", type=float, default=240.0,
                    help="Seconds to wait for the API call before failing.")
    args = ap.parse_args()

    # Symmetric-mode guard: refuse to AI-edit a mirror cell directly. The
    # mirror is always derived from the primary, so per-mirror edits would
    # silently get blown away on the next primary regen. Friendlier to fail
    # loudly with a redirect than to surprise the user.
    if args.symmetric:
        az_norm = float(args.az) % 360.0
        if abs(args.el) < 89.5 and 180.5 < az_norm < 359.5:
            primary_az = round(360.0 - az_norm, 6)
            print(
                f"[finetune] refusing to edit mirror frame (az={args.az}, "
                f"el={args.el}) directly under --symmetric.\n"
                f"  → edit the primary at az={primary_az}, el={args.el} "
                f"and the mirror will be regenerated automatically.",
                file=sys.stderr,
            )
            return 2

    # Load the user note from disk OR --prompt-suffix. File wins if both are
    # given, since the F4 viewer always writes a file.
    user_note = ""
    if args.prompt_suffix_file:
        if args.prompt_suffix_file.is_file():
            user_note = args.prompt_suffix_file.read_text(encoding="utf-8").strip()
        else:
            print(f"  ! note file missing: {args.prompt_suffix_file}", file=sys.stderr)
    if not user_note:
        user_note = args.prompt_suffix.strip()

    # Locate render + prior sprite.
    render_dir = REPO / "assets" / "ships" / args.ship / "renders"
    sprite_dir = REPO / "assets" / "ships" / args.ship / "sprites"

    manifest_path = render_dir / "manifest.json"
    if not manifest_path.is_file():
        print(f"  ! no render manifest at {manifest_path}", file=sys.stderr)
        return 2
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))

    # Find the matching sample. Snap to nearest authored angle so users can
    # type round numbers (--az 0 --el 90) and not worry about float exactness.
    target = None
    for sample in manifest["samples"]:
        if (int(round(float(sample["az"]))) == int(round(args.az))
            and int(round(float(sample["el"]))) == int(round(args.el))):
            target = sample
            break
    if target is None:
        print(f"  ! no manifest sample for az={args.az}, el={args.el}",
              file=sys.stderr)
        return 2
    az, el = float(target["az"]), float(target["el"])

    clay_render = render_dir / target["file"]
    if not clay_render.is_file():
        print(f"  ! clay render missing: {clay_render}", file=sys.stderr)
        return 2

    stem = f"{args.ship}_az{az_tag(az)}_el{angle_tag(el)}_newmodel_512"
    prior = find_prior_sprite(sprite_dir, stem)
    if prior is None:
        print(
            f"  ! no prior sprite found for {stem}. Looked for: "
            f"{', '.join(t.format(stem=stem) for t in PRIOR_PRECEDENCE)}\n"
            f"  → run batch_generate_ship_sprites.py first to seed the frame.",
            file=sys.stderr,
        )
        return 2

    canonical = resolve_canonical_refs(args.ship, REPO)
    raw_output = sprite_dir / f"{stem}.png"
    clean_output = sprite_dir / f"{stem}_clean.png"

    prompt = build_finetune_prompt(args.ship, prior, clay_render,
                                   canonical, az, el, user_note)

    # Write the prompt to disk for diff-friendliness, alongside batch_generate's
    # prompts/ output so reviewers see all per-frame prompts together.
    prompt_dir = sprite_dir / "prompts"
    prompt_dir.mkdir(parents=True, exist_ok=True)
    finetune_prompt_path = prompt_dir / f"{stem}.finetune.txt"
    finetune_prompt_path.write_text(prompt, encoding="utf-8")

    print(f"[finetune] ship={args.ship}  az={az}  el={el}")
    print(f"  prior:        {prior}")
    print(f"  clay render:  {clay_render}")
    print(f"  canonical:    {[str(p) for p in canonical] or '(none)'}")
    print(f"  user note:    {len(user_note)} chars "
          f"({'present' if user_note else 'empty — minimal-edit pass'})")
    print(f"  prompt file:  {finetune_prompt_path}")

    # Reference list ordering matters: prior sprite first (dominant signal),
    # then pose anchor, then canonical sheet for paint. See module docstring.
    references = [str(prior), str(clay_render), *(str(p) for p in canonical)]

    generate_sprite = _load_pixelart_generate_sprite()
    raw_output.parent.mkdir(parents=True, exist_ok=True)
    print(f"  pixelart: {prior.name} -> {raw_output.name}")
    result = generate_sprite(
        subject=prompt,
        output_path=str(raw_output),
        view="side",                 # the prompt + reference carry the actual angle
        pixel_grid=512,
        palette_size=40,
        render_size=1024,
        transparent_bg=False,
        size="1024x1024",
        quality=args.quality,
        extra_style=EXTRA_STYLE,
        save_raw=True,
        reference_image=references,
        reference_strength=args.reference_strength,
        timeout=args.timeout,
    )
    print(f"  ok: {result.get('elapsed_seconds', 0):.1f}s  "
          f"{result.get('final_dimensions')}")

    # Mirror batch_generate's clean step so a finetune output drops into the
    # atlas pipeline with no extra babysitting.
    if args.clean:
        cleaner = REPO / "tools" / "clean_sprite_alpha.py"
        raw_white_bg = raw_output.with_name(raw_output.stem + ".raw.png")
        src = raw_white_bg if raw_white_bg.exists() else raw_output
        cmd = [sys.executable, str(cleaner), str(src), str(clean_output)]
        if args.preview:
            cmd.append("--preview")
        print(f"  clean: {' '.join(shlex.quote(c) for c in cmd)}")
        subprocess.run(cmd, check=True, cwd=REPO)

    # Bilateral-symmetry sync: when --symmetric and we just regenerated a
    # primary, horizontally flip the mirror partner so the (180, 360) half
    # of the atlas tracks the edit. Self-mirrors (az 0 / 180) and poles
    # have no partner — the mirror tool's az-filter handles that gracefully.
    if args.symmetric and abs(el) < 89.5:
        mirror_tool = REPO / "tools" / "mirror_symmetric_ship_frames.py"
        cmd = [sys.executable, str(mirror_tool),
               "--ship", args.ship, "--force"]
        print(f"  symmetric mirror: {' '.join(shlex.quote(c) for c in cmd)}")
        subprocess.run(cmd, check=True, cwd=REPO)

    # Rebuild the engine-facing _cell.png so the runtime atlas reads the
    # refined art on next reload. The centerer is atlas-wide because the
    # canvas size is shared across frames (max width/height across the
    # whole atlas), so single-frame mode would still need to read all 82
    # to compute that — running the whole pass costs ~1-2 s and is what
    # the engine's hot-reload depends on.
    if not args.no_rebuild_cells:
        centerer = REPO / "tools" / "center_ship_atlas_cells.py"
        cmd = [sys.executable, str(centerer), "--ship", args.ship]
        if args.preview:
            cmd.append("--preview")
        print(f"  cells: {' '.join(shlex.quote(c) for c in cmd)}")
        subprocess.run(cmd, check=True, cwd=REPO)

    return 0


if __name__ == "__main__":
    sys.exit(main())

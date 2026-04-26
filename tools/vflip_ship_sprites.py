#!/usr/bin/env python3
"""vflip_ship_sprites.py — vertically flip a ship's sprite cells in place.

Backstory: the gpt-image-2 sprite generator we use for ship view-sphere
atlases sometimes returns frames vertically inverted relative to the engine
render that conditioned them. The Tarsus came back upright; the Talon came
back upside-down (cockpit on the bottom, engines/dorsal on the top). Same
prompt, same script, same model — chalk it up to the model "knowing" that
ship sprites traditionally render with engines pointing down and forcing
that orientation regardless of the reference's actual camera angle.

Symptom in-engine: as the camera circle-strafes around the top/bottom of
the ship, the sprite appears to do a "loop-de-loop" — neighbouring atlas
cells aren't smoothly related because the ship's UP axis is inverted in
sprite space relative to what the engine's az/el cell-picker assumes.

Cheap fix: just flip the affected ship's sprites vertically. Also flips
the v-coordinates of any authored light spots (.lights.json sidecars) so
engine glow / nav lights stay on the right physical positions.

Usage:
    python3 tools/vflip_ship_sprites.py talon
    python3 tools/vflip_ship_sprites.py talon --dry-run

Idempotent guard: writes a single sentinel file at
<ship>/sprites/.vflip_applied so re-running won't double-flip and put you
back where you started. Delete the sentinel to re-flip.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from PIL import Image


# Variants we flip. Anything *.png in the sprites dir matching these tails.
# The .raw.png is the AI's pre-pixelization output; .png is the downscaled
# version; _clean.png is alpha-keyed; _clean_cell.png is the engine-loaded
# centered/padded variant; _clean_cell_on_black.png is the QA preview.
SPRITE_PNG_TAILS = (
    ".raw.png",
    ".png",                       # the plain downscaled (must match AFTER ruling out raw)
    "_clean.png",
    "_clean_cell.png",
    "_clean_cell_on_black.png",
)

SENTINEL = ".vflip_applied"


def vflip_png(path: Path) -> None:
    """In-place vertical flip of a PNG. Preserves mode + alpha."""
    im = Image.open(path)
    Image.Image.transpose(im, Image.FLIP_TOP_BOTTOM).save(path)


def _format_spot(spot: dict) -> str:
    """Match the in-engine light editor's exact one-line spot format.

    Looks like: { "u": 0.61, "v": 0.04, "color": [255, 26, 26], ... }
    The padding spaces inside the braces matter for diff cleanliness — without
    them every flip produces 80 noisy reformats on top of the actual v changes.
    """
    parts = []
    for k, v in spot.items():
        # json.dumps on a list gives the right [255, 26, 26] formatting; on a
        # bare value gives the right `"steady"` / 0.6 formatting too.
        parts.append(f'"{k}": {json.dumps(v)}')
    return "{ " + ", ".join(parts) + " }"


def vflip_lights_json(path: Path) -> int:
    """v -> 1-v for every spot in a .lights.json sidecar.

    The schema is a plain list of dicts: [{u, v, color, size, hz, phase, kind}, ...].
    Returns the number of spots flipped.
    """
    spots = json.loads(path.read_text())
    if isinstance(spots, dict) and isinstance(spots.get("spots"), list):
        # Forward-compat: some files might wrap in {"spots": [...]}.
        for spot in spots["spots"]:
            if isinstance(spot, dict) and "v" in spot:
                spot["v"] = round(1.0 - float(spot["v"]), 6)
        path.write_text(json.dumps(spots, indent=2) + "\n")
        return len(spots["spots"])
    if not isinstance(spots, list):
        return 0
    n = 0
    for spot in spots:
        if isinstance(spot, dict) and "v" in spot:
            spot["v"] = round(1.0 - float(spot["v"]), 6)
            n += 1
    path.write_text("[\n" + ",\n".join(
        "  " + _format_spot(s) for s in spots
    ) + "\n]\n")
    return n


def is_sprite_png(p: Path) -> bool:
    name = p.name
    return any(name.endswith(tail) for tail in SPRITE_PNG_TAILS)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("ship", help="ship name, e.g. 'talon'")
    ap.add_argument("--dry-run", action="store_true",
                    help="report what would be flipped without writing")
    ap.add_argument("--ships-root", default="assets/ships",
                    help="parent dir holding per-ship folders")
    args = ap.parse_args()

    ship_dir = Path(args.ships_root) / args.ship
    sprites_dir = ship_dir / "sprites"
    if not sprites_dir.is_dir():
        print(f"[vflip] no sprites dir at {sprites_dir}", file=sys.stderr)
        return 2

    sentinel = sprites_dir / SENTINEL
    if sentinel.exists() and not args.dry_run:
        print(f"[vflip] sentinel {sentinel} exists — already flipped. "
              f"Delete it to re-flip.", file=sys.stderr)
        return 1

    pngs = sorted(p for p in sprites_dir.iterdir() if is_sprite_png(p))
    lights = sorted(sprites_dir.glob("*.lights.json"))
    print(f"[vflip] ship={args.ship}")
    print(f"        sprites dir : {sprites_dir}")
    print(f"        PNG cells   : {len(pngs)}")
    print(f"        light sidecars: {len(lights)}")

    if args.dry_run:
        print("[vflip] dry-run; no files modified.")
        return 0

    for p in pngs:
        vflip_png(p)
    n_spots = sum(vflip_lights_json(p) for p in lights)

    sentinel.write_text("vflip applied; remove this file to re-flip\n")
    print(f"[vflip] flipped {len(pngs)} PNGs and {n_spots} light spots "
          f"across {len(lights)} sidecars.")
    print(f"        sentinel  : {sentinel}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""build_wcnews_atlas.py — turn the cached wcnews 1993 Privateer sprite
sheets into a real ShipSpriteAtlas the engine can load directly.

Inputs (per ship-variant):
    assets/ships/<ship>/wcnews_sprites/<variant>/<PREFIX>.IFF.ANGLES.SHP-NNN.png

Outputs (gitignored sprites + committed manifest):
    assets/ships/<ship>/sprites_wcnews/<variant>/<ship>_<variant>_az<NNN>_el<+EE>.png
    assets/ships/<ship>/atlas_manifest_<variant>_wcnews.json

The pre-bake approach: rather than teaching the engine a new mirror_h
flag, we write each off-equator yaw twice on disk — original, then a
horizontally-flipped copy at the complementary az = (360 - az) %% 360.
The engine's nearest-(az,el) frame picker then Just Works without any
schema/code changes.

Self-mirroring yaws (0° = directly behind, 180° = directly in front) are
written once. Equator frames (el=0) are written once each — the wcnews
layout already provides a full 12-frame 360° equator ring. The polar cap
ring (el=-60) is mirrored too, but only contributes 4 source frames at
60° steps, giving 6 effective cells (60°,120° each get a mirror).

Final ring counts AFTER mirroring (per variant):
    el=+60: 12 cells   (7 src + 5 mirrors)
    el=+30: 12 cells   (7 src + 5 mirrors)
    el=  0: 12 cells   (no mirror)
    el=-30: 12 cells   (7 src + 5 mirrors)
    el=-60:  6 cells   (4 src + 2 mirrors, coarse 60° step)
    -----
    total : 54 cells per variant

Upscaling is plain NEAREST 4× (125x102 → 500x408). Privateer's pixel
art is the whole point of this exercise; bicubic would blur it into a
1990s memory of nothing in particular. We can crank UPSCALE later if a
specific variant wants higher fidelity.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from PIL import Image

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tools"))
from wcnews_layout import frame_to_az_el, FRAMES_PER_SHIP   # noqa: E402

# variant slug -> wcnews IFF prefix
VARIANTS: dict[str, str] = {
    "militia":       "TALMIL",
    "pirate":        "TALPIR",
    "church_of_man": "TALRELIG",
}

UPSCALE = 4   # NEAREST integer upscale; 125x102 -> 500x408


def _mirror_az(az_deg: int) -> int:
    """Complementary azimuth for horizontal mirroring (camera reflected
    across the ship's symmetry plane)."""
    return (360 - az_deg) % 360


def _build_one(ship: str, variant: str, prefix: str) -> int:
    src_dir = REPO / "assets" / "ships" / ship / "wcnews_sprites" / variant
    out_dir = REPO / "assets" / "ships" / ship / "sprites_wcnews" / variant
    if not src_dir.is_dir():
        sys.exit(
            f"[wcnews-atlas] missing source dir: {src_dir.relative_to(REPO)}\n"
            f"               run tools/download_wcnews_ship_sprites.py first."
        )
    out_dir.mkdir(parents=True, exist_ok=True)

    samples: list[dict] = []
    written = 0
    cell_w = cell_h = None   # captured from first source for manifest metadata

    for idx in range(FRAMES_PER_SHIP):
        az, el, _ = frame_to_az_el(idx)
        src_path = src_dir / f"{prefix}.IFF.ANGLES.SHP-{idx:03d}.png"
        if not src_path.is_file():
            sys.exit(f"[wcnews-atlas] missing source frame: {src_path.relative_to(REPO)}")
        src = Image.open(src_path).convert("RGBA")
        big = src.resize((src.width * UPSCALE, src.height * UPSCALE), Image.NEAREST)
        if cell_w is None:
            cell_w, cell_h = big.size

        # Original frame
        name = f"{ship}_{variant}_az{az:03d}_el{el:+03d}.png"
        big.save(out_dir / name)
        samples.append({
            "az": float(az),
            "el": float(el),
            "sprite": f"ships/{ship}/sprites_wcnews/{variant}/{name}",
        })
        written += 1

        # Horizontal mirror for non-equator yaws that aren't self-symmetric.
        # Equator frames (el=0) already cover the full 360°, no mirror needed.
        # az ∈ {0, 180} is its own mirror (rear/front along symmetry plane),
        # writing it twice would just duplicate the same cell.
        if el != 0 and az not in (0, 180):
            maz = _mirror_az(az)
            flipped = big.transpose(Image.FLIP_LEFT_RIGHT)
            mname = f"{ship}_{variant}_az{maz:03d}_el{el:+03d}.png"
            flipped.save(out_dir / mname)
            samples.append({
                "az": float(maz),
                "el": float(el),
                "sprite": f"ships/{ship}/sprites_wcnews/{variant}/{mname}",
            })
            written += 1

    # Stable order so manifest diffs are reviewable.
    samples.sort(key=lambda s: (s["el"], s["az"]))
    azs = sorted({s["az"] for s in samples})
    els = sorted({s["el"] for s in samples})

    manifest = {
        "ship": ship,
        "variant": variant,
        "kind": "view_sphere_sprite_atlas",
        "source": "wcnews_wcpedia_3space_sprite_archive",
        "source_frames": FRAMES_PER_SHIP,
        "upscale": UPSCALE,
        "forward_axis": "+Z",
        "up_axis": "+Y",
        "azimuth_degrees": azs,
        "elevation_degrees": els,
        "cell_width":  cell_w,
        "cell_height": cell_h,
        "pixel_grid": max(cell_w, cell_h),
        "framing": (
            "Each cell is the wcnews sprite upscaled NEAREST x{u}. Off-equator "
            "yaws are pre-mirrored horizontally; el=0 ring is full 360 deg."
        ).format(u=UPSCALE),
        "samples": samples,
    }
    manifest_path = REPO / "assets" / "ships" / ship / f"atlas_manifest_{variant}_wcnews.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"  {variant:14s}: {written:2d} cells -> {manifest_path.relative_to(REPO)}")
    return written


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ship", nargs="?", default="talon",
                        help="ship slug; only 'talon' is wired up today")
    parser.add_argument("--variant", choices=list(VARIANTS), default=None,
                        help="default: build all known variants")
    args = parser.parse_args()

    variants = [args.variant] if args.variant else list(VARIANTS)
    print(f"ship: {args.ship}")
    total = sum(_build_one(args.ship, v, VARIANTS[v]) for v in variants)
    print(f"total cells across {len(variants)} variant(s): {total}")


if __name__ == "__main__":
    main()

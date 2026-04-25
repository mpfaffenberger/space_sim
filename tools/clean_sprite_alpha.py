#!/usr/bin/env python3
"""clean_sprite_alpha.py — remove chroma-key matte/fringe from generated sprites.

Image generators often return "transparent" sprites on a hot-magenta matte,
or alpha-key the obvious background while leaving purple antialias pixels along
edges. That fringe looks awful in-game. This tool aggressively keys magenta-ish
pixels, clears hidden RGB in transparent pixels, crops to content, and optionally
writes a black preview for quick visual inspection.

Usage:
    python3 tools/clean_sprite_alpha.py input.png output.png --preview
"""

from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image


def is_magenta_matte(r: int, g: int, b: int) -> bool:
    """Return true for hot-pink / purple matte and fringe colors.

    Deliberately broad: ship hull is olive/gray/brown, so saturated magenta is
    not legitimate sprite content. Better to lose a 1px contaminated edge than
    ship a neon outline. YAGNI says no fancy chroma spill solver until needed.
    """
    # Blatant hot-pink background.
    if r > 190 and b > 170 and g < 110:
        return True

    # Purple antialias fringe: red+blue dominate green by a lot.
    if r > 95 and b > 95 and g < 115 and (r - g) > 45 and (b - g) > 45:
        return True

    # Darker magenta edge pixels along alpha boundary.
    if r > 70 and b > 70 and g < 70 and (r + b) > (g * 3 + 80):
        return True

    return False


def clean_alpha(src: Path, dst: Path) -> tuple[int, int, int, int]:
    im = Image.open(src).convert("RGBA")
    px = im.load()
    w, h = im.size

    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            if a == 0 or is_magenta_matte(r, g, b):
                # Zero RGB too. Some viewers show hidden RGB for transparent
                # pixels, which made the previous "transparent" file look pink.
                px[x, y] = (0, 0, 0, 0)

    bbox = im.getbbox()
    if bbox:
        im = im.crop(bbox)
    else:
        bbox = (0, 0, 0, 0)

    dst.parent.mkdir(parents=True, exist_ok=True)
    im.save(dst, "PNG")
    return bbox


def write_preview(clean_path: Path) -> Path:
    im = Image.open(clean_path).convert("RGBA")
    black = Image.new("RGBA", im.size, (0, 0, 0, 255))
    black.alpha_composite(im)
    out = clean_path.with_name(clean_path.stem + "_on_black.png")
    black.save(out, "PNG")
    return out


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--preview", action="store_true", help="also write *_on_black.png")
    args = parser.parse_args()

    bbox = clean_alpha(args.input, args.output)
    print(f"cleaned: {args.output}")
    print(f"bbox: {bbox}")

    if args.preview:
        preview = write_preview(args.output)
        print(f"preview: {preview}")


if __name__ == "__main__":
    main()

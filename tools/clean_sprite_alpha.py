#!/usr/bin/env python3
"""clean_sprite_alpha.py — remove chroma-key matte/fringe from generated sprites.

Image generators often return "transparent" sprites on a hot-magenta matte,
or alpha-key the obvious background while leaving purple antialias pixels along
edges. That fringe looks awful in-game. This tool aggressively keys magenta-ish
pixels, clears hidden RGB in transparent pixels, crops to content, and optionally
writes a black preview for quick visual inspection.

DEFAULT BEHAVIOUR (flood-fill bg removal):
    The pixelart tool's own white-keying threshold-kills any near-white pixel
    anywhere in the frame, which eats interior hull highlights, cockpit
    glints, and bright engine details. To prevent that we re-key from the
    raw white-bg AI output via 4-connected flood fill from each image
    corner — only pixels REACHABLE from a corner via near-white pixels
    count as background. Internal bright pixels survive. This is now the
    default for any sprite that arrives with an opaque or near-opaque
    background; pass --no-flood-fill to fall back to the legacy alpha+
    magenta-key path (rarely needed).

Usage:
    python3 tools/clean_sprite_alpha.py input.png output.png --preview
    python3 tools/clean_sprite_alpha.py input.png output.png --no-flood-fill
"""

from __future__ import annotations

import argparse
from collections import deque
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


def flood_fill_background(im: Image.Image, white_threshold: int = 240) -> Image.Image:
    """Replace TRUE background with alpha=0 via 4-connectivity flood fill from
    each image corner. Internal near-white pixels (lit hull highlights, cockpit
    glints, etc.) stay opaque. Defeats the over-aggressive whole-image
    threshold keying that the upstream pixelart tool applies — that approach
    eats any bright pixel anywhere, including critical ship highlights.

    `white_threshold` (per-channel) — pixels with R, G, B all >= this count
    as background-candidate. 240 is generous enough for the AI's slightly-
    off-pure-white pages without catching anti-aliased ship-edge pixels in
    the 200-220 range.
    """
    px = im.load()
    W, H = im.size

    def is_white(x: int, y: int) -> bool:
        p = px[x, y]
        r, g, b = p[0], p[1], p[2]
        return r >= white_threshold and g >= white_threshold and b >= white_threshold

    # 4-connectivity BFS from each corner that's near-white. Anything
    # reachable from a corner via near-white pixels is OUTSIDE background;
    # anything else (white surrounded by ship) stays as ship content.
    visited = bytearray(W * H)
    queue: deque[tuple[int, int]] = deque()
    for cx, cy in ((0, 0), (W - 1, 0), (0, H - 1), (W - 1, H - 1)):
        if is_white(cx, cy) and not visited[cy * W + cx]:
            visited[cy * W + cx] = 1
            queue.append((cx, cy))

    while queue:
        x, y = queue.popleft()
        # Visit 4-neighbours.
        for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
            if 0 <= nx < W and 0 <= ny < H:
                idx = ny * W + nx
                if not visited[idx] and is_white(nx, ny):
                    visited[idx] = 1
                    queue.append((nx, ny))

    # Apply: visited (= true background) → alpha 0; rest → alpha 255.
    for y in range(H):
        for x in range(W):
            if visited[y * W + x]:
                px[x, y] = (0, 0, 0, 0)
            else:
                # Force alpha=255 in case upstream keying killed any
                # interior pixels we want to restore.
                p = px[x, y]
                if len(p) == 4:
                    px[x, y] = (p[0], p[1], p[2], 255)
                else:
                    px[x, y] = (p[0], p[1], p[2], 255)
    return im


def clean_alpha(src: Path, dst: Path, flood_fill: bool = True) -> tuple[int, int, int, int]:
    im = Image.open(src).convert("RGBA")
    px = im.load()
    w, h = im.size

    if flood_fill:
        # Corner-anchored bg removal (preserves interior highlights). Use
        # this when feeding the truly-raw AI output (.raw.png) where the
        # whole image is opaque white-bg before keying.
        im = flood_fill_background(im)
        px = im.load()

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
    # Flood-fill is the default. It's strictly safer for raw AI output (where
    # the pixelart tool's whole-image white-keying eats interior highlights)
    # AND a no-op for already-keyed inputs (corner pixels are alpha=0, not
    # near-white, so the BFS never starts). The escape hatch exists for
    # historical sprite assets that arrive on a magenta-only matte without
    # a clean white background.
    parser.add_argument("--no-flood-fill", dest="flood_fill", action="store_false",
                        help="Skip the corner-anchored flood-fill bg pass. Use"
                             " only for inputs that DON'T have a white-bg"
                             " component (i.e. magenta-only matte sprites).")
    parser.add_argument("--flood-fill", dest="flood_fill", action="store_true",
                        help=argparse.SUPPRESS)   # back-compat noop; default-on
    parser.set_defaults(flood_fill=True)
    args = parser.parse_args()

    bbox = clean_alpha(args.input, args.output, flood_fill=args.flood_fill)
    print(f"cleaned: {args.output}")
    print(f"bbox: {bbox}")

    if args.preview:
        preview = write_preview(args.output)
        print(f"preview: {preview}")


if __name__ == "__main__":
    main()

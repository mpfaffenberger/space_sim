#!/usr/bin/env python3
"""visualize_wcnews_layout.py — render a labelled contact sheet of one
ship's wcnews sprite set with the hypothesised (az, el) for each frame.

Goal: it's *very* easy to fly-check the layout by eye when each cell is
captioned with what the engine is going to ask of it. If a frame is in
the wrong slot, the disagreement will leap out (e.g. a "yaw +180 front"
caption sitting over a sprite that is plainly the rear).

Layout reverse-engineered (see tools/wcnews_layout.py for details).
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from PIL import Image, ImageDraw

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tools"))
from wcnews_layout import frame_to_az_el, FRAMES_PER_SHIP   # noqa: E402


def build_sheet(variant_dir: Path, prefix: str, scale: int, cols: int) -> Image.Image:
    files = sorted(variant_dir.glob(f"{prefix}.IFF.ANGLES.SHP-*.png"))
    if len(files) != FRAMES_PER_SHIP:
        raise SystemExit(f"expected {FRAMES_PER_SHIP} frames in {variant_dir}, got {len(files)}")

    cell_w, cell_h = 125 * scale, 102 * scale
    hdr = 28
    rows = (len(files) + cols - 1) // cols
    sheet = Image.new("RGBA", (cols * cell_w, rows * (cell_h + hdr) + 6), (16, 16, 20, 255))
    d = ImageDraw.Draw(sheet)

    for idx, p in enumerate(files):
        r, c = divmod(idx, cols)
        x, y = c * cell_w, r * (cell_h + hdr)
        az, el, mirror = frame_to_az_el(idx)
        flag = "  M" if mirror else ""
        label = f"{idx:02d}  az {az:+04d}  el {el:+03d}{flag}"
        d.rectangle([x, y, x + cell_w, y + hdr], fill=(28, 28, 34, 255))
        d.text((x + 6, y + 5), label, fill=(220, 255, 180, 255))
        im = Image.open(p).convert("RGBA").resize((cell_w, cell_h), Image.NEAREST)
        bg = Image.new("RGBA", (cell_w, cell_h), (8, 8, 10, 255))
        bg.alpha_composite(im)
        sheet.alpha_composite(bg, (x, y + hdr))

    # API constraint: image-analysis input rejects images with any edge > 2000.
    W, H = sheet.size
    m = max(W, H)
    if m > 2000:
        s = 2000.0 / m
        sheet = sheet.resize((int(W * s), int(H * s)), Image.LANCZOS)
    return sheet


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ship", nargs="?", default="talon")
    parser.add_argument("--variant", default="militia",
                        help="subfolder under wcnews_sprites/ (militia|pirate|church_of_man)")
    parser.add_argument("--prefix", default=None,
                        help="IFF prefix (e.g. TALMIL); default inferred from variant")
    parser.add_argument("--scale", type=int, default=4,
                        help="nearest-neighbour upscale factor per cell")
    parser.add_argument("--cols", type=int, default=6)
    parser.add_argument("--out", default="/tmp/diag/wcnews_layout.png")
    args = parser.parse_args()

    inferred = {"militia": "TALMIL", "pirate": "TALPIR", "church_of_man": "TALRELIG"}
    prefix = args.prefix or inferred.get(args.variant)
    if not prefix:
        raise SystemExit(f"can't infer prefix for variant {args.variant!r}; pass --prefix")

    variant_dir = REPO / "assets" / "ships" / args.ship / "wcnews_sprites" / args.variant
    sheet = build_sheet(variant_dir, prefix, args.scale, args.cols)
    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    sheet.save(args.out)
    print(f"wrote {args.out}  ({sheet.size[0]}x{sheet.size[1]})")


if __name__ == "__main__":
    main()

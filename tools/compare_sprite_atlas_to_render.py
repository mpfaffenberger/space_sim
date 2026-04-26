#!/usr/bin/env python3
"""compare_sprite_atlas_to_render.py — visual diff between ground-truth mesh
renders and the live sprite atlas as seen by the in-game renderer.

Workflow this completes (the rest already exists):

  1. Render ground-truth: boot game with `--system <ship>_atlas` and run
     `tools/render_ship_atlas.py --outdir assets/ships/<ship>/renders ...`
  2. Generate sprites:    `tools/batch_generate_ship_sprites.py` etc.
  3. Capture in-game:     boot game with `--system <ship>_sprite_atlas` and
     run `tools/render_ship_atlas.py --outdir assets/ships/<ship>/sprites_ingame ...`
  4. THIS TOOL:           build a side-by-side grid showing
                            row-N top half = ground-truth render
                            row-N bot half = in-game sprite capture
                          for all 80 (az, el) vantage points.

Why this matters: spotting orientation/cell-mapping bugs by flying around
in-game is unreliable — strafing changes both az AND el, the eye normalises
small mismatches, and intermediate-bin frames are hard to land on. A static
80-cell grid pinned to the same orbit positions both pipelines use makes
mistakes obvious in seconds.

Usage:
    python3 tools/compare_sprite_atlas_to_render.py --ship talon
    open /tmp/diag/talon_render_vs_ingame.png
"""

import argparse
import os
from pathlib import Path

from PIL import Image, ImageDraw

# Same 80-sample grid as render_ship_atlas.py defaults for atlas captures.
AZIMUTHS = ['000', '022p5', '045', '067p5', '090', '112p5', '135', '157p5',
            '180', '202p5', '225', '247p5', '270', '292p5', '315', '337p5']
ELEVATIONS = ['+060', '+030', '+000', '-030', '-060']  # top → bottom rows


def load_or_placeholder(path: Path, size: int) -> Image.Image:
    """Load a PNG resized to size×size, or return a tagged red placeholder."""
    if path.exists():
        return Image.open(path).convert('RGBA').resize((size, size), Image.BILINEAR)
    placeholder = Image.new('RGBA', (size, size), (40, 0, 0, 255))
    d = ImageDraw.Draw(placeholder)
    d.text((4, 4), 'MISSING', fill=(255, 80, 80, 255))
    d.text((4, 18), path.name[:18], fill=(255, 120, 120, 255))
    return placeholder


def build_grid(ship: str,
               renders_dir: Path,
               ingame_dir: Path,
               cell_px: int,
               out_path: Path) -> None:
    """Render the 80-cell render-vs-ingame comparison sheet."""
    label_band = 22
    row_height = 2 * cell_px + 6
    width  = cell_px * len(AZIMUTHS) + 4 * (len(AZIMUTHS) - 1) + 60
    height = (row_height + label_band) * len(ELEVATIONS) + 24

    sheet = Image.new('RGBA', (width, height), (16, 16, 20, 255))
    d = ImageDraw.Draw(sheet)
    d.text((10, 4),
           f"{ship}: TOP half of each row = mesh RENDER (ground truth)   "
           f"BOTTOM half = in-game SPRITE capture (after atlas selection)",
           fill=(220, 220, 220, 255))

    for row_idx, el in enumerate(ELEVATIONS):
        y_label = 24 + row_idx * (row_height + label_band)
        d.text((10, y_label + 4), f"el {el}", fill=(255, 255, 180, 255))
        y_render = y_label + label_band
        y_sprite = y_render + cell_px + 4
        for col_idx, az in enumerate(AZIMUTHS):
            x = 60 + col_idx * (cell_px + 4)
            # Filename convention is shared between renderer and tour script.
            stem = f"{ship}_az{az}_el{el}.png"
            r = load_or_placeholder(renders_dir / stem, cell_px)
            s = load_or_placeholder(ingame_dir / stem, cell_px)
            sheet.paste(r, (x, y_render), r)
            sheet.paste(s, (x, y_sprite), s)
            if row_idx == 0:
                d.text((x + 4, y_render - 12), az, fill=(180, 255, 180, 255))

    out_path.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(out_path)
    print(f"wrote {out_path}  ({sheet.size[0]}x{sheet.size[1]})")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ship", default="talon",
                        help="ship name (used for filenames & dir lookup)")
    parser.add_argument("--renders-dir", default=None,
                        help="override default renders dir")
    parser.add_argument("--ingame-dir", default=None,
                        help="override default sprites_ingame dir")
    parser.add_argument("--cell-px", type=int, default=130,
                        help="size in pixels of each grid cell (per half-row)")
    parser.add_argument("--out", default=None,
                        help="output PNG path (default: /tmp/diag/<ship>_render_vs_ingame.png)")
    args = parser.parse_args()

    renders_dir = Path(args.renders_dir or f"assets/ships/{args.ship}/renders")
    ingame_dir  = Path(args.ingame_dir  or f"assets/ships/{args.ship}/sprites_ingame")
    out_path    = Path(args.out or f"/tmp/diag/{args.ship}_render_vs_ingame.png")

    if not renders_dir.exists():
        raise SystemExit(f"renders dir not found: {renders_dir}")
    if not ingame_dir.exists():
        raise SystemExit(f"in-game captures dir not found: {ingame_dir}\n"
                         f"  → boot game with --system {args.ship}_sprite_atlas --capture-clean\n"
                         f"  → then: python3 tools/render_ship_atlas.py --ship {args.ship} "
                         f"--outdir {ingame_dir} --az-count 16 --elevations=-60,-30,0,30,60")

    build_grid(args.ship, renders_dir, ingame_dir, args.cell_px, out_path)


if __name__ == "__main__":
    main()

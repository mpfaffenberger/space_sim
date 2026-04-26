#!/usr/bin/env python3
"""make_sprite_contact_sheet.py — tile per-angle ship sprites into one image.

Walks a sprite directory, parses azimuth + elevation from filenames of the
form `<ship>_az<AZ>_el<EL>_*.png`, and arranges them into a labeled grid.
Each sprite is letterboxed into a fixed cell so variable-cropped outputs
align cleanly. Useful for at-a-glance QA of a freshly-generated atlas.

Usage:
    python3 tools/make_sprite_contact_sheet.py \
        --src assets/ships/talon/sprites \
        --suffix _newmodel_512_clean.png \
        --out  /tmp/talon_contact_sheet.png
"""
from __future__ import annotations

import argparse
import re
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


# `az022p5` => 22.5,  `el-060` => -60,  `el+030` => 30
_AZ_EL_RE = re.compile(r"_az(\d+)(?:p(\d+))?_el([+-]?\d+)")


def parse_az_el(stem: str) -> tuple[float, int] | None:
    m = _AZ_EL_RE.search(stem)
    if not m:
        return None
    az_int, az_frac, el = m.groups()
    az = float(az_int) + (float(f"0.{az_frac}") if az_frac else 0.0)
    return az, int(el)


def fit_into_cell(im: Image.Image, cell_w: int, cell_h: int) -> Image.Image:
    """Letterbox an RGBA image into a fixed cell, preserving pixel-art crispness.

    Uses NEAREST resampling so the chunky pixel look is preserved at the
    contact-sheet scale. The result is centered on a transparent canvas.
    """
    iw, ih = im.size
    scale = min(cell_w / iw, cell_h / ih)
    new_w, new_h = max(1, int(iw * scale)), max(1, int(ih * scale))
    scaled = im.resize((new_w, new_h), Image.Resampling.NEAREST)
    canvas = Image.new("RGBA", (cell_w, cell_h), (0, 0, 0, 0))
    canvas.paste(scaled, ((cell_w - new_w) // 2, (cell_h - new_h) // 2), scaled)
    return canvas


def _load_font(size: int) -> ImageFont.ImageFont:
    """System font with a graceful fallback to PIL's default if unavailable."""
    for candidate in (
        "/System/Library/Fonts/SFNSMono.ttf",
        "/System/Library/Fonts/Menlo.ttc",
        "/System/Library/Fonts/Supplemental/Arial.ttf",
    ):
        try:
            return ImageFont.truetype(candidate, size)
        except OSError:
            continue
    return ImageFont.load_default()


def _index_sprite_dir(
    src_dir: Path, suffix: str
) -> tuple[dict[tuple[float, int], Path], list[float], list[int]]:
    """Walk a sprite dir once, returning (cells_by_az_el, sorted_azs, sorted_els)."""
    files = sorted(src_dir.glob(f"*{suffix}"))
    if not files:
        raise SystemExit(f"no files matching *{suffix} under {src_dir}")

    cells: dict[tuple[float, int], Path] = {}
    for f in files:
        ae = parse_az_el(f.stem)
        if ae is None:
            print(f"  skip (can't parse az/el): {f.name}")
            continue
        cells[ae] = f

    azs = sorted({az for az, _ in cells})
    els = sorted({el for _, el in cells})
    return cells, azs, els


def build_contact_sheet(
    cells: dict[tuple[float, int], Path],
    azs: list[float],
    els: list[int],
    cell_w: int = 200,
    cell_h: int = 200,
    pad: int = 4,
    label_w: int = 80,
    header_h: int = 30,
    title: str = "",
) -> Image.Image:
    print(f"  sheet: {len(azs)} az rows × {len(els)} el cols   "
          f"({sum(1 for az in azs for el in els if (az, el) in cells)}"
          f"/{len(azs)*len(els)} cells filled)")

    title_h = 40 if title else 0
    sheet_w = label_w + len(els) * (cell_w + pad) + pad
    sheet_h = title_h + header_h + len(azs) * (cell_h + pad) + pad

    sheet = Image.new("RGBA", (sheet_w, sheet_h), (16, 16, 20, 255))
    draw = ImageDraw.Draw(sheet)
    font_label = _load_font(14)
    font_title = _load_font(20)

    if title:
        draw.text((label_w + pad, 10), title, fill=(220, 220, 230), font=font_title)

    # Column headers (elevations)
    for ci, el in enumerate(els):
        x = label_w + pad + ci * (cell_w + pad) + cell_w // 2
        y = title_h + header_h // 2
        draw.text((x, y), f"el {el:+d}", fill=(180, 200, 220),
                  font=font_label, anchor="mm")

    # Row labels (azimuths) + cell contents
    for ri, az in enumerate(azs):
        y0 = title_h + header_h + ri * (cell_h + pad) + pad
        # Row label
        az_label = f"az {az:g}".rstrip("0").rstrip(".") if az != int(az) else f"az {int(az)}"
        draw.text((label_w // 2, y0 + cell_h // 2),
                  f"az {az:g}", fill=(180, 200, 220),
                  font=font_label, anchor="mm")
        for ci, el in enumerate(els):
            f = cells.get((az, el))
            x0 = label_w + pad + ci * (cell_w + pad)
            # cell border
            draw.rectangle((x0, y0, x0 + cell_w - 1, y0 + cell_h - 1),
                           outline=(40, 40, 50), width=1)
            if f is None:
                draw.text((x0 + cell_w // 2, y0 + cell_h // 2),
                          "—", fill=(80, 80, 90),
                          font=font_label, anchor="mm")
                continue
            im = Image.open(f).convert("RGBA")
            fitted = fit_into_cell(im, cell_w, cell_h)
            sheet.paste(fitted, (x0, y0), fitted)

    return sheet


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--src", type=Path, required=True,
                    help="directory containing per-angle sprite PNGs")
    ap.add_argument("--suffix", default="_clean.png",
                    help="filename suffix to match (default: _clean.png)")
    ap.add_argument("--out", type=Path, required=True,
                    help="output PNG path. With --rows-per-sheet, becomes a"
                         " template: foo.png -> foo_part1.png, foo_part2.png ...")
    ap.add_argument("--cell", type=int, default=200, help="cell size px")
    ap.add_argument("--title", default="", help="optional title at top of sheet")
    ap.add_argument("--rows-per-sheet", type=int, default=0,
                    help="chunk azimuths into multiple sheets of N rows each"
                         " (default: 0 = single sheet)")
    args = ap.parse_args()

    cells, azs, els = _index_sprite_dir(args.src, args.suffix)
    print(f"indexed: {len(azs)} azimuths × {len(els)} elevations")

    chunk_size = args.rows_per_sheet if args.rows_per_sheet > 0 else len(azs)
    chunks = [azs[i:i + chunk_size] for i in range(0, len(azs), chunk_size)]
    multi = len(chunks) > 1

    args.out.parent.mkdir(parents=True, exist_ok=True)
    for idx, chunk_azs in enumerate(chunks, 1):
        chunk_title = (
            f"{args.title} (part {idx}/{len(chunks)}: az "
            f"{chunk_azs[0]:g}–{chunk_azs[-1]:g})"
            if multi and args.title
            else (f"part {idx}/{len(chunks)}: az {chunk_azs[0]:g}–{chunk_azs[-1]:g}"
                  if multi else args.title)
        )
        sheet = build_contact_sheet(
            cells=cells, azs=chunk_azs, els=els,
            cell_w=args.cell, cell_h=args.cell,
            title=chunk_title,
        )
        if multi:
            out = args.out.with_name(f"{args.out.stem}_part{idx}{args.out.suffix}")
        else:
            out = args.out
        sheet.save(out, "PNG")
        print(f"wrote {out}  ({sheet.size[0]}x{sheet.size[1]})")


if __name__ == "__main__":
    main()

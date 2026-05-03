#!/usr/bin/env python3
"""
Crush AI-hallucinated bright-white pixels in ship sprite atlases.

The image model occasionally paints what it interprets as "cockpit
windows" or "hangar bay openings" as solid bright white (RGB > ~230).
On the textured Galaxy in particular, the mesh has actual recessed
geometry (a forward docking arm, side intake recesses) that should
render as deep dark cavities — but the model fills them with bright
specular highlights or near-pure white "glass". The result reads as
a hole punched in the sprite rather than tinted glass.

Postprocess fix: walk every `*_clean.png` for the ship, replace any
pixel where R, G, AND B all clear a threshold AND alpha > 0 with a
near-black value. Then rebuild the `*_clean_on_black.png` previews
so the F4 viewer / contact sheets pick up the change. Optionally
re-flip mirrors and re-center engine-facing cells so the change
propagates everywhere downstream.

Threshold and replacement colour are tuned for the Galaxy's gunmetal
hull (~150-200 grey), so genuine paint stays untouched.

Usage:
    # crush every frame in the galaxy atlas, sync mirrors + cells
    python3 tools/crush_bright_pixels.py --ship galaxy

    # only the front-quadrant frames at el 0
    python3 tools/crush_bright_pixels.py --ship galaxy \\
        --frames galaxy_az000_el+000 galaxy_az022p5_el+000

    # dry run — show what *would* be crushed without writing
    python3 tools/crush_bright_pixels.py --ship galaxy --dry-run

    # tighter threshold (only kill near-pure-white)
    python3 tools/crush_bright_pixels.py --ship galaxy --threshold 245
"""
from __future__ import annotations
import argparse
import shutil
import subprocess
import sys
from pathlib import Path

from PIL import Image

REPO = Path(__file__).resolve().parent.parent


def crush_one(src: Path, threshold: int, replacement: tuple[int, int, int],
              backup_suffix: str | None, dry_run: bool) -> tuple[int, int]:
    """Crush bright pixels in one *_clean.png. Returns (crushed, total_opaque)."""
    im = Image.open(src).convert("RGBA")
    px = im.load()
    W, H = im.size
    crushed = 0
    total_opaque = 0
    rep = (*replacement, 0)  # alpha filled in per-pixel below
    for y in range(H):
        for x in range(W):
            r, g, b, a = px[x, y]
            if a == 0:
                continue
            total_opaque += 1
            if r >= threshold and g >= threshold and b >= threshold:
                if not dry_run:
                    px[x, y] = (replacement[0], replacement[1], replacement[2], a)
                crushed += 1

    if crushed and not dry_run:
        if backup_suffix:
            bak = src.with_suffix(f".{backup_suffix}.png")
            if not bak.exists():           # don't clobber a previous backup
                shutil.copy(src, bak)
        im.save(src)
    return crushed, total_opaque


def rebuild_on_black(clean: Path) -> Path:
    """Re-derive *_clean_on_black.png from *_clean.png (clean RGBA composited
    onto solid black), so the F4 viewer / contact sheets pick up the fix."""
    dst = clean.with_name(clean.stem + "_on_black.png")
    fg = Image.open(clean).convert("RGBA")
    bg = Image.new("RGB", fg.size, (0, 0, 0))
    bg.paste(fg, mask=fg.split()[3])
    bg.save(dst)
    return dst


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Crush bright-white pixels in ship sprite atlases.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    ap.add_argument("--ship", required=True, help="ship name, e.g. 'galaxy'")
    ap.add_argument("--threshold", type=int, default=230,
                    help="all 3 RGB channels must be >= this to be crushed (default 230)")
    ap.add_argument("--replacement", type=int, nargs=3, default=[12, 12, 12],
                    metavar=("R", "G", "B"),
                    help="replacement colour (default near-black 12 12 12)")
    ap.add_argument("--frames", nargs="*", default=None,
                    help="optional list of frame stems to limit to (e.g. 'galaxy_az000_el+000'); "
                         "default: every *_clean.png in the ship's sprite dir")
    ap.add_argument("--no-backup", action="store_true",
                    help="skip writing .pre_crush.png backups (default: keep them)")
    ap.add_argument("--no-mirror", action="store_true",
                    help="skip mirror_symmetric_ship_frames after crushing")
    ap.add_argument("--no-cells", action="store_true",
                    help="skip center_ship_atlas_cells after crushing")
    ap.add_argument("--dry-run", action="store_true",
                    help="report counts but don't write any files")
    args = ap.parse_args()

    sprites = REPO / "assets" / "ships" / args.ship / "sprites"
    if not sprites.is_dir():
        print(f"[crush] no sprite dir: {sprites}", file=sys.stderr)
        return 2

    # Find candidate _clean.png files (the editable canonical layer).
    # We deliberately skip _clean_cell.png and _clean_on_black.png — those
    # are derived and get rebuilt below.
    all_clean = sorted(p for p in sprites.glob("*_clean.png")
                       if "_cell" not in p.name and "_on_black" not in p.name)
    if args.frames:
        wanted = set(args.frames)
        all_clean = [p for p in all_clean
                     if any(p.name.startswith(stem + "_") or p.stem == stem + "_clean"
                            for stem in wanted)]
        if not all_clean:
            print(f"[crush] no _clean.png matched --frames {args.frames}", file=sys.stderr)
            return 2

    backup_suffix = None if args.no_backup else "pre_crush"
    print(f"[crush] ship={args.ship}  files={len(all_clean)}  threshold={args.threshold}  "
          f"replacement=({args.replacement[0]},{args.replacement[1]},{args.replacement[2]})  "
          f"dry_run={args.dry_run}")

    touched: list[Path] = []
    total_crushed = 0
    for src in all_clean:
        crushed, opaque = crush_one(src, args.threshold, tuple(args.replacement),
                                    backup_suffix, args.dry_run)
        if crushed:
            pct = 100.0 * crushed / max(1, opaque)
            print(f"  {src.name}  crushed={crushed:>5}  ({pct:.2f}% of opaque)")
            touched.append(src)
            total_crushed += crushed

    print(f"[crush] {len(touched)}/{len(all_clean)} files touched, {total_crushed} pixels total")

    if args.dry_run or not touched:
        return 0

    # Rebuild the _on_black previews for every touched file so atlas viewer + sheets stay current.
    for src in touched:
        dst = rebuild_on_black(src)
        print(f"  rebuilt {dst.name}")

    if not args.no_mirror:
        print("[crush] mirror sync (primary _clean.png is now newer than its mirror)")
        subprocess.run([sys.executable, str(REPO / "tools" / "mirror_symmetric_ship_frames.py"),
                        "--ship", args.ship], check=True)

    if not args.no_cells:
        print("[crush] re-center cells")
        subprocess.run([sys.executable, str(REPO / "tools" / "center_ship_atlas_cells.py"),
                        "--ship", args.ship, "--preview"], check=True)

    print("[crush] done.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

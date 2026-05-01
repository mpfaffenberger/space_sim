#!/usr/bin/env python3
"""synthesize_mirrored_ship_sprites.py — fill mirrored ship atlas cells.

Privateer-style ship sprite atlases can author only one horizontal half of each
azimuth ring, then synthesize the opposite half by horizontally flipping PNGs.
For our current smooth 82-cell runtime grid (16 azimuths × five non-pole
rings + two poles), that means only 47 expensive AI generations are required:

    poles:                  2 authored  (el = -90, +90)
    non-pole rings: 5 × 9 = 45 authored (az 0..180 inclusive)
    runtime atlas:  2 + 5 × 16 = 82 cells after mirroring

This tool uses assets/ships/<ship>/renders/manifest.json as the authoritative
runtime target list. For any non-pole sample with az > 180, it looks for the
matching source at (360 - az, same elevation), mirrors the source sprite files,
and writes the target files under the expected batch_generate naming scheme.

It mirrors every variant that exists for the source stem:
    *_newmodel_512.png
    *_newmodel_512.raw.png
    *_newmodel_512_clean.png
    *_newmodel_512_clean_on_black.png

Run this after generating/cleaning the authored half, before
build_ship_atlas_manifest.py / center_ship_atlas_cells.py.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from PIL import Image

REPO = Path(__file__).resolve().parents[1]

VARIANT_SUFFIXES = (
    ".png",
    ".raw.png",
    "_clean.png",
    "_clean_on_black.png",
)


def sample_key(az: float, el: float) -> tuple[int, int]:
    """Integer-ish key in tenths of a degree, avoids float dict sadness."""
    return (int(round(az * 10.0)), int(round(el * 10.0)))


def is_pole(el: float) -> bool:
    return abs(abs(el) - 90.0) < 0.01


def mirror_png(src: Path, dst: Path, force: bool) -> bool:
    if dst.exists() and not force:
        return False
    dst.parent.mkdir(parents=True, exist_ok=True)
    im = Image.open(src).convert("RGBA")
    Image.Image.transpose(im, Image.Transpose.FLIP_LEFT_RIGHT).save(dst)
    return True


def synthesize(ship: str, force: bool, dry_run: bool) -> None:
    render_manifest = REPO / "assets" / "ships" / ship / "renders" / "manifest.json"
    sprite_dir = REPO / "assets" / "ships" / ship / "sprites"
    if not render_manifest.exists():
        raise SystemExit(f"render manifest not found: {render_manifest}")

    manifest = json.loads(render_manifest.read_text(encoding="utf-8"))
    samples = manifest.get("samples", [])
    by_angle: dict[tuple[int, int], dict] = {
        sample_key(float(s["az"]), float(s["el"])): s
        for s in samples
    }

    mirrored_cells = 0
    written_files = 0
    skipped_existing = 0
    missing_sources: list[str] = []

    for target in samples:
        target_az = float(target["az"])
        target_el = float(target["el"])
        if is_pole(target_el) or target_az <= 180.0:
            continue

        source_az = (360.0 - target_az) % 360.0
        source = by_angle.get(sample_key(source_az, target_el))
        if not source:
            missing_sources.append(f"az={target_az:g} el={target_el:g}: no manifest source az={source_az:g}")
            continue

        source_stem = Path(source["file"]).stem + "_newmodel_512"
        target_stem = Path(target["file"]).stem + "_newmodel_512"
        cell_touched = False

        for suffix in VARIANT_SUFFIXES:
            src = sprite_dir / f"{source_stem}{suffix}"
            dst = sprite_dir / f"{target_stem}{suffix}"
            if not src.exists():
                # Only _clean is mandatory for runtime. Other variants are nice-to-have.
                if suffix == "_clean.png":
                    missing_sources.append(f"{target_stem}: missing required source {src.name}")
                continue

            action = "WRITE" if force or not dst.exists() else "skip"
            print(f"{action:5s} {dst.name} <= mirror({src.name})")
            if action == "skip":
                skipped_existing += 1
                continue

            if not dry_run:
                mirror_png(src, dst, force=force)
            written_files += 1
            cell_touched = True

        if cell_touched:
            mirrored_cells += 1

    print()
    print(f"ship             : {ship}")
    print(f"target samples   : {len(samples)}")
    print(f"mirrored cells   : {mirrored_cells}")
    print(f"written files    : {written_files}")
    print(f"skipped existing : {skipped_existing}")
    if missing_sources:
        print(f"missing sources  : {len(missing_sources)}")
        for msg in missing_sources[:20]:
            print(f"  ! {msg}")
        if len(missing_sources) > 20:
            print(f"  ... {len(missing_sources) - 20} more")
        raise SystemExit(1)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--ship", required=True)
    ap.add_argument("--force", action="store_true", help="Overwrite existing mirrored target files")
    ap.add_argument("--dry-run", action="store_true", help="Print actions without writing PNGs")
    args = ap.parse_args()
    synthesize(args.ship, force=args.force, dry_run=args.dry_run)


if __name__ == "__main__":
    main()

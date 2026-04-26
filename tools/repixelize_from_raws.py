#!/usr/bin/env python3
"""repixelize_from_raws.py — locally re-run the pixelart downsample/quantize
step over previously-generated `.raw.png` outputs and re-emit the cleaned
sprite siblings, without hitting the OpenAI API again.

Use case: you tweaked the pixelart pipeline (e.g. swapped the palette
quantizer from MEDIANCUT to FASTOCTREE to stop nuking rare accent colors)
and want to re-process N raws without paying for re-generation.

Usage:
    # Re-pixelize every *_newmodel_512.raw.png under a sprites dir:
    python3 tools/repixelize_from_raws.py assets/ships/talon/sprites

    # Single file:
    python3 tools/repixelize_from_raws.py path/to/foo.raw.png

Idempotent: writes <stem>.png and <stem>_clean.png next to each raw.
"""
from __future__ import annotations

import argparse
import importlib.util
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image
from scipy import ndimage as ndi


REPO = Path(__file__).resolve().parent.parent
PIXELART_GENERATE_PATH = (
    "/Users/mpfaffenberger/.code_puppy/plugins/universal_constructor/"
    "pixelart/generate_sprite.py"
)
CLEAN_TOOL = REPO / "tools" / "clean_sprite_alpha.py"

# These mirror the values our batch tool passes for ship sprites.
PIXEL_GRID  = 512
PALETTE     = 40
RENDER_SIZE = 1024


def _key_out_white_flood(img: Image.Image, tolerance: int = 12) -> Image.Image:
    """Background-remove a white-bg sprite WITHOUT eating interior highlights.

    The UC plugin's `_key_out_white` treats every near-white pixel as
    background, which mistakenly kills specular highlights painted onto a
    metallic hull (Talon, Centurion, etc.) and punches black holes through
    the sprite when displayed on a dark background.

    Fix: only key out near-white pixels that are connected via 4-neighbour
    adjacency to the image border. Interior near-white islands (highlights)
    are left opaque.
    """
    arr = np.array(img.convert("RGBA"))
    r, g, b, a = arr[..., 0], arr[..., 1], arr[..., 2], arr[..., 3]
    thresh = 255 - tolerance
    near_white = (r >= thresh) & (g >= thresh) & (b >= thresh)

    # Label connected components of near-white pixels, then identify which
    # labels touch any of the four image borders. Those are background.
    labels, _ = ndi.label(near_white, structure=np.ones((3, 3), dtype=bool))
    border_labels = set()
    border_labels.update(np.unique(labels[0,  :]))
    border_labels.update(np.unique(labels[-1, :]))
    border_labels.update(np.unique(labels[:,  0]))
    border_labels.update(np.unique(labels[:, -1]))
    border_labels.discard(0)  # 0 = not near-white

    bg_mask = np.isin(labels, list(border_labels))
    arr[bg_mask] = (0, 0, 0, 0)
    return Image.fromarray(arr, mode="RGBA")


def _load_uc_helpers():
    """Pull `_crop_to_content` and `_pixelize` out of the (patched) UC plugin
    so we re-use the same code path the batch generator uses for those steps.
    The full pipeline matches `generate_sprite()` post-API except we swap in
    our smarter flood-fill keyer:
        raw -> RGBA -> _key_out_white_flood -> _crop_to_content -> _pixelize
    """
    spec = importlib.util.spec_from_file_location(
        "uc_pixelart_generate", PIXELART_GENERATE_PATH
    )
    if spec is None or spec.loader is None:
        raise RuntimeError(f"can't load pixelart plugin from {PIXELART_GENERATE_PATH}")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)  # type: ignore[union-attr]
    return mod._crop_to_content, mod._pixelize


def repixelize_one(
    raw_path: Path, crop_to_content, pixelize
) -> tuple[Path, Path]:
    """Repipe one .raw.png → .png (pixelized) → _clean.png (alpha-clean+crop)."""
    if not raw_path.name.endswith(".raw.png"):
        raise ValueError(f"expected a *.raw.png input, got {raw_path.name!r}")
    stem = raw_path.name[: -len(".raw.png")]
    png_path   = raw_path.with_name(stem + ".png")
    clean_path = raw_path.with_name(stem + "_clean.png")

    # Pipeline mirrors generate_sprite()'s post-API path, except the keyer is
    # our flood-fill version so interior hull highlights survive.
    working = Image.open(raw_path).convert("RGBA")
    working = _key_out_white_flood(working)
    working = crop_to_content(working, pad=2)
    out = pixelize(working, PIXEL_GRID, PALETTE, RENDER_SIZE)
    out.save(png_path, "PNG")

    subprocess.run(
        [sys.executable, str(CLEAN_TOOL), str(png_path), str(clean_path)],
        check=True,
        cwd=REPO,
        capture_output=True,
    )
    return png_path, clean_path


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("target", type=Path,
                    help="either a single *.raw.png file or a directory")
    args = ap.parse_args()

    crop_to_content, pixelize = _load_uc_helpers()

    raws: list[Path]
    if args.target.is_file():
        raws = [args.target]
    elif args.target.is_dir():
        raws = sorted(args.target.glob("*.raw.png"))
    else:
        raise SystemExit(f"not a file or dir: {args.target}")

    if not raws:
        raise SystemExit(f"no *.raw.png files under {args.target}")

    print(f"repixelizing {len(raws)} file(s) using FASTOCTREE quantization...")
    for i, raw in enumerate(raws, 1):
        png, clean = repixelize_one(raw, crop_to_content, pixelize)
        print(f"  [{i:>3}/{len(raws)}]  {raw.name} -> {clean.name}")
    print(f"done. {len(raws)} sprite(s) updated.")


if __name__ == "__main__":
    main()

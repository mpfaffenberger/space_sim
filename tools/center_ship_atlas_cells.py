#!/usr/bin/env python3
"""Pad ship sprite atlas frames into fixed-size transparent cells.

Why this exists: generated/cleaned sprites are tightly cropped per angle. That is
fine as standalone PNG hygiene, but terrible for a view-sphere atlas: every frame
gets a different billboard rectangle, so the ship appears to slide/scale while
scrubbing azimuth/elevation. This tool computes max width/height across the
atlas and writes *_cell.png variants with each source centered in that shared
transparent rectangle, then updates atlas_manifest.json to point at them.

Vertical flip note: the engine's sprite billboard pipeline expects ship atlas
frames stored upside-down on disk relative to how a human would view them in
an image viewer. (Long story: see the UV-mapping comment in src/sprite.cpp
and the screencapture-vs-framebuffer Y convention dance.) The reference PNGs
in renders/ and the *_clean.png outputs from the pixel-art generator are
stored upright — that's what the model produces best from, and it's what we
want to look at while debugging. Only the engine-facing cells need to be
flipped, so we do it here, once, in the place that exists specifically to
emit engine-ready artifacts. Pass --no-flip-y if you ever need raw cells.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from PIL import Image, ImageOps

REPO = Path(__file__).resolve().parents[1]


def asset_path(rel: str) -> Path:
    return REPO / "assets" / rel


def cell_path_for(sprite_rel: str) -> str:
    p = Path(sprite_rel)
    return str(p.with_name(p.stem + "_cell.png"))


def preview_path_for(cell_rel: str) -> str:
    p = Path(cell_rel)
    return str(p.with_name(p.stem + "_on_black.png"))


def source_sprite_rel(sample: dict) -> str:
    # Re-running should be idempotent: if manifest already points to cell files,
    # use the preserved uncropped source instead of nesting _cell_cell forever.
    return sample.get("sprite_uncropped") or sample["sprite"]


def rebuild_cells(ship: str, write_previews: bool, flip_y: bool) -> None:
    manifest_path = REPO / "assets" / "ships" / ship / "atlas_manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))

    loaded: list[tuple[dict, str, Image.Image]] = []
    for sample in manifest["samples"]:
        rel = source_sprite_rel(sample)
        img = Image.open(asset_path(rel)).convert("RGBA")
        loaded.append((sample, rel, img))

    max_w = max(img.width for _, _, img in loaded)
    max_h = max(img.height for _, _, img in loaded)

    for sample, rel, img in loaded:
        canvas = Image.new("RGBA", (max_w, max_h), (0, 0, 0, 0))
        canvas.alpha_composite(img, ((max_w - img.width) // 2, (max_h - img.height) // 2))

        # Preview is taken from the upright canvas (humans inspect previews,
        # so they should look like the reference image, not flipped). The
        # engine cell is then flipped in place if requested.
        preview_canvas = canvas
        if flip_y:
            canvas = ImageOps.flip(canvas)

        cell_rel = cell_path_for(rel)
        cell_abs = asset_path(cell_rel)
        cell_abs.parent.mkdir(parents=True, exist_ok=True)
        canvas.save(cell_abs)

        sample["sprite_uncropped"] = rel
        sample["sprite"] = cell_rel

        if write_previews:
            preview_rel = preview_path_for(cell_rel)
            black = Image.new("RGBA", (max_w, max_h), (0, 0, 0, 255))
            black.alpha_composite(preview_canvas)
            black.save(asset_path(preview_rel))
            if sample.get("preview") and "preview_uncropped" not in sample:
                sample["preview_uncropped"] = sample["preview"]
            sample["preview"] = preview_rel

    manifest["cell_width"] = max_w
    manifest["cell_height"] = max_h
    manifest["framing"] = "fixed transparent cell; each clean sprite centered in max width/height across atlas"
    manifest_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    print(f"{ship}: wrote {len(loaded)} cells at {max_w}x{max_h}")
    print(f"updated {manifest_path}")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--ship", default="tarsus")
    ap.add_argument("--preview", action="store_true", help="Also write *_cell_on_black.png previews")
    ap.add_argument("--no-flip-y", action="store_true",
                    help="Skip the vertical flip baked into ship cells (engine expects flipped on disk; default is to flip)")
    args = ap.parse_args()
    rebuild_cells(args.ship, args.preview, flip_y=not args.no_flip_y)


if __name__ == "__main__":
    main()

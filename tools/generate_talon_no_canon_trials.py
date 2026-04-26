#!/usr/bin/env python3
"""Generate a tiny Talon sprite trial batch WITHOUT the canonical reference.

This is deliberately an experiment runner, not the production atlas pipeline.
The current Talon atlas appears to have inherited pose/content from
canonical_reference.png, especially on camera-below cells where the model drew
cockpit/dorsal details that should be hidden. This script makes a few isolated
reference-only generations so we can answer one question cheaply:

    Does removing the secondary canonical image make bottom-sphere Talon cells
    respect the 3D mesh render pose better?

Outputs go to assets/ships/talon/regen_trials/no_canonical/ by default and are
ignored by git. If the results are good, promote/regenerate intentionally later;
do not silently overwrite production atlas cells from here. Future-you deserves
fewer jump scares.
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import sys
import time
from collections import deque
from dataclasses import dataclass
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

REPO = Path(__file__).resolve().parents[1]
PIXELART_GENERATE = REPO / "tools" / "pixelart" / "generate_sprite.py"
CLEANER = REPO / "tools" / "clean_sprite_alpha.py"

DEFAULT_SAMPLES = "0:-60,90:-60,180:-60,270:-60"

EXTRA_STYLE = (
    "sleek futuristic spaceship pixel art; glossy polished aerospace alloy "
    "hull (F-22 / B-2 / X-wing vibe) with paper-thin panel seams; clean and "
    "showroom-fresh; NEVER stone, brick, mortar, masonry, granite, sandstone, "
    "or cobblestones; preserve silhouette from the attached render exactly"
)


@dataclass(frozen=True)
class TrialSample:
    az: float
    el: float


def angle_tag(value: float) -> str:
    return f"{value:+04.0f}"


def az_tag(value: float) -> str:
    if abs(value - round(value)) < 0.01:
        return f"{int(round(value)):03d}"
    return f"{value:05.1f}".replace(".", "p")


def parse_samples(value: str) -> list[TrialSample]:
    samples: list[TrialSample] = []
    for chunk in value.split(","):
        chunk = chunk.strip()
        if not chunk:
            continue
        try:
            az_s, el_s = chunk.split(":", 1)
            samples.append(TrialSample(float(az_s), float(el_s)))
        except ValueError as exc:
            raise argparse.ArgumentTypeError(
                f"bad sample {chunk!r}; expected az:el, e.g. 90:-60"
            ) from exc
    if not samples:
        raise argparse.ArgumentTypeError("at least one sample is required")
    return samples


def load_generate_sprite():
    spec = importlib.util.spec_from_file_location("_pixelart_generate_sprite", PIXELART_GENERATE)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Could not load {PIXELART_GENERATE}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.generate_sprite


def load_cleaner():
    spec = importlib.util.spec_from_file_location("_clean_sprite_alpha", CLEANER)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Could not load {CLEANER}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.clean_alpha, module.write_preview


def render_path(sample: TrialSample) -> Path:
    return REPO / "assets" / "ships" / "talon" / "renders" / (
        f"talon_az{az_tag(sample.az)}_el{angle_tag(sample.el)}.png"
    )


def stem_for(sample: TrialSample, reference_mode: str) -> str:
    suffix = "no_canon_trial" if reference_mode == "render" else f"no_canon_{reference_mode}_trial"
    return f"talon_az{az_tag(sample.az)}_el{angle_tag(sample.el)}_{suffix}_512"


def largest_bright_component_mask(im: Image.Image) -> Image.Image:
    """Extract the ship from a starfield render using the largest bright blob.

    Renders are opaque screenshots, not clean alpha PNGs. Thresholding grabs
    stars too, so we keep only the largest 8-connected component. It is boring,
    deterministic, dependency-free, and good enough for these capture crops.
    """
    rgb = im.convert("RGB")
    w, h = rgb.size
    px = rgb.load()
    candidate = [[False] * w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]
            # Ship hull is medium-bright over dark space; stars are tiny and
            # disconnected. Keep saturated red/purple accents too.
            if max(r, g, b) > 42 and (r + g + b) > 95:
                candidate[y][x] = True

    seen = [[False] * w for _ in range(h)]
    best: list[tuple[int, int]] = []
    dirs = [(-1, -1), (0, -1), (1, -1), (-1, 0), (1, 0), (-1, 1), (0, 1), (1, 1)]
    for sy in range(h):
        for sx in range(w):
            if not candidate[sy][sx] or seen[sy][sx]:
                continue
            comp: list[tuple[int, int]] = []
            q: deque[tuple[int, int]] = deque([(sx, sy)])
            seen[sy][sx] = True
            while q:
                x, y = q.popleft()
                comp.append((x, y))
                for dx, dy in dirs:
                    nx, ny = x + dx, y + dy
                    if 0 <= nx < w and 0 <= ny < h and candidate[ny][nx] and not seen[ny][nx]:
                        seen[ny][nx] = True
                        q.append((nx, ny))
            if len(comp) > len(best):
                best = comp

    mask = Image.new("L", (w, h), 0)
    mp = mask.load()
    for x, y in best:
        mp[x, y] = 255
    # Close tiny threshold gaps and add a small antialiased-ish edge.
    return mask.filter(ImageFilter.MaxFilter(5)).filter(ImageFilter.MinFilter(3)).filter(ImageFilter.GaussianBlur(0.75))


def make_flat_reference(src: Path, dst: Path) -> Path:
    """Write a single-reference de-textured render for generation.

    It keeps silhouette + broad lighting but nukes high-frequency brick/block
    texture. This is still "without additional reference image": one derived
    primary reference, no canonical sheet. The model should have less masonry
    to copy while preserving the bottom-view pose.
    """
    im = Image.open(src).convert("RGBA")
    mask = largest_bright_component_mask(im)
    blurred = im.filter(ImageFilter.GaussianBlur(6.0)).convert("RGBA")

    # Nudge toward clean white/silver metal while retaining broad light/shadow.
    out = Image.new("RGBA", im.size, (0, 0, 0, 0))
    bp = blurred.load()
    op = out.load()
    mp = mask.load()
    for y in range(im.height):
        for x in range(im.width):
            a = mp[x, y]
            if a == 0:
                continue
            r, g, b, _ = bp[x, y]
            lum = int((r * 0.299 + g * 0.587 + b * 0.114))
            metal = max(70, min(235, int(lum * 1.2 + 45)))
            # Slight cool tint; broad shading only, no masonry edges.
            op[x, y] = (metal, min(245, metal + 3), min(255, metal + 10), a)

    # Add a readable dark silhouette outline from the mask boundary.
    dilated = mask.filter(ImageFilter.MaxFilter(7))
    outline = Image.new("RGBA", im.size, (0, 0, 0, 0))
    dp = dilated.load()
    mp = mask.load()
    opx = outline.load()
    for y in range(im.height):
        for x in range(im.width):
            if dp[x, y] and not mp[x, y]:
                opx[x, y] = (25, 28, 34, min(210, dp[x, y]))
    outline.alpha_composite(out)

    # Put it on transparent background; image-edit accepts PNG alpha fine.
    dst.parent.mkdir(parents=True, exist_ok=True)
    outline.save(dst, "PNG")
    return dst


def build_prompt(sample: TrialSample, reference: Path) -> str:
    below_note = ""
    if sample.el < 0:
        below_note = """
CAMERA POSITION WARNING — THIS IS THE MOST IMPORTANT PART:
- The camera is BELOW the ship (negative elevation).
- This must be a ventral / belly view, matching the attached 3D render.
- Do NOT rotate the ship into a top-down beauty shot.
- Do NOT draw the black cockpit canopy as a large visible top feature.
- Dorsal-only details should be hidden unless the reference shows a thin edge.
"""
    elif sample.el > 0:
        below_note = """
CAMERA POSITION WARNING:
- The camera is ABOVE the ship (positive elevation).
- This should show dorsal/top features such as the cockpit canopy where visible.
- Match the attached render's pose exactly; do not invent a different angle.
"""

    return f"""Create a pixel-art spaceship sprite using ONLY the attached 3D render as the reference image.

REFERENCE IMAGE:
{reference}

No canonical design sheet is attached for this trial. Do not borrow pose,
orientation, or content from any other image. The attached render is the sole
source of truth for silhouette, viewing angle, and which ship surfaces are
visible.

Subject: Talon-class light pirate fighter from Wing Commander: Privateer.

{below_note}
Requirements:
- Preserve the silhouette, proportions, and exact viewing angle from the render.
- Preserve the outline exactly; do not simplify, blur, or redraw a different ship.
- Distinguishing features: flat wedge-shaped main hull tapering to a pointed
  nose, four short swept fins splayed outward, narrow rear engine block.
- Repaint the ugly placeholder block/stone texture from the render into sleek
  glossy aerospace metal.
- White-silver hull, sparse blue spine/accent stripe only where that surface is
  actually visible from this camera angle, red engine-bell accents only where
  visible.
- Center the ship with transparent/flat keyed background, readable sprite first.

Style:
- 1993 DOS / Wing Commander: Privateer style pixel art
- chunky pixels, hard readable silhouette, dithered shading, limited palette
- paper-thin panel seams, not brick/mortar grooves
- crisp specular highlights along wings and curved hull

Avoid:
- top-down canopy on camera-below frames
- copying any canonical beauty-shot orientation
- stone walls, brickwork, masonry, cobblestones, castle textures
- painterly blur, mushy outlines, extra fins/greebles, starfield background
"""


def black_preview(path: Path, size: int = 220) -> Image.Image:
    if not path.exists():
        img = Image.new("RGBA", (size, size), (45, 0, 0, 255))
        ImageDraw.Draw(img).text((8, 8), "MISSING", fill=(255, 100, 100, 255))
        return img
    im = Image.open(path).convert("RGBA")
    bg = Image.new("RGBA", im.size, (0, 0, 0, 255))
    bg.alpha_composite(im)
    bg.thumbnail((size, size), Image.Resampling.LANCZOS)
    canvas = Image.new("RGBA", (size, size), (16, 16, 20, 255))
    canvas.alpha_composite(bg, ((size - bg.width) // 2, (size - bg.height) // 2))
    return canvas


def build_contact_sheet(samples: list[TrialSample], outdir: Path, outpath: Path) -> None:
    cell = 220
    label = 24
    rows = [("mesh render", None), ("current atlas", "current"), ("no-canon trial", "trial")]
    sheet = Image.new("RGBA", (cell * len(samples), (cell + label) * len(rows) + 30), (16, 16, 20, 255))
    draw = ImageDraw.Draw(sheet)
    draw.text((8, 6), "Talon no-canonical-reference trials — render vs current vs trial", fill=(230, 230, 230, 255))

    for col, sample in enumerate(samples):
        x = col * cell
        draw.text((x + 6, 24), f"az {az_tag(sample.az)} el {angle_tag(sample.el)}", fill=(180, 255, 180, 255))
        for row, (label_text, kind) in enumerate(rows):
            y = 30 + row * (cell + label)
            draw.text((x + 6, y), label_text, fill=(230, 210, 180, 255))
            if kind is None:
                img = Image.open(render_path(sample)).convert("RGBA")
                img.thumbnail((cell, cell), Image.Resampling.LANCZOS)
                canvas = Image.new("RGBA", (cell, cell), (16, 16, 20, 255))
                canvas.alpha_composite(img, ((cell - img.width) // 2, (cell - img.height) // 2))
            elif kind == "current":
                current = REPO / "assets" / "ships" / "talon" / "sprites" / (
                    f"talon_az{az_tag(sample.az)}_el{angle_tag(sample.el)}_newmodel_512_clean.png"
                )
                canvas = black_preview(current, cell)
            else:
                # Prefer any trial variant for this sample; contact-sheet calls
                # are usually run after a single mode. If multiple exist, newest
                # one wins by mtime. Good enough for scratch experiments.
                matches = sorted(outdir.glob(
                    f"talon_az{az_tag(sample.az)}_el{angle_tag(sample.el)}_*_trial_512_clean.png"
                ), key=lambda p: p.stat().st_mtime if p.exists() else 0)
                canvas = black_preview(matches[-1] if matches else outdir / f"{stem_for(sample, 'render')}_clean.png", cell)
            sheet.alpha_composite(canvas, (x, y + label))

    outpath.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(outpath)
    print(f"contact sheet: {outpath}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--samples", type=parse_samples, default=parse_samples(DEFAULT_SAMPLES),
                        help="Comma-separated az:el list, e.g. '0:-60,90:-60'")
    parser.add_argument("--outdir", type=Path,
                        default=REPO / "assets" / "ships" / "talon" / "regen_trials" / "no_canonical")
    parser.add_argument("--quality", default="high", choices=["low", "medium", "high", "auto"])
    parser.add_argument("--reference-mode", default="render", choices=["render", "flat"],
                        help="render = original mesh render; flat = derived de-textured render (still one reference, no canonical)")
    parser.add_argument("--force", action="store_true", help="Regenerate even if trial PNG exists")
    parser.add_argument("--dry-run", action="store_true", help="Write prompts/contact sheet only; no API calls")
    parser.add_argument("--timeout", type=float, default=240.0)
    args = parser.parse_args()

    args.outdir.mkdir(parents=True, exist_ok=True)
    clean_alpha, write_preview = load_cleaner()
    generate_sprite = None if args.dry_run else load_generate_sprite()

    print(f"samples: {len(args.samples)}")
    print(f"outdir : {args.outdir}")
    print(f"refs   : {args.reference_mode} primary reference only (NO canonical_reference.png)")

    for idx, sample in enumerate(args.samples, 1):
        source_ref = render_path(sample)
        if not source_ref.exists():
            raise FileNotFoundError(source_ref)
        ref = source_ref
        if args.reference_mode == "flat":
            ref = make_flat_reference(
                source_ref,
                args.outdir / "refs" / f"talon_az{az_tag(sample.az)}_el{angle_tag(sample.el)}_flat_ref.png",
            )

        stem = stem_for(sample, args.reference_mode)
        out = args.outdir / f"{stem}.png"
        clean = args.outdir / f"{stem}_clean.png"
        prompt_file = args.outdir / "prompts" / f"{stem}.txt"
        prompt_file.parent.mkdir(parents=True, exist_ok=True)
        prompt = build_prompt(sample, ref)
        prompt_file.write_text(prompt, encoding="utf-8")

        print(f"\n[{idx}/{len(args.samples)}] az={sample.az:g} el={sample.el:g}")
        print(f"  ref   : {ref.relative_to(REPO)}")
        print(f"  prompt: {prompt_file.relative_to(REPO)}")

        if args.dry_run:
            print("  dry-run: skip API")
            continue
        if out.exists() and not args.force:
            print(f"  skip existing: {out.relative_to(REPO)}")
        else:
            t0 = time.monotonic()
            result = generate_sprite(
                subject=prompt,
                output_path=str(out),
                view="side",
                pixel_grid=512,
                palette_size=40,
                render_size=1024,
                transparent_bg=False,
                size="1024x1024",
                quality=args.quality,
                extra_style=EXTRA_STYLE,
                save_raw=True,
                reference_image=[str(ref)],
                reference_strength="strict",
                timeout=args.timeout,
            )
            print("  ok    : " + json.dumps({
                "elapsed": round(time.monotonic() - t0, 1),
                "dims": result.get("final_dimensions"),
                "endpoint": result.get("used_endpoint"),
            }))

        if out.exists():
            bbox = clean_alpha(out, clean)
            preview = write_preview(clean)
            print(f"  clean : {clean.relative_to(REPO)} bbox={bbox}")
            print(f"  prev  : {preview.relative_to(REPO)}")

    build_contact_sheet(args.samples, args.outdir, Path("/tmp/diag/talon_no_canon_trials.png"))


if __name__ == "__main__":
    main()

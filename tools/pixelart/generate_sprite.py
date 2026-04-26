TOOL_META = {
    "name": "generate_sprite",
    "namespace": "pixelart",
    "description": "Generate a pixel art sprite using OpenAI's gpt-image-2 model, then crisp-pixelize it with PIL. Produces transparent PNG sprites suitable for games. Supports view angles (side, front, 3/4, top-down), palette quantization, custom pixel grid sizes, and optional reference image(s) for visual guidance via the /v1/images/edits endpoint.",
    "enabled": True,
    "version": "1.1.1",
    "author": "user",
    "created_at": "2026-04-18T20:27:15.746389",
}

"""Pixel art sprite generator using OpenAI's image API + PIL post-processing.

Generates sprites with transparent backgrounds, then applies a crisp
nearest-neighbor pixelization pass so output looks like authentic pixel art
rather than "AI-painted pixel art".
"""
import base64
import io
import mimetypes
import os
import time
from pathlib import Path
from typing import Any, Dict, List, Optional, Union

import httpx
from PIL import Image


OPENAI_IMAGES_URL = "https://api.openai.com/v1/images/generations"
OPENAI_IMAGES_EDIT_URL = "https://api.openai.com/v1/images/edits"

_ALLOWED_SIZES = {"1024x1024", "1024x1536", "1536x1024", "auto"}

_VIEW_FRAGMENTS = {
    "side": "full side-view profile, character facing right, flat silhouette",
    "front": "front-facing view, character centered and symmetric",
    "back": "back-facing view, character centered",
    "three-quarter": "classic JRPG three-quarter view, slight top-down angle",
    "3/4": "classic JRPG three-quarter view, slight top-down angle",
    "top-down": "top-down view as seen from directly above (Zelda-style)",
    "isometric": "isometric 2:1 projection, clean diagonal edges",
}


def _enhance_prompt(
    subject, view, palette_size, pixel_grid, transparent_bg, extra_style
):
    view_frag = _VIEW_FRAGMENTS.get(view.lower(), _VIEW_FRAGMENTS["side"])
    bg_frag = (
        "pure transparent background, no shadow, no ground, no scenery"
        if transparent_bg
        else "solid flat pure-white background (will be keyed out), no shadows on ground"
    )
    style_extra = f" Additional style notes: {extra_style}." if extra_style else ""
    return (
        f"A single pixel art sprite of: {subject}. "
        f"{view_frag}. "
        f"Rendered as authentic {pixel_grid}x{pixel_grid} pixel art with hard, "
        f"chunky pixels, crisp 1-pixel outlines, NO anti-aliasing, NO gradients, "
        f"NO blur, NO motion lines. "
        f"Limited palette of roughly {palette_size} carefully chosen colors with "
        f"clear cel-shading (flat lit/shadow regions). "
        f"Centered in frame with generous margin on all sides. "
        f"{bg_frag}. "
        f"Style: classic 16-bit era game sprite, SNES/Genesis aesthetic, "
        f"clean readable silhouette.{style_extra}"
    )


def _call_openai_image(prompt, size, api_key, transparent_bg, quality, timeout):
    payload: Dict[str, Any] = {
        "model": "gpt-image-2",
        "prompt": prompt,
        "size": size,
        "n": 1,
        "quality": quality,
    }
    if transparent_bg:
        payload["background"] = "transparent"

    headers = {
        "Authorization": f"Bearer {api_key}",
        "Content-Type": "application/json",
    }

    with httpx.Client(timeout=timeout) as client:
        resp = client.post(OPENAI_IMAGES_URL, json=payload, headers=headers)
        if resp.status_code != 200:
            raise RuntimeError(
                f"OpenAI image API error {resp.status_code}: {resp.text[:500]}"
            )
        data = resp.json()

    try:
        b64 = data["data"][0]["b64_json"]
    except (KeyError, IndexError) as e:
        raise RuntimeError(f"Unexpected API response shape: {data}") from e

    return base64.b64decode(b64)


def _normalize_reference_paths(reference_image):
    if reference_image is None:
        return []
    if isinstance(reference_image, (str, Path)):
        paths = [reference_image]
    else:
        paths = list(reference_image)
    resolved = []
    for p in paths:
        rp = Path(str(p)).expanduser().resolve()
        if not rp.is_file():
            raise FileNotFoundError(f"Reference image not found: {rp}")
        resolved.append(rp)
    if not resolved:
        raise ValueError("reference_image was provided but resolved to no files")
    if len(resolved) > 16:
        raise ValueError("At most 16 reference images are supported")
    return resolved


def _call_openai_image_edit(
    prompt, reference_paths, size, api_key, transparent_bg, quality, timeout
):
    data: Dict[str, Any] = {
        "model": "gpt-image-2",
        "prompt": prompt,
        "n": "1",
        "quality": quality,
    }
    if size != "auto":
        data["size"] = size
    if transparent_bg:
        data["background"] = "transparent"

    files = []
    open_handles = []
    try:
        for rp in reference_paths:
            mime, _ = mimetypes.guess_type(str(rp))
            if mime is None:
                mime = "image/png"
            fh = open(rp, "rb")
            open_handles.append(fh)
            field_name = "image[]" if len(reference_paths) > 1 else "image"
            files.append((field_name, (rp.name, fh, mime)))

        headers = {"Authorization": f"Bearer {api_key}"}
        with httpx.Client(timeout=timeout) as client:
            resp = client.post(
                OPENAI_IMAGES_EDIT_URL, data=data, files=files, headers=headers
            )
            if resp.status_code != 200:
                raise RuntimeError(
                    f"OpenAI image edit API error {resp.status_code}: {resp.text[:500]}"
                )
            payload = resp.json()
    finally:
        for fh in open_handles:
            try:
                fh.close()
            except Exception:
                pass

    try:
        b64 = payload["data"][0]["b64_json"]
    except (KeyError, IndexError) as e:
        raise RuntimeError(f"Unexpected API response shape: {payload}") from e

    return base64.b64decode(b64)


def _key_out_white(img, tolerance=12):
    img = img.convert("RGBA")
    pixels = img.load()
    w, h = img.size
    thresh = 255 - tolerance
    for y in range(h):
        for x in range(w):
            r, g, b, a = pixels[x, y]
            if r >= thresh and g >= thresh and b >= thresh:
                pixels[x, y] = (0, 0, 0, 0)
    return img


def _crop_to_content(img, pad=2):
    if img.mode != "RGBA":
        return img
    bbox = img.getbbox()
    if not bbox:
        return img
    left, top, right, bottom = bbox
    w, h = img.size
    if (left, top, right, bottom) == (0, 0, w, h):
        return img
    left = max(0, left - pad)
    top = max(0, top - pad)
    right = min(w, right + pad)
    bottom = min(h, bottom + pad)
    return img.crop((left, top, right, bottom))


def _pixelize(img, pixel_grid, palette_size, render_size):
    img = img.convert("RGBA")

    w, h = img.size
    if w >= h:
        new_w = pixel_grid
        new_h = max(1, round(h * pixel_grid / w))
    else:
        new_h = pixel_grid
        new_w = max(1, round(w * pixel_grid / h))

    small = img.resize((new_w, new_h), Image.Resampling.BOX)

    if palette_size and palette_size > 0:
        alpha = small.split()[-1]
        # FASTOCTREE > MEDIANCUT for sprite art: MEDIANCUT is frequency-based
        # and discards rare-but-meaningful colors (small accent stripes,
        # canopy highlights, navigation lights) by bucketing them into the
        # majority gray. FASTOCTREE divides by color-space region, preserving
        # rare-but-saturated clusters. Verified A/B on talon sprites: vivid
        # blue/red accents survive at 64-color palette under FASTOCTREE while
        # MEDIANCUT zeroes them out.
        rgb = (
            small.convert("RGB")
            .quantize(
                colors=palette_size,
                method=Image.Quantize.FASTOCTREE,
                dither=Image.Dither.NONE,
            )
            .convert("RGB")
        )
        small = Image.merge("RGBA", (*rgb.split(), alpha))
        a = small.split()[-1].point(lambda v: 255 if v >= 128 else 0)
        small.putalpha(a)

    sw, sh = small.size
    if sw >= sh:
        up_w = render_size
        up_h = max(1, round(sh * render_size / sw))
    else:
        up_h = render_size
        up_w = max(1, round(sw * render_size / sh))
    return small.resize((up_w, up_h), Image.Resampling.NEAREST)


def generate_sprite(
    subject: str,
    output_path: str = "sprite.png",
    view: str = "side",
    pixel_grid: int = 64,
    palette_size: int = 16,
    render_size: int = 512,
    transparent_bg: bool = True,
    size: str = "1024x1024",
    quality: str = "medium",
    extra_style: Optional[str] = None,
    save_raw: bool = False,
    api_key: Optional[str] = None,
    timeout: float = 180.0,
    reference_image: Optional[Union[str, List[str]]] = None,
    reference_strength: str = "strong",
) -> Dict[str, Any]:
    """Generate a pixel art sprite.

    Args:
        subject: What to draw ("a brave knight with a red cape and silver sword").
        output_path: Where to save the final pixelized PNG.
        view: One of "side", "front", "back", "three-quarter" / "3/4",
              "top-down", "isometric".
        pixel_grid: Target pixel grid resolution (longest side). 32 = chunky,
                    64 = SNES-ish, 128 = detailed. Default 64.
        palette_size: Number of colors after quantization. 0 = no quantization.
                      8 = NES-ish, 16 = SNES-ish, 32 = modern pixel art.
        render_size: Upscaled output size for the longest side (for viewing).
        transparent_bg: Request transparent background from the model.
        size: Native generation size. One of "1024x1024", "1024x1536",
              "1536x1024", or "auto".
        quality: "low", "medium", "high", or "auto".
        extra_style: Optional extra style directives to inject into the prompt.
        save_raw: If True, also save the raw AI image alongside (as .raw.png).
        api_key: OpenAI API key. Defaults to $OPENAI_API_KEY.
        timeout: HTTP timeout in seconds for the image API call.
        reference_image: Optional path (or list of paths, up to 16) to local
            image file(s) used as visual reference. When provided, the call is
            routed through OpenAI's /v1/images/edits endpoint with gpt-image-2,
            which uses the reference(s) to guide design, palette, and silhouette.
            Accepts PNG / JPEG / WEBP. Each file should be < 25 MB.
        reference_strength: How strictly to follow the reference. One of
            "loose" (treat as inspiration only), "strong" (default — match
            silhouette, pose, and palette), or "strict" (preserve exact
            composition; only re-render in pixel art style).

    Returns:
        Dict with output_path, raw_path, prompt, final_dimensions,
        pixel_grid_dimensions, reference_images, and elapsed_seconds.
    """
    t0 = time.monotonic()

    if not subject or not subject.strip():
        raise ValueError("subject must be a non-empty description")

    if size not in _ALLOWED_SIZES:
        raise ValueError(f"size must be one of {sorted(_ALLOWED_SIZES)}, got {size!r}")

    if quality not in {"low", "medium", "high", "auto"}:
        raise ValueError(
            f"quality must be 'low', 'medium', 'high', or 'auto', got {quality!r}"
        )

    if pixel_grid < 8 or pixel_grid > 512:
        raise ValueError("pixel_grid must be between 8 and 512")

    if palette_size < 0 or palette_size > 256:
        raise ValueError("palette_size must be between 0 and 256 (0 = disable)")

    if render_size < pixel_grid:
        raise ValueError(
            f"render_size ({render_size}) must be >= pixel_grid ({pixel_grid})"
        )

    key = api_key or os.environ.get("OPENAI_API_KEY")
    if not key:
        raise RuntimeError(
            "No OpenAI API key found. Set OPENAI_API_KEY or pass api_key=..."
        )

    if reference_strength not in {"loose", "strong", "strict"}:
        raise ValueError(
            f"reference_strength must be 'loose', 'strong', or 'strict', got {reference_strength!r}"
        )

    reference_paths = _normalize_reference_paths(reference_image)

    extra_with_ref = extra_style
    if reference_paths:
        ref_directives = {
            "loose": (
                "A reference image is provided for loose visual inspiration only — "
                "borrow general vibe, palette hints, and subject feel, but feel free "
                "to reinterpret pose, composition, and details freely."
            ),
            "strong": (
                "A reference image is provided. MATCH its silhouette, pose, character "
                "design, color palette, and key visual features closely, but re-render "
                "the result fully in the requested chunky pixel art style."
            ),
            "strict": (
                "A reference image is provided. PRESERVE its exact composition, pose, "
                "proportions, and color choices as faithfully as possible. Only change "
                "the rendering style: convert to crisp, hard-edged pixel art with the "
                "requested grid resolution and limited palette."
            ),
        }[reference_strength]
        extra_with_ref = (
            f"{extra_style}. {ref_directives}" if extra_style else ref_directives
        )

    prompt = _enhance_prompt(
        subject=subject.strip(),
        view=view,
        palette_size=palette_size if palette_size > 0 else 16,
        pixel_grid=pixel_grid,
        transparent_bg=transparent_bg,
        extra_style=extra_with_ref,
    )

    if reference_paths:
        png_bytes = _call_openai_image_edit(
            prompt=prompt,
            reference_paths=reference_paths,
            size=size,
            api_key=key,
            transparent_bg=transparent_bg,
            quality=quality,
            timeout=timeout,
        )
    else:
        png_bytes = _call_openai_image(
            prompt=prompt,
            size=size,
            api_key=key,
            transparent_bg=transparent_bg,
            quality=quality,
            timeout=timeout,
        )

    raw_img = Image.open(io.BytesIO(png_bytes))

    out_path = Path(output_path).expanduser().resolve()
    out_path.parent.mkdir(parents=True, exist_ok=True)

    raw_path = None
    if save_raw:
        raw_path = Path(str(out_path.with_suffix("")) + ".raw.png")
        raw_img.save(raw_path, "PNG")

    working = raw_img.convert("RGBA")
    if not transparent_bg:
        working = _key_out_white(working)
    working = _crop_to_content(working, pad=2)

    final = _pixelize(working, pixel_grid, palette_size, render_size)
    final.save(out_path, "PNG")

    return {
        "output_path": str(out_path),
        "raw_path": str(raw_path) if raw_path else None,
        "prompt": prompt,
        "final_dimensions": list(final.size),
        "pixel_grid_dimensions": [
            max(1, round(final.size[0] * pixel_grid / max(final.size))),
            max(1, round(final.size[1] * pixel_grid / max(final.size))),
        ],
        "reference_images": [str(p) for p in reference_paths] or None,
        "reference_strength": reference_strength if reference_paths else None,
        "used_endpoint": "edits" if reference_paths else "generations",
        "elapsed_seconds": round(time.monotonic() - t0, 2),
    }

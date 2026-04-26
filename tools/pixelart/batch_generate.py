TOOL_META = {
    "name": "batch_generate",
    "namespace": "pixelart",
    "description": "Batch-generate multiple pixel art sprites from a list of specs. Wraps pixelart.generate_sprite, skips already-existing files (resume-safe), supports reference images, logs progress to a file, and returns a manifest.",
    "enabled": True,
    "version": "1.1.0",
    "author": "helios",
}

"""Batch sprite generator.

Feed it a list of specs like:
    [{"filename": "kitten.png", "subject": "a tiny kitten ...", "view": "front"}, ...]

It will:
  - Skip files that already exist in output_dir (resume-friendly)
  - Generate the rest sequentially
  - Log progress to a log file (tail it to watch!)
  - Return a manifest with successes, skips, failures
"""
import importlib.util
import json
import sys
import time
from pathlib import Path
from typing import Any, Dict, List, Optional


_SPRITE_TOOL_PATH = Path(
    "/Users/mpfaffenberger/.code_puppy/plugins/universal_constructor/pixelart/generate_sprite.py"
)


def _load_generate_sprite():
    """Dynamically load the generate_sprite function from its plugin file."""
    spec = importlib.util.spec_from_file_location(
        "_pixelart_generate_sprite", _SPRITE_TOOL_PATH
    )
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Could not load sprite tool at {_SPRITE_TOOL_PATH}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.generate_sprite


def batch_generate(
    specs: List[Dict[str, Any]],
    output_dir: str = "pixel_art",
    log_path: Optional[str] = None,
    skip_existing: bool = True,
    default_view: str = "front",
    default_pixel_grid: int = 64,
    default_palette_size: int = 16,
    default_quality: str = "medium",
    default_extra_style: Optional[
        str
    ] = "kawaii, cuddly, clean pixel outlines, vibrant palette",
    stop_on_error: bool = False,
) -> Dict[str, Any]:
    """Generate many sprites in one call.

    Args:
        specs: List of dicts, each with:
            - filename (str, required): output filename (e.g. "kitten.png")
            - subject (str, required): what to draw
            - view (str, optional): override default_view
            - pixel_grid (int, optional): override default_pixel_grid
            - palette_size (int, optional): override default_palette_size
            - extra_style (str, optional): override default_extra_style
            - reference_image (str | list[str], optional): local reference image(s)
            - reference_strength (str, optional): loose | strong | strict
            - render_size (int, optional): final upscaled longest side
            - size (str, optional): native image model canvas
            - quality (str, optional): low | medium | high | auto
            - save_raw (bool, optional): save raw AI image alongside output
        output_dir: Directory to write sprites into. Created if missing.
        log_path: Path to progress log. Defaults to <output_dir>/batch_log.txt.
        skip_existing: If True, don't regenerate files that already exist.
        default_view: Default view if spec doesn't specify.
        default_pixel_grid: Default pixel grid.
        default_palette_size: Default palette size.
        default_quality: "low", "medium", "high", or "auto".
        default_extra_style: Default style modifier string.
        stop_on_error: If True, abort the batch on first failure.

    Returns:
        Manifest dict with:
          - total, generated, skipped, failed counts
          - results: list of per-spec result dicts
          - log_path: where progress was logged
          - elapsed_seconds: total wall-clock time
    """
    generate_sprite = _load_generate_sprite()

    out_dir = Path(output_dir).expanduser().resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    if log_path is None:
        log_path = str(out_dir / "batch_log.txt")
    log_file = Path(log_path)
    log_file.parent.mkdir(parents=True, exist_ok=True)

    def log(msg: str) -> None:
        line = f"[{time.strftime('%H:%M:%S')}] {msg}"
        print(line, flush=True)
        with log_file.open("a") as f:
            f.write(line + "\n")

    log(f"=== Batch starting: {len(specs)} specs -> {out_dir} ===")

    results: List[Dict[str, Any]] = []
    generated = skipped = failed = 0
    t_start = time.time()

    for idx, spec in enumerate(specs, 1):
        filename = spec.get("filename")
        subject = spec.get("subject")
        if not filename or not subject:
            failed += 1
            results.append(
                {
                    "index": idx,
                    "status": "invalid_spec",
                    "error": "Missing filename or subject",
                    "spec": spec,
                }
            )
            log(f"[{idx}/{len(specs)}] INVALID SPEC: {spec}")
            if stop_on_error:
                break
            continue

        out_path = out_dir / filename
        prefix = f"[{idx}/{len(specs)}] {filename}"

        if skip_existing and out_path.exists():
            skipped += 1
            results.append(
                {
                    "index": idx,
                    "status": "skipped",
                    "filename": filename,
                    "output_path": str(out_path),
                }
            )
            log(f"{prefix} SKIP (exists)")
            continue

        log(f"{prefix} generating: {subject[:70]}...")
        try:
            r = generate_sprite(
                subject=subject,
                output_path=str(out_path),
                view=spec.get("view", default_view),
                pixel_grid=spec.get("pixel_grid", default_pixel_grid),
                palette_size=spec.get("palette_size", default_palette_size),
                render_size=spec.get("render_size", max(512, spec.get("pixel_grid", default_pixel_grid))),
                transparent_bg=spec.get("transparent_bg", True),
                size=spec.get("size", "1024x1024"),
                quality=spec.get("quality", default_quality),
                extra_style=spec.get("extra_style", default_extra_style),
                save_raw=spec.get("save_raw", False),
                reference_image=spec.get("reference_image"),
                reference_strength=spec.get("reference_strength", "strong"),
            )
            generated += 1
            results.append(
                {
                    "index": idx,
                    "status": "generated",
                    "filename": filename,
                    "output_path": r.get("output_path"),
                    "elapsed_seconds": r.get("elapsed_seconds"),
                    "reference_images": r.get("reference_images"),
                    "used_endpoint": r.get("used_endpoint"),
                }
            )
            log(f"{prefix} OK ({r.get('elapsed_seconds', 0):.1f}s)")
        except Exception as e:  # noqa: BLE001
            failed += 1
            results.append(
                {
                    "index": idx,
                    "status": "failed",
                    "filename": filename,
                    "error": f"{type(e).__name__}: {e}",
                }
            )
            log(f"{prefix} FAIL: {type(e).__name__}: {e}")
            if stop_on_error:
                break

    elapsed = time.time() - t_start
    log(
        f"=== Batch done in {elapsed:.1f}s: {generated} generated, {skipped} skipped, {failed} failed ==="
    )

    manifest = {
        "total": len(specs),
        "generated": generated,
        "skipped": skipped,
        "failed": failed,
        "elapsed_seconds": round(elapsed, 1),
        "output_dir": str(out_dir),
        "log_path": str(log_file),
        "results": results,
    }

    # Also dump manifest as JSON next to log for inspection
    try:
        (out_dir / "batch_manifest.json").write_text(json.dumps(manifest, indent=2))
    except Exception:
        pass

    return manifest

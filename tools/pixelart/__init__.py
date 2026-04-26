"""Pixel-art sprite generation pipeline.

Two pieces:

  * ``generate_sprite``  — single-shot sprite generation. Calls OpenAI's
    image API for the raw render, then runs a crisp PIL post-process pass
    (background-key, palette quantize, nearest-neighbor downsample) so the
    output reads as authentic pixel art rather than "AI-painted pixel art".

  * ``batch_generate``   — resume-friendly batch driver. Feed it a list of
    sprite specs, it skips files that already exist, logs progress, returns
    a manifest.

These power ``tools/batch_generate_ship_sprites.py`` (the ship view-sphere
atlas generator) and ``tools/repixelize_from_raws.py`` (re-runs the
post-process pass against existing ``.raw.png`` outputs without burning
new API calls).

Implementation note — palette quantization uses ``Image.Quantize.FASTOCTREE``
rather than the more common MEDIANCUT. MEDIANCUT is frequency-based and
will quietly bucket rare-but-meaningful accent colors (small blue canopy
stripes, red engine rings, yellow nav lights) into the dominant hull gray.
FASTOCTREE divides by color-space region instead, so accent colors survive
the 64-color palette pass.
"""

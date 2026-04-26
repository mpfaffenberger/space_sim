#!/usr/bin/env python3
"""Repaint a ship's diffuse PNGs to a chosen "skin profile".

The original WCU diffuse textures use chunky brick-wall-style panel patterns
that confuse downstream vision pipelines (AI sprite generators read them as
masonry). They also baked in colour choices we may want to override — e.g. the
WCU Talon ships a magenta dorsal stripe, while the canonical Privateer Gold
Talon is white/silver with a blue stripe.

This tool does two passes per file, both driven by a `SkinProfile`:

  1. Hull pass: blur the low-saturation hull region heavily and recolour it
     by multiplying blurred luminance × `hull_tint`. Crushes brick edges
     into a flat alloy, preserves broad lighting.

  2. Markings pass: optional HSV hue remaps (e.g. magenta → blue) applied
     to the high-saturation regions, preserving each pixel's saturation and
     value so the markings stay readable.

Profiles are *data*, not code — adding a new ship/skin doesn't touch logic.
Operates idempotently: re-running on an already-repainted PNG is a no-op
(blur of flat region stays flat; remapped hue is outside source range).
"""
from __future__ import annotations

import argparse
import sys
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np
from PIL import Image, ImageFilter


# ---------------------------------------------------------------------------
# Pipeline knobs that aren't worth per-profile dials.
# ---------------------------------------------------------------------------
SAT_THRESHOLD = 0.18      # below this saturation, pixel is "hull"
BLUR_RADIUS_PX = 32       # strong enough to crush brick edges, weak enough
                          # to preserve broad hull shading


@dataclass(frozen=True)
class HueRemap:
    """Shift pixels whose hue is in [src_lo, src_hi] (degrees, wraps at 360)
    onto a target hue, preserving saturation and value.

    Hue ranges are inclusive. To match a region that wraps past 360°, give
    src_lo > src_hi (e.g. 350..20 = "around red").
    """
    src_lo: float
    src_hi: float
    target: float
    min_saturation: float = 0.30  # ignore near-gray pixels in the band


@dataclass(frozen=True)
class SkinProfile:
    """A repaint preset. All fields are pure data — adding a new ship is
    a one-entry edit to PROFILES, no logic change."""
    hull_tint: tuple[float, float, float]
    remaps: tuple[HueRemap, ...] = ()


# ---------------------------------------------------------------------------
# Profile registry. Add new entries here, not new functions.
# ---------------------------------------------------------------------------
PROFILES: dict[str, SkinProfile] = {
    # The first repaint we shipped: dark cool gunmetal, no marking remap.
    # Kept for reproducibility; was the right call before we had reference
    # imagery for the canonical Talon paint scheme.
    "talon_classic": SkinProfile(
        hull_tint=(0.96, 0.98, 1.06),
    ),

    # Privateer Gold / canonical Talon: glossy white-silver hull, blue
    # dorsal stripe (instead of WCU's magenta), red engine accents kept
    # as-is (they're already correct).
    "talon_hd": SkinProfile(
        hull_tint=(2.05, 2.10, 2.20),                 # ×0.44 V → ~0.93 = bright silver
        remaps=(
            HueRemap(src_lo=270.0, src_hi=340.0,      # magenta / pink band
                     target=215.0,                    # cyan-leaning blue
                     min_saturation=0.25),
        ),
    ),
}


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
def _rgb_to_hsv(rgb: np.ndarray) -> np.ndarray:
    """RGB[0..1] (H,W,3) → HSV[0..1, 0..1, 0..1] using the same formulas as
    PIL/colorsys. Vectorised; preserves shape."""
    r, g, b = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    maxc = rgb.max(axis=-1)
    minc = rgb.min(axis=-1)
    delta = maxc - minc

    s = np.where(maxc > 1e-6, delta / np.maximum(maxc, 1e-6), 0.0)
    v = maxc

    # Hue in [0, 360); we'll normalise at the end.
    safe_delta = np.maximum(delta, 1e-6)
    h_r = ((g - b) / safe_delta) % 6
    h_g = (b - r) / safe_delta + 2
    h_b = (r - g) / safe_delta + 4
    h = np.where(maxc == r, h_r,
        np.where(maxc == g, h_g, h_b)) * 60.0
    h = np.where(delta < 1e-6, 0.0, h) % 360.0
    return np.stack([h / 360.0, s, v], axis=-1)


def _hsv_to_rgb(hsv: np.ndarray) -> np.ndarray:
    """HSV[0..1] (H,W,3) → RGB[0..1] (H,W,3)."""
    h, s, v = hsv[..., 0] * 6.0, hsv[..., 1], hsv[..., 2]
    i = np.floor(h).astype(np.int32) % 6
    f = h - np.floor(h)
    p = v * (1.0 - s)
    q = v * (1.0 - s * f)
    t = v * (1.0 - s * (1.0 - f))

    r = np.choose(i, [v, q, p, p, t, v])
    g = np.choose(i, [t, v, v, q, p, p])
    b = np.choose(i, [p, p, t, v, v, q])
    return np.stack([r, g, b], axis=-1)


def _hue_in_band(h_deg: np.ndarray, lo: float, hi: float) -> np.ndarray:
    """Return mask of pixels whose hue (degrees, [0,360)) is in the band
    [lo, hi]. Bands wrap if lo > hi (e.g. 350..20 = around red)."""
    if lo <= hi:
        return (h_deg >= lo) & (h_deg <= hi)
    return (h_deg >= lo) | (h_deg <= hi)


# ---------------------------------------------------------------------------
# Core
# ---------------------------------------------------------------------------
def repaint_one(path: Path, profile: SkinProfile, *, dry_run: bool = False) -> dict[str, float]:
    img = Image.open(path).convert("RGB")
    blurred = img.filter(ImageFilter.GaussianBlur(radius=BLUR_RADIUS_PX))

    a = np.asarray(img, dtype=np.float32) / 255.0
    b = np.asarray(blurred, dtype=np.float32) / 255.0

    hsv = _rgb_to_hsv(a)
    hue_deg, sat, val = hsv[..., 0] * 360.0, hsv[..., 1], hsv[..., 2]

    # --- Pass 1: hull recolouring ------------------------------------------
    is_hull = sat < SAT_THRESHOLD
    blurred_lum = b.mean(axis=2, keepdims=True)
    hull_rgb = blurred_lum * np.array(profile.hull_tint, dtype=np.float32)
    out = np.where(is_hull[..., None], hull_rgb, a)

    # --- Pass 2: marking hue remaps ----------------------------------------
    remap_pixels = 0
    for rm in profile.remaps:
        in_band = _hue_in_band(hue_deg, rm.src_lo, rm.src_hi) & (sat >= rm.min_saturation)
        if not in_band.any():
            continue
        # Build the remapped HSV: same S, same V, hue = target.
        new_hsv = hsv.copy()
        new_hsv[..., 0] = (rm.target / 360.0)
        new_rgb = _hsv_to_rgb(new_hsv)
        out = np.where(in_band[..., None], new_rgb, out)
        remap_pixels += int(in_band.sum())

    out = np.clip(out * 255.0, 0, 255).astype(np.uint8)

    if not dry_run:
        Image.fromarray(out).save(path)

    return {
        "hull_fraction":      float(is_hull.mean()),
        "marking_fraction":   1.0 - float(is_hull.mean()),
        "remapped_pixels":    float(remap_pixels),
        "hull_mean_v_before": float(val[is_hull].mean()) if is_hull.any() else 0.0,
        "hull_mean_v_after":  float(out[is_hull].mean() / 255.0) if is_hull.any() else 0.0,
    }


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("paths", nargs="+", type=Path,
                    help="PNG files to repaint in-place")
    ap.add_argument("--profile", default="talon_classic",
                    choices=sorted(PROFILES),
                    help="Skin profile to apply (default: talon_classic)")
    ap.add_argument("--dry-run", action="store_true",
                    help="Print stats without writing")
    args = ap.parse_args()

    profile = PROFILES[args.profile]
    print(f"profile: {args.profile}  hull_tint={profile.hull_tint}  "
          f"remaps={len(profile.remaps)}")

    rc = 0
    for p in args.paths:
        if not p.exists():
            print(f"  ✗ missing: {p}", file=sys.stderr)
            rc = 1
            continue
        stats = repaint_one(p, profile, dry_run=args.dry_run)
        action = "would repaint" if args.dry_run else "repainted"
        print(f"  {action} {p}  hull={stats['hull_fraction']*100:5.1f}%  "
              f"V {stats['hull_mean_v_before']:.3f} → {stats['hull_mean_v_after']:.3f}  "
              f"remapped={int(stats['remapped_pixels'])}px")
    return rc


if __name__ == "__main__":
    sys.exit(main())

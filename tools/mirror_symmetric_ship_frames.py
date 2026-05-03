#!/usr/bin/env python3
"""mirror_symmetric_ship_frames.py — fill the mirror half of a bilaterally
symmetric ship's atlas by horizontally flipping the primary frames.

Saves ~43% of generation cost. Most of our ships have a single nose-tail
plane of mirror symmetry (delta wings out left and right are identical
under reflection, etc), which means for every azimuth `az > 180` the
camera view is the horizontal flip of the view at `360 - az`. We only
have to AI-generate one half — the other half is a free PIL flip.

Frame partitioning, per (az, el):

    primary   self-mirror:    az = 0  or  az = 180   (on the symmetry plane)
    primary   left half:      az ∈ (0, 180)
    mirror    right half:     az ∈ (180, 360)         → partner at 360 - az
    primary   poles:          el = ±90                 (single-cell pole)

So for the standard 16-az / 5-non-pole-el / 2-pole grid:

    primaries:  9 per ring × 5 rings  +  2 poles  =  47
    mirrors:    7 per ring × 5 rings              =  35
                                                    ----
                                                    82

Usage:
    python3 tools/mirror_symmetric_ship_frames.py --ship centurion
    python3 tools/mirror_symmetric_ship_frames.py --ship centurion --dry-run
    python3 tools/mirror_symmetric_ship_frames.py --ship centurion --force

By default we DON'T overwrite an existing mirror PNG that's newer than
its primary — that lets a manual finetune of a mirror cell stick around
even after a fresh primary generation. `--force` ignores that and always
mirrors. `--dry-run` only prints what would be done.

Variants flipped per frame: `.raw.png`, `.png`, `_clean.png`,
`_clean_on_black.png`. The `_clean_cell.png` is intentionally NOT flipped
here — it's a derived artefact rebuilt by `tools/center_ship_atlas_cells.py`
which reads `_clean.png` as input. Keeping the cell rebuild in one place
avoids two paths producing different cell sizes.

Lights JSON sidecars are flipped via `u → 1 - u` (UV-space mirror) just
like vflip_ship_sprites.py does for vertical flips.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from PIL import Image, ImageOps


# Variants we mirror. Order matters only for log output.
SPRITE_VARIANTS = (
    ".raw.png",
    ".png",
    "_clean.png",
    "_clean_on_black.png",
)


# ---------------------------------------------------------------------------
# Filename utilities — mirror the angle-tag conventions used by the existing
# render/sprite pipeline so we operate on the right files.
# ---------------------------------------------------------------------------


def az_tag(az: float) -> str:
    """Filename-safe azimuth tag: az000 / az022p5 / az337p5."""
    if abs(az - round(az)) < 0.01:
        return f"az{int(round(az)):03d}"
    return f"az{az:05.1f}".replace(".", "p")


def el_tag(el: float) -> str:
    """Filename-safe signed elevation tag: el+000 / el-060 / el+090."""
    return f"el{int(round(el)):+04d}"


def stem_for(ship: str, az: float, el: float) -> str:
    """Sprite-output stem (no .png) for the AI-generated cell at (az, el)."""
    return f"{ship}_{az_tag(az)}_{el_tag(el)}_newmodel_512"


def is_self_mirror_az(az: float) -> bool:
    """True when az lies on the bilateral symmetry plane (rear or front)."""
    a = az % 360.0
    return abs(a - 0.0) < 0.5 or abs(a - 180.0) < 0.5


def mirror_partner_az(az: float) -> float | None:
    """Return the primary's azimuth for a mirror az, or None if `az` IS a
    primary (or self-mirror or pole-equivalent). Primary-side convention:
    primaries cover az ∈ [0, 180]; mirrors cover az ∈ (180, 360)."""
    a = az % 360.0
    if a <= 180.0 + 0.5:
        return None     # primary or self-mirror — no mirror needed
    return round(360.0 - a, 6)


def is_pole(el: float) -> bool:
    return abs(el - 90.0) < 0.5 or abs(el - (-90.0)) < 0.5


# ---------------------------------------------------------------------------
# Mirror operations.
# ---------------------------------------------------------------------------


def mirror_png(src: Path, dst: Path) -> None:
    """Horizontally flip src into dst. Preserves mode + alpha."""
    im = Image.open(src)
    Image.Image.transpose(im, Image.FLIP_LEFT_RIGHT).save(dst)


def _format_spot(spot: dict) -> str:
    """Same one-line spot format vflip_ship_sprites.py uses, for diff-clean
    sidecar rewrites."""
    parts = []
    for k, v in spot.items():
        parts.append(f'"{k}": {json.dumps(v)}')
    return "{ " + ", ".join(parts) + " }"


def mirror_lights_sidecar(src: Path, dst: Path) -> int:
    """u → 1 - u for every spot. Mirrors mirror_lights_sidecar's vertical
    cousin in vflip_ship_sprites.py (which uses v → 1 - v). Returns the
    count flipped. Wraps both flat-list and {"spots": [...]} schemas."""
    spots = json.loads(src.read_text())

    if isinstance(spots, dict) and isinstance(spots.get("spots"), list):
        n = 0
        for spot in spots["spots"]:
            if isinstance(spot, dict) and "u" in spot:
                spot["u"] = round(1.0 - float(spot["u"]), 6)
                n += 1
        dst.write_text(json.dumps(spots, indent=2) + "\n")
        return n

    if not isinstance(spots, list):
        return 0
    n = 0
    out = []
    for spot in spots:
        if isinstance(spot, dict) and "u" in spot:
            mirrored = dict(spot)
            mirrored["u"] = round(1.0 - float(spot["u"]), 6)
            out.append(mirrored)
            n += 1
        else:
            out.append(spot)
    dst.write_text("[\n" + ",\n".join(
        "  " + _format_spot(s) for s in out
    ) + "\n]\n")
    return n


# ---------------------------------------------------------------------------
# Plan + execute.
# ---------------------------------------------------------------------------


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--ship", required=True)
    ap.add_argument("--ships-root", default="assets/ships")
    ap.add_argument("--dry-run", action="store_true",
                    help="Print what would be flipped without writing.")
    ap.add_argument("--force", action="store_true",
                    help="Always overwrite mirror outputs, even if their "
                         "mtime is newer than the primary's.")
    ap.add_argument("--variants", default=",".join(SPRITE_VARIANTS),
                    help="Comma-separated list of suffix variants to mirror. "
                         f"Default: {','.join(SPRITE_VARIANTS)}")
    args = ap.parse_args()

    ship_root = Path(args.ships_root) / args.ship
    sprites_dir = ship_root / "sprites"
    renders_dir = ship_root / "renders"
    manifest_path = renders_dir / "manifest.json"

    if not manifest_path.is_file():
        print(f"[mirror] no render manifest at {manifest_path}", file=sys.stderr)
        return 2
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))

    variants = tuple(v.strip() for v in args.variants.split(",") if v.strip())
    if not variants:
        print("[mirror] --variants resolved to empty list — nothing to do.",
              file=sys.stderr)
        return 2

    # Plan: for every mirror sample, find its primary partner and the on-disk
    # PNGs to flip. Build the plan completely before touching disk so a
    # dry-run is informative even if intermediate steps would fail.
    plans: list[tuple[dict, dict]] = []   # (mirror_sample, primary_sample)
    by_az_el: dict[tuple[float, float], dict] = {
        (round(s["az"], 6), round(s["el"], 6)): s for s in manifest["samples"]
    }
    primaries = 0
    self_mirrors = 0
    poles = 0
    for s in manifest["samples"]:
        az = float(s["az"])
        el = float(s["el"])
        if is_pole(el):
            poles += 1
            continue
        partner_az = mirror_partner_az(az)
        if partner_az is None:
            if is_self_mirror_az(az):
                self_mirrors += 1
            else:
                primaries += 1
            continue
        primary = by_az_el.get((round(partner_az, 6), round(el, 6)))
        if primary is None:
            print(f"[mirror] no primary at (az={partner_az}, el={el}) — "
                  f"skipping mirror at (az={az}, el={el})",
                  file=sys.stderr)
            continue
        plans.append((s, primary))

    print(f"[mirror] ship={args.ship}")
    print(f"  manifest samples : {len(manifest['samples'])}")
    print(f"  poles (skip)     : {poles}")
    print(f"  self-mirrors     : {self_mirrors}  (az = 0 or 180)")
    print(f"  primaries        : {primaries}")
    print(f"  mirrors planned  : {len(plans)}")
    print(f"  variants         : {', '.join(variants)}")
    if args.dry_run:
        print("  (dry-run: no writes)")
    if args.force:
        print("  (force: ignore mirror-newer-than-primary)")

    n_png = 0
    n_lights = 0
    n_skip_newer = 0
    for mirror_s, primary_s in plans:
        m_stem = stem_for(args.ship, float(mirror_s["az"]), float(mirror_s["el"]))
        p_stem = stem_for(args.ship, float(primary_s["az"]), float(primary_s["el"]))

        # PNG variants.
        for tail in variants:
            src = sprites_dir / (p_stem + tail)
            dst = sprites_dir / (m_stem + tail)
            if not src.is_file():
                # Missing primary variants are fine — e.g. .raw.png absent
                # when save_raw was off at gen time. Just skip silently.
                continue
            if (not args.force and dst.is_file() and
                    dst.stat().st_mtime > src.stat().st_mtime):
                n_skip_newer += 1
                if args.dry_run:
                    print(f"  skip (mirror newer): {dst.name}")
                continue
            print(f"  mirror: {src.name}  →  {dst.name}")
            if not args.dry_run:
                mirror_png(src, dst)
                n_png += 1

        # Lights sidecar — *.lights.json. We also flip the v cousin's spots'
        # u coords. Not every cell has lights authored.
        src_lights = sprites_dir / (p_stem + ".lights.json")
        dst_lights = sprites_dir / (m_stem + ".lights.json")
        if src_lights.is_file():
            if (not args.force and dst_lights.is_file() and
                    dst_lights.stat().st_mtime > src_lights.stat().st_mtime):
                if args.dry_run:
                    print(f"  skip lights (newer): {dst_lights.name}")
            else:
                print(f"  mirror lights: {src_lights.name}  →  {dst_lights.name}")
                if not args.dry_run:
                    n_lights += mirror_lights_sidecar(src_lights, dst_lights)

    print(f"[mirror] done. wrote {n_png} PNGs, {n_lights} light spots, "
          f"skipped {n_skip_newer} mirror-newer-than-primary.")
    if not args.dry_run and n_png > 0:
        print("[mirror] note: *_clean_cell.png is NOT flipped here — re-run "
              "tools/center_ship_atlas_cells.py to rebuild engine-facing cells.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

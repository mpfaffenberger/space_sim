#!/usr/bin/env python3
"""feather_sprite_edges.py — soften the silhouette of every cell PNG in a
ship's sprite atlas by ~1 px while preserving the chunky pixel-art
interior.

The problem: ship sprites are saved with binary alpha (every pixel is
either fully opaque or fully transparent). The cell-selection pipeline
draws them with bilinear filtering, but bilinear can't invent transition
pixels that don't exist in the source — so the silhouette stair-steps
along the alpha cutoff, which reads as harsh aliasing in-flight. The
interior pixel-art (hull plates, panel lines, lights) is the chunky
look we WANT; only the outline needs softening.

The fix (offline, idempotent): for each `*_clean_cell.png` in the ship's
sprites/ directory:

  1. Premultiply RGB by alpha. Where alpha=0, premult RGB is also 0.
  2. Gaussian-blur the premultiplied RGB AND the alpha channel by a
     small radius (default 0.8 px).
  3. Unpremultiply: out_rgb = blurred_rgb_premult / blurred_alpha. This
     mathematically equals the average of all opaque-neighbour colours
     weighted by alpha, which is exactly the "RGB bleed outward into
     transparent area" pattern we need to avoid the classic dark-fringe
     artifact you get from naive alpha-only blur.
  4. Replace alpha with the blurred alpha.

The interior 90+% of each cell is unchanged because it's surrounded by
fully-opaque neighbours — the blur of (premult_rgb / alpha) collapses
back to the original RGB. Only edge pixels move, gaining a 1 px feather.

Idempotence: writes a `.feather_applied` sentinel next to the sprites
directory. Re-running is refused unless --force; doing the blur twice
DOES double-feather (this isn't a flip-flop op like elevation negation),
so the sentinel exists specifically to prevent accidental double-blur.

To revert: use git-LFS to restore the originals (`git checkout
assets/ships/<ship>/sprites/` then delete the sentinel).

Usage:
    python3 tools/feather_sprite_edges.py talon
    python3 tools/feather_sprite_edges.py tarsus --radius 1.0
    python3 tools/feather_sprite_edges.py talon --dry-run
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from PIL import Image, ImageFilter, ImageMath

REPO = Path(__file__).resolve().parents[1]


def feather_one_image(path: Path, radius: float) -> tuple[int, int]:
    """Apply premultiplied-alpha blur to a single PNG in place.

    Returns (n_edge_pixels_before, n_edge_pixels_after) where "edge" is
    counted as alpha pixels in the [1, 254] range. A binary-alpha source
    has 0 edge pixels before, and however-many the feather produced
    after — useful as a sanity check that blurring actually happened.
    """
    img = Image.open(path).convert("RGBA")
    r, g, b, a = img.split()

    edge_before = sum(a.histogram()[1:255])

    # Step 1: premultiply RGB by alpha. ImageChops.multiply does
    # pixel-wise multiplication and divides by 255, which is exactly the
    # premult op we want (alpha treated as 0..1 by the divide).
    from PIL import ImageChops
    r_pm = ImageChops.multiply(r, a)
    g_pm = ImageChops.multiply(g, a)
    b_pm = ImageChops.multiply(b, a)

    # Step 2: blur premultiplied RGB AND alpha by the same radius. Same
    # radius is critical — different radii produce hue-shift artifacts
    # at the edge.
    blur = ImageFilter.GaussianBlur(radius=radius)
    r_pm_b = r_pm.filter(blur)
    g_pm_b = g_pm.filter(blur)
    b_pm_b = b_pm.filter(blur)
    a_b    = a.filter(blur)

    # Step 3: unpremultiply blurred RGB. Where alpha is 0 the divide is
    # undefined; we sub alpha=1 there to dodge div-by-zero, and the
    # resulting RGB gets multiplied back to ~0 anyway because premult was
    # 0 there. Pillow 10+ renamed `ImageMath.eval` to `lambda_eval` —
    # callable instead of a string, same effect, type-safe API. ImageMath's
    # `min` and `max` helpers are method-shaped (operand first, scalar
    # second) so we build the expression bottom-up rather than nesting.
    def safe_div(args):
        pm = args["pm"]
        a  = args["a"]
        safe_a  = args["max"](a, 1)               # clamp alpha ≥ 1 to dodge /0
        raw     = (pm * 255 + a / 2) / safe_a     # rounded unpremultiply
        clipped = args["min"](raw, 255)           # clamp top
        return args["convert"](clipped, "L")
    r_bled = ImageMath.lambda_eval(safe_div, pm=r_pm_b, a=a_b)
    g_bled = ImageMath.lambda_eval(safe_div, pm=g_pm_b, a=a_b)
    b_bled = ImageMath.lambda_eval(safe_div, pm=b_pm_b, a=a_b)
    # ImageMath returns an `Image.core` object (raw image core, not Image).
    # Wrap back into PIL Image so it composes with original-channel ops.
    r_bled = Image.frombytes("L", img.size, r_bled.tobytes())
    g_bled = Image.frombytes("L", img.size, g_bled.tobytes())
    b_bled = Image.frombytes("L", img.size, b_bled.tobytes())

    # Step 4: protect the interior. Where the ORIGINAL alpha was 255, the
    # pixel is fully inside the silhouette and its RGB IS the chunky
    # pixel-art Mike wants preserved. Only the bled-out feather zone
    # (where original alpha was 0 but blurred alpha is now > 0) should
    # carry the bled colour. Image.composite picks `original` where the
    # mask is non-zero, `bled` where it's zero — so the original alpha
    # channel itself (binary 0 or 255) is the perfect selector mask.
    r_out = Image.composite(r, r_bled, a)
    g_out = Image.composite(g, g_bled, a)
    b_out = Image.composite(b, b_bled, a)

    out = Image.merge("RGBA", (r_out, g_out, b_out, a_b))

    edge_after = sum(a_b.histogram()[1:255])

    out.save(path, "PNG")
    return edge_before, edge_after


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("ship", help="ship name, e.g. talon — looks at "
                                     "assets/ships/<ship>/sprites/")
    parser.add_argument("--radius", type=float, default=0.8,
                        help="Gaussian blur radius in pixels (default 0.8). "
                             "Bump to 1.2-1.5 for a more pronounced halo; "
                             "drop to 0.5 for barely-there softening.")
    parser.add_argument("--dry-run", action="store_true",
                        help="Process the FIRST matching file only, write to "
                             "/tmp/np_feather_preview.png, leave originals "
                             "and sentinel untouched.")
    parser.add_argument("--force", action="store_true",
                        help="Apply even if the sentinel says we already "
                             "feathered. WARNING: this DOES re-blur an "
                             "already-blurred image (compounding the effect). "
                             "Use git-lfs to restore originals before re-running.")
    args = parser.parse_args()

    ship_dir = REPO / "assets" / "ships" / args.ship
    sprites_dir = ship_dir / "sprites"
    sentinel = ship_dir / ".feather_applied"

    if not sprites_dir.is_dir():
        print(f"sprites dir not found: {sprites_dir}", file=sys.stderr)
        sys.exit(1)

    cells = sorted(sprites_dir.glob("*_clean_cell.png"))
    if not cells:
        print(f"no *_clean_cell.png found in {sprites_dir}", file=sys.stderr)
        sys.exit(2)

    print(f"[feather] ship      : {args.ship}")
    print(f"[feather] sprites   : {sprites_dir.relative_to(REPO)}")
    print(f"[feather] cells     : {len(cells)}")
    print(f"[feather] radius    : {args.radius} px")

    if args.dry_run:
        target = cells[0]
        preview = Path("/tmp/np_feather_preview.png")
        # Copy first so we don't mutate the source in dry-run mode.
        Image.open(target).save(preview)
        edge_before, edge_after = feather_one_image(preview, args.radius)
        print(f"[feather] dry-run: {target.name}")
        print(f"[feather]   edge px before : {edge_before}")
        print(f"[feather]   edge px after  : {edge_after}")
        print(f"[feather]   preview        : {preview}")
        print("[feather] no originals modified.")
        return

    if sentinel.exists() and not args.force:
        print(f"[feather] sentinel exists ({sentinel.relative_to(REPO)}); "
              f"refusing to feather again.")
        print(f"[feather] to revert: `git checkout {sprites_dir.relative_to(REPO)}` "
              f"then `rm {sentinel.relative_to(REPO)}`")
        sys.exit(0)

    total_edge_before = 0
    total_edge_after = 0
    for cell in cells:
        eb, ea = feather_one_image(cell, args.radius)
        total_edge_before += eb
        total_edge_after  += ea

    sentinel.write_text(
        f"feather_sprite_edges.py applied with radius={args.radius}.\n"
        f"To revert: `git checkout {sprites_dir.relative_to(REPO)}` then\n"
        f"delete this sentinel. Re-running with --force compounds the blur.\n",
        encoding="utf-8",
    )

    print(f"[feather] done — feathered {len(cells)} cells.")
    print(f"[feather]   total edge px before : {total_edge_before}")
    print(f"[feather]   total edge px after  : {total_edge_after}")
    print(f"[feather]   sentinel             : {sentinel.relative_to(REPO)}")


if __name__ == "__main__":
    main()

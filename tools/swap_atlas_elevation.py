#!/usr/bin/env python3
"""swap_atlas_elevation.py — flip the sign of every atlas frame's elevation.

Rationale (Talon, April 2026):
    The Talon view-sphere atlas behaves as if the AI generation pipeline
    confused which hemisphere it was rendering — cells stored at el+X
    visually contain content that belongs at el-X (camera-above sprites
    show belly silhouettes, camera-below sprites show dorsal content),
    and vice versa. Confirmed live via the F3 ship-frame HUD: as the
    camera moves above the Talon the engine picks an el+60 cell whose
    rendered hull is plainly a from-below view of the ship.

    The cheapest possible repair is to swap the slot each cell occupies
    rather than regenerating anything: negate the el value in every
    atlas_manifest.json sample. The sprite files on disk don't move,
    their .lights.json sidecars stay paired with the same sprite art, and
    the ShipSpriteFrame nearest-neighbour lookup picks the now-correctly-
    labelled cell.

Pattern mirrors tools/vflip_ship_sprites.py:
    - idempotent via a per-ship sentinel file next to the manifest
    - flip + flip = identity, so re-running with the sentinel removed
      undoes a previous swap; safe to back out.
    - --dry-run prints the planned changes without writing anything.
    - --force overrides the sentinel if you really mean it.

This intentionally does NOT rename the .png files. Renaming would churn
~80 LFS objects and the file basenames are now technically a tiny lie
(filename says el+060, manifest slot says el-60). Worth it for a one-line
fix that any future you can reverse with one shell command. If the
filename mismatch ever becomes confusing, write a separate rename pass.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]


def manifest_path(ship: str) -> Path:
    return REPO / "assets" / "ships" / ship / "atlas_manifest.json"


def sentinel_path(ship: str) -> Path:
    return REPO / "assets" / "ships" / ship / ".elswap_applied"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ship", help="ship name, e.g. talon")
    parser.add_argument("--dry-run", action="store_true",
                        help="print planned changes without writing")
    parser.add_argument("--force", action="store_true",
                        help="apply even if sentinel says we already swapped")
    args = parser.parse_args()

    mpath = manifest_path(args.ship)
    spath = sentinel_path(args.ship)
    if not mpath.exists():
        print(f"manifest not found: {mpath}", file=sys.stderr)
        sys.exit(1)

    if spath.exists() and not args.force and not args.dry_run:
        print(f"[elswap] sentinel exists ({spath.relative_to(REPO)}); refusing to swap again.")
        print(f"[elswap] delete the sentinel and re-run to undo (flip + flip = identity).")
        sys.exit(0)

    manifest = json.loads(mpath.read_text(encoding="utf-8"))
    samples = manifest.get("samples")
    if not isinstance(samples, list):
        print(f"manifest is missing a 'samples' array: {mpath}", file=sys.stderr)
        sys.exit(2)

    swapped = 0
    for sample in samples:
        if not isinstance(sample, dict) or "el" not in sample:
            continue
        old = float(sample["el"])
        # Add 0.0 to canonicalise -0.0 → 0.0 (avoids ugly diffs on the el=0 row).
        new = (-old) + 0.0
        # Preserve the integer-vs-float storage style so the diff stays
        # minimal — the existing manifest writes -60.0 not -60.
        sample["el"] = new if isinstance(sample["el"], float) else int(new)
        swapped += 1

    # Mirror the change in the top-level elevation_degrees list so the
    # manifest is internally consistent. Sort it for stable output. Add 0.0
    # to scrub a stray -0.0 from negating a literal 0.0 — JSON-legal but
    # ugly in diffs.
    if isinstance(manifest.get("elevation_degrees"), list):
        manifest["elevation_degrees"] = sorted(
            [(-float(e)) + 0.0 for e in manifest["elevation_degrees"]]
        )

    print(f"[elswap] ship={args.ship}")
    print(f"[elswap]   manifest : {mpath.relative_to(REPO)}")
    print(f"[elswap]   samples  : {len(samples)} ({swapped} elevations negated)")

    if args.dry_run:
        print("[elswap] dry-run; no files modified.")
        return

    mpath.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    spath.write_text(
        "swap_atlas_elevation.py applied; remove this file to allow another swap "
        "(flip + flip = identity).\n",
        encoding="utf-8",
    )
    print(f"[elswap] sentinel : {spath.relative_to(REPO)}")


if __name__ == "__main__":
    main()

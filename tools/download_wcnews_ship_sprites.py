#!/usr/bin/env python3
"""download_wcnews_ship_sprites.py — pull canonical Privateer ship sprite
sheets from the Combat Information Center wcpedia "3Space Ship Sprite
Archive" page.

Why this exists:
    Our AI-generated Talon atlas drifted hard on bottom-hemisphere cells
    (cockpit hallucinated where there should be belly), and the
    canonical-reference image leaked dorsal pose into every angle. The
    wcnews wcpedia hosts the full, alpha-cut, low-res ORIGINAL Origin
    sprite sheets — 37 view angles per ship, three faction variants for
    the Talon (Militia / Pirate / Church of Man). That is the ground
    truth we should have started with.

Legal / hygiene:
    These are 1993 Origin Systems assets, not ours. The download dir is
    gitignored under assets/ships/*/wcnews_sprites/ — we do NOT commit
    them to the public repo. Mirroring locally for personal-use sprite
    research is fine; redistributing isn't. If we end up shipping any
    derived art it'll be Mike-authored work that learns from these, not
    these files themselves.

Usage:
    tools/download_wcnews_ship_sprites.py talon
    tools/download_wcnews_ship_sprites.py talon --variants TALMIL TALPIR
    tools/download_wcnews_ship_sprites.py --list                # just print plan

Naming convention on the wiki:
    /wcpedia/images/<PREFIX>.IFF.ANGLES.SHP-<NNN>.png

We hardcode (ship → list of variant prefixes) here. New ships: add a row
to SHIP_VARIANTS, run, done. The wiki uses one IFF per faction (TALMIL,
TALPIR, TALRELIG for talon), and 37 angles per IFF.
"""

from __future__ import annotations

import argparse
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]

WCNEWS_BASE = "https://www.wcnews.com/wcpedia/images"
USER_AGENT = "biscuit-the-puppy/1.0 (+research; contact: mike@new_privateer)"
FRAMES_PER_SHIP = 37   # confirmed from page scrape; 0..36

# (ship_slug, [(variant_label, iff_prefix), ...])
SHIP_VARIANTS: dict[str, list[tuple[str, str]]] = {
    "talon": [
        ("militia",       "TALMIL"),
        ("pirate",        "TALPIR"),
        ("church_of_man", "TALRELIG"),
    ],
}


def variant_outdir(ship: str, variant_label: str) -> Path:
    return REPO / "assets" / "ships" / ship / "wcnews_sprites" / variant_label


def remote_url(prefix: str, frame: int) -> str:
    return f"{WCNEWS_BASE}/{prefix}.IFF.ANGLES.SHP-{frame:03d}.png"


def fetch(url: str, dest: Path, timeout: float = 20.0) -> tuple[bool, str]:
    """Download `url` to `dest`. Returns (ok, status_msg). Skips if file exists."""
    if dest.exists() and dest.stat().st_size > 0:
        return True, "cached"
    dest.parent.mkdir(parents=True, exist_ok=True)
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            data = resp.read()
    except (urllib.error.HTTPError, urllib.error.URLError, TimeoutError) as exc:
        return False, f"{type(exc).__name__}: {exc}"

    # PNGs start with the standard 8-byte signature. If we got HTML or 404
    # text by mistake, skip writing it so we don't pollute the cache.
    if not data.startswith(b"\x89PNG\r\n\x1a\n"):
        return False, f"not a PNG ({len(data)} bytes; starts {data[:8]!r})"
    dest.write_bytes(data)
    return True, f"{len(data)} bytes"


def download_ship(ship: str, only_variants: list[str] | None,
                  delay_s: float, list_only: bool) -> int:
    if ship not in SHIP_VARIANTS:
        known = ", ".join(sorted(SHIP_VARIANTS))
        print(f"unknown ship {ship!r}; known: {known}", file=sys.stderr)
        return 2

    variants = SHIP_VARIANTS[ship]
    if only_variants:
        variants = [(lbl, pref) for (lbl, pref) in variants if pref in only_variants]
        if not variants:
            print(f"no variants matched {only_variants!r}", file=sys.stderr)
            return 2

    print(f"ship       : {ship}")
    print(f"variants   : {[v[0] for v in variants]}")
    print(f"frames each: {FRAMES_PER_SHIP}")
    print(f"total dl   : {len(variants) * FRAMES_PER_SHIP}")
    print()

    failures: list[tuple[str, int, str]] = []
    for label, prefix in variants:
        outdir = variant_outdir(ship, label)
        for n in range(FRAMES_PER_SHIP):
            url  = remote_url(prefix, n)
            dest = outdir / f"{prefix}.IFF.ANGLES.SHP-{n:03d}.png"
            if list_only:
                print(f"  PLAN  {url} -> {dest.relative_to(REPO)}")
                continue
            ok, msg = fetch(url, dest)
            mark = "ok " if ok else "FAIL"
            print(f"  {mark}  {prefix}-{n:03d}  ({msg})")
            if not ok:
                failures.append((prefix, n, msg))
            # Be polite to the wiki; cached fetches don't sleep.
            if msg != "cached" and delay_s > 0:
                time.sleep(delay_s)
        print()

    if failures and not list_only:
        print(f"=== {len(failures)} failure(s) ===")
        for prefix, n, msg in failures:
            print(f"  {prefix}-{n:03d}: {msg}")
        return 1
    return 0


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ship", nargs="?", default="talon",
                        help="ship slug (currently: 'talon')")
    parser.add_argument("--variants", nargs="*", default=None,
                        help="optional whitelist of IFF prefixes, e.g. TALMIL TALPIR")
    parser.add_argument("--delay", type=float, default=0.05,
                        help="seconds to sleep between non-cached fetches")
    parser.add_argument("--list", dest="list_only", action="store_true",
                        help="print the planned downloads without fetching")
    args = parser.parse_args()
    sys.exit(download_ship(args.ship, args.variants, args.delay, args.list_only))


if __name__ == "__main__":
    main()

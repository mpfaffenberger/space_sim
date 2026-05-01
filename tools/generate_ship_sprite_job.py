#!/usr/bin/env python3
"""Generate one ship sprite cell from an editor-authored JSON request.

This is the bridge used by the in-game Sprite Generation Workbench. The game
writes a tiny request JSON, then shells out to this script. Keeping model/API
calls in Python avoids stuffing secrets, network code, and flaky API behavior
into the C++ renderer. Boring boundaries save puppies.

Request shape:
{
  "ship": "centurion",
  "display_name": "Centurion",
  "az": 45.0,
  "el": 0.0,
  "primary_reference": "",          # optional; blank = resolve from renders/manifest.json
  "extra_references": ["assets/..."],
  "use_default_canonical_refs": true,
  "prompt_suffix": "extra art direction",
  "custom_prompt": "",              # optional full prompt override
  "quality": "high",
  "clean": true,
  "preview": true,
  "force": false,
  "write_prompt_only": false
}
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tools"))

import batch_generate_ship_sprites as batch  # noqa: E402

SHIP_RE = re.compile(r"^[A-Za-z0-9_-]+$")
QUALITY = {"low", "medium", "high", "auto"}


def repo_path(value: str | Path) -> Path:
    p = Path(value)
    return p if p.is_absolute() else REPO / p


def angle_close(a: float, b: float, eps: float = 0.05) -> bool:
    return abs(float(a) - float(b)) <= eps


def require_ship_slug(ship: str) -> str:
    if not ship or not SHIP_RE.match(ship):
        raise SystemExit(f"bad ship slug {ship!r}; use letters, numbers, '_' or '-'")
    return ship


def load_request(path: Path) -> dict[str, Any]:
    if not path.exists():
        raise SystemExit(f"request not found: {path}")
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise SystemExit("request root must be a JSON object")
    return data


def find_manifest_reference(ship: str, az: float, el: float) -> Path:
    manifest_path = REPO / "assets" / "ships" / ship / "renders" / "manifest.json"
    if not manifest_path.exists():
        raise SystemExit(
            f"no primary_reference supplied and render manifest is missing: {manifest_path}"
        )
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    matches = []
    for sample in manifest.get("samples", []):
        saz = float(sample.get("az", 999999.0))
        sel = float(sample.get("el", 999999.0))
        if angle_close(saz, az) and angle_close(sel, el):
            matches.append(sample)
    if not matches:
        raise SystemExit(f"no render sample for ship={ship!r} az={az:g} el={el:g}")
    return REPO / "assets" / "ships" / ship / "renders" / matches[0]["file"]


def resolve_reference_list(values: list[Any]) -> tuple[Path, ...]:
    out: list[Path] = []
    for raw in values:
        text = str(raw).strip()
        if not text:
            continue
        p = repo_path(text).resolve()
        if not p.is_file():
            raise SystemExit(f"reference image not found: {p}")
        out.append(p)
    return tuple(out)


def output_stem(ship: str, az: float, el: float) -> str:
    return f"{ship}_az{batch.az_tag(az)}_el{batch.angle_tag(el)}_newmodel_512"


def write_json(path: Path, data: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2), encoding="utf-8")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--request", required=True, type=Path)
    args = ap.parse_args()

    request_path = repo_path(args.request).resolve()
    req = load_request(request_path)

    ship = require_ship_slug(str(req.get("ship", "")).strip())
    display_name = str(req.get("display_name", "")).strip()
    az = float(req.get("az", 0.0))
    el = float(req.get("el", 0.0))
    quality = str(req.get("quality", "high")).strip().lower()
    if quality not in QUALITY:
        raise SystemExit(f"bad quality {quality!r}; expected one of {sorted(QUALITY)}")

    primary_text = str(req.get("primary_reference", "")).strip()
    primary_ref = repo_path(primary_text).resolve() if primary_text else find_manifest_reference(ship, az, el).resolve()
    if not primary_ref.is_file():
        raise SystemExit(f"primary reference not found: {primary_ref}")

    extra_refs = resolve_reference_list(req.get("extra_references", []) or [])
    canonical_refs: tuple[Path, ...] = ()
    if bool(req.get("use_default_canonical_refs", True)):
        canonical_refs = batch.resolve_canonical_refs(ship, REPO)
    all_style_refs = (*canonical_refs, *extra_refs)

    prompt_suffix = str(req.get("prompt_suffix", ""))
    if display_name:
        prompt_suffix = f"Ship display/canonical name: {display_name}\n" + prompt_suffix

    custom_prompt = str(req.get("custom_prompt", "")).strip()
    prompt = custom_prompt or batch.build_prompt(
        ship,
        primary_ref,
        all_style_refs,
        prompt_suffix=prompt_suffix,
        az=az,
        el=el,
    )

    sprite_dir = REPO / "assets" / "ships" / ship / "sprites"
    prompt_dir = sprite_dir / "prompts"
    stem = output_stem(ship, az, el)
    raw = sprite_dir / f"{stem}.png"
    clean = sprite_dir / f"{stem}_clean.png"
    prompt_file = prompt_dir / f"{stem}.txt"
    prompt_file.parent.mkdir(parents=True, exist_ok=True)
    prompt_file.write_text(prompt, encoding="utf-8")

    job = batch.SpriteJob(
        ship=ship,
        az=az,
        el=el,
        reference=primary_ref,
        raw_output=raw,
        clean_output=clean,
        prompt_file=prompt_file,
        prompt=prompt,
        canonical_refs=all_style_refs,
    )

    status_path = request_path.with_suffix(".status.json")
    status = {
        "ship": ship,
        "display_name": display_name,
        "az": az,
        "el": el,
        "primary_reference": str(primary_ref.relative_to(REPO)),
        "style_references": [str(p.relative_to(REPO)) for p in all_style_refs],
        "prompt_file": str(prompt_file.relative_to(REPO)),
        "raw_output": str(raw.relative_to(REPO)),
        "clean_output": str(clean.relative_to(REPO)),
        "quality": quality,
        "generated": False,
        "cleaned": False,
    }
    write_json(status_path, status)

    print(f"request      : {request_path.relative_to(REPO)}")
    print(f"ship         : {ship}")
    print(f"angle        : az={az:g} el={el:+g}")
    print(f"primary ref  : {primary_ref.relative_to(REPO)}")
    print(f"style refs   : {len(all_style_refs)}")
    for ref in all_style_refs:
        print(f"  - {ref.relative_to(REPO)}")
    print(f"prompt file  : {prompt_file.relative_to(REPO)}")
    print(f"raw output   : {raw.relative_to(REPO)}")

    if bool(req.get("write_prompt_only", False)):
        print("write_prompt_only=true; stopping before model call")
        return

    force = bool(req.get("force", False))
    if raw.exists() and not force:
        print(f"skip existing raw output: {raw.relative_to(REPO)}")
    else:
        batch.run_pixelart_tool(job, quality)
        status["generated"] = True
        write_json(status_path, status)

    if bool(req.get("clean", True)):
        if not raw.exists():
            raise SystemExit(f"generator did not create raw output: {raw}")
        batch.clean_sprite(job, preview=bool(req.get("preview", True)))
        status["cleaned"] = clean.exists()
        write_json(status_path, status)

    print("done")


if __name__ == "__main__":
    main()

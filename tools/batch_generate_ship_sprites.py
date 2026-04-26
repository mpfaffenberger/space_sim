#!/usr/bin/env python3
"""Batch-generate reference-conditioned ship sprites from rendered view samples.

This script owns the *boring orchestration*:
  - reads assets/ships/<ship>/renders/manifest.json
  - builds one strict prompt per render angle
  - writes prompts/jobs for review or external queueing
  - optionally invokes a user-supplied generator command template
  - optionally runs tools/clean_sprite_alpha.py on generated outputs

It intentionally does NOT hard-code a specific image model API. The pixel artist
agent/tool is not a normal repo-local Python package, and pretending otherwise
would be brittle nonsense. Instead, wire your updated image tool via
--generator-cmd with placeholders:

  {reference}    input render path
  {output}       raw generated PNG output path
  {prompt_file}  temporary prompt text file
  {prompt_json}  JSON-escaped prompt string

Examples:

  # Just write jobs/prompts, no generation:
  python3 tools/batch_generate_ship_sprites.py --ship tarsus

  # Generate only missing raw sprites with your CLI:
  python3 tools/batch_generate_ship_sprites.py --ship tarsus \
    --generator-cmd 'my-image-tool --reference {reference} --prompt-file {prompt_file} --out {output}' \
    --clean

  # Test first 4 samples only:
  python3 tools/batch_generate_ship_sprites.py --ship tarsus --limit 4
"""

from __future__ import annotations

import argparse
import concurrent.futures
import importlib.util
import json
import shlex
import subprocess
import sys
import threading
from dataclasses import dataclass
from pathlib import Path


REPO = Path(__file__).resolve().parents[1]
PIXELART_DIR      = REPO / "tools" / "pixelart"
PIXELART_GENERATE = PIXELART_DIR / "generate_sprite.py"
PIXELART_BATCH    = PIXELART_DIR / "batch_generate.py"


@dataclass(frozen=True)
class SpriteJob:
    ship: str
    az: float
    el: float
    reference: Path                          # primary per-angle render (pose/silhouette)
    raw_output: Path
    clean_output: Path
    prompt_file: Path
    prompt: str
    canonical_refs: tuple[Path, ...] = ()    # extra style/colour guides (same for every angle)

    @property
    def all_references(self) -> list[Path]:
        """Primary render first, canonical refs after. Generators that take a
        list use this; the order matters because some attention models weight
        the first reference more heavily."""
        return [self.reference, *self.canonical_refs]


def angle_tag(value: float) -> str:
    """Filename-safe signed angle tag: +022, -030, +000."""
    return f"{value:+04.0f}"


def az_tag(value: float) -> str:
    """Filename-safe azimuth tag. Preserve legacy az000 for integral angles."""
    if abs(value - round(value)) < 0.01:
        return f"{int(round(value)):03d}"
    return f"{value:05.1f}".replace(".", "p")


# ---------------------------------------------------------------------------
# Per-ship design briefs.
#
# The base prompt is generic; everything ship-specific goes here. New ships
# get an entry in this dict — no prompt-template surgery required. If a ship
# is missing we fall back to DEFAULT_BRIEF, which leans entirely on the
# reference image for shape information (safer than lying about geometry).
#
# Fields:
#   role        : the ship's class ("freighter", "light fighter", ...).
#                 Goes into the "Subject:" line.
#   silhouette  : 1–2 sentences naming the distinguishing structural
#                 features the AI must preserve.
#   palette     : short palette hint for hull colour bias.
# ---------------------------------------------------------------------------
SHIP_DESIGN_BRIEFS: dict[str, dict[str, str]] = {
    "tarsus": {
        "role": "space freighter",
        "silhouette": (
            "boxy cockpit/nose, long central fuselage, twin side nacelles, "
            "utilitarian cargo-hauler structure"
        ),
        "palette": "dark gunmetal / olive-gray hull",
    },
    "talon": {
        "role": "light pirate fighter",
        "silhouette": (
            "flat wedge-shaped main hull tapering to a pointed nose, "
            "four short swept fins splayed outward, narrow rear engine "
            "block with a glowing central thrust strip"
        ),
        "palette": (
            "glossy white-silver hull with a blue dorsal spine stripe and red "
            "engine-bell accents — match the canonical reference sheet exactly"
        ),
    },
}

DEFAULT_BRIEF: dict[str, str] = {
    "role": "spacecraft",
    "silhouette": "the exact shape shown in the reference image — defer to the reference for every structural detail",
    "palette": "dark gunmetal hull",
}

# ---------------------------------------------------------------------------
# Per-ship canonical reference imagery.
#
# Paths to one or more PNG files showing the ship's CANONICAL look (multiple
# angles, source-of-truth colours/materials). These get attached to every
# sprite generation alongside the per-angle render. The per-angle render
# dictates pose & silhouette; the canonical refs dictate paint scheme,
# material finish, and overall design fidelity.
#
# Paths are repo-relative and resolved at job-build time. Missing files are
# silently skipped (with a one-line warning) so a ship without canonical
# imagery still works — it just falls back to render-only conditioning.
# ---------------------------------------------------------------------------
CANONICAL_REFS: dict[str, tuple[str, ...]] = {
    "talon": ("assets/ships/talon/canonical_reference.png",),
}

# Single source of truth for the per-spec extra_style string. Used by the
# pixelart batch tool to bias both the model and the style classifier.
EXTRA_STYLE = (
    "sleek futuristic spaceship pixel art; glossy polished aerospace alloy "
    "hull (think F-22, B-2, mirrored chrome) with paper-thin panel seams; "
    "clean and showroom-fresh; NEVER stone, brick, mortar, masonry, granite, "
    "or sandstone; preserve silhouette from the primary reference exactly"
)


def resolve_canonical_refs(ship: str, repo_root: Path) -> tuple[Path, ...]:
    """Resolve per-ship canonical reference paths. Missing files are dropped
    with a one-line warning so the rest of the batch still runs — a ship
    without canonical imagery just gets render-only conditioning."""
    out: list[Path] = []
    for rel in CANONICAL_REFS.get(ship, ()):
        p = (repo_root / rel).resolve()
        if p.is_file():
            out.append(p)
        else:
            print(f"  ! canonical ref missing for {ship!r}: {p}", file=sys.stderr)
    return tuple(out)


def build_prompt(ship: str, reference: Path, canonical_refs: tuple[Path, ...] = (),
                 prompt_suffix: str = "") -> str:
    pretty_ship = ship.replace("_", " ").title()
    brief = SHIP_DESIGN_BRIEFS.get(ship, DEFAULT_BRIEF)

    # Reference-images section. The PRIMARY render dictates pose+silhouette;
    # CANONICAL refs (when present) dictate paint scheme & material finish.
    # Splitting them in the prompt prevents the model from copying the
    # canonical's POSE (which would collapse all 80 sprites to the same
    # angle) or the primary's COLOURS (which are stale WCU stone-textures).
    if canonical_refs:
        canon_lines = "\n".join(f"  - {p}" for p in canonical_refs)
        ref_block = (
            f"REFERENCE IMAGES:\n"
            f"  PRIMARY (this exact angle — match silhouette and pose 1:1):\n"
            f"  - {reference}\n"
            f"  CANONICAL DESIGN SHEET (multiple angles — use ONLY for paint scheme,\n"
            f"  hull colour, material finish, and overall design fidelity; do NOT\n"
            f"  copy any specific angle from this sheet):\n"
            f"{canon_lines}"
        )
    else:
        ref_block = (
            f"REFERENCE IMAGE (use directly as image conditioning):\n"
            f"{reference}"
        )

    return f"""Create a pixel-art spaceship sprite using the attached reference image as a strict shape and pose guide.

{ref_block}

Subject: {pretty_ship}-class {brief['role']} from Wing Commander: Privateer.

Requirements:
- Preserve the silhouette, proportions, and exact viewing angle from the reference image.
- Do NOT invent a different ship design.
- Distinguishing features to preserve: {brief['silhouette']}.
- Preserve the outline exactly; do not overly simplify or blur the geometry.
- Keep the ship centered and fully readable as a single clean silhouette.
- Readable sprite first, texture detail second.

Style:
- 1993 DOS / Wing Commander: Privateer style pixel art
- chunky pixels
- hand-authored-looking sprite readability
- dithered shading
- limited palette
- {brief['palette']}
- transparent background

MATERIAL — READ THIS CAREFULLY (most important section):
The PRIMARY reference is a low-fidelity 3D viewport screenshot with placeholder
stone-block textures baked into the model. Those grid-of-rectangles patterns
are a TEXTURING BUG, not a design choice. DO NOT REPRODUCE THEM.

The CANONICAL DESIGN SHEET shows the correct surface finish:
- glossy polished aerospace alloy — think F-22 Raptor, B-2 Spirit, X-wing,
  Cylon Raider, mirrored chrome, brushed titanium
- showroom-fresh painted military hull, clean off the production line
- panel SEAMS are paper-thin scribed lines (1 pixel wide max) — NOT thick
  mortar grooves between blocks
- broad smooth metallic regions with gradient shading from directional lighting
- specular highlights along edges and curved surfaces
- a few crisp painted insignia / numbers / accent stripes (sparse, not
  every panel)

Use the PRIMARY reference for SILHOUETTE / POSE / PROPORTIONS only.
Use the CANONICAL reference for MATERIAL / FINISH / PAINT JOB.
These roles are non-negotiable — never let the primary's bricky textures
leak into your output.

Avoid:
- painterly blur
- mushy silhouette
- rounded organic forms
- inventing extra fins / wings / greebles
- changing the ship proportions
- smearing thin structures
- starfield background or background artifacts
- stone walls, brickwork, masonry, cobblestones, castle/fortress textures
- mortar lines, grout, chiseled stone blocks, ashlar masonry
- weathered/aged/medieval/fantasy/wood-grain styling — this is a SLEEK SPACESHIP
- copying the PRIMARY's surface texture pattern (it is a known bad input)

Target:
- master sprite quality
- pixel grid 512 on longest side
- palette around 40 colors
- transparent background
- large source sprite suitable for later downsampling into in-game atlas frames
""" + (f"\nAdditional per-frame art direction from inspector:\n{prompt_suffix.strip()}\n" if prompt_suffix.strip() else "")


def load_jobs(
    ship: str,
    limit: int | None = None,
    only_az: float | None = None,
    only_el: float | None = None,
    prompt_suffix: str = "",
) -> list[SpriteJob]:
    render_dir = REPO / "assets" / "ships" / ship / "renders"
    sprite_dir = REPO / "assets" / "ships" / ship / "sprites"
    prompt_dir = sprite_dir / "prompts"
    manifest_path = render_dir / "manifest.json"

    with open(manifest_path, "r", encoding="utf-8") as f:
        manifest = json.load(f)

    # Resolve canonical refs once per ship — they're the same for every angle.
    canonical = resolve_canonical_refs(ship, REPO)
    if canonical:
        rels = ", ".join(str(p.relative_to(REPO)) for p in canonical)
        print(f"  canonical refs for {ship!r}: {rels}")

    jobs: list[SpriteJob] = []
    for sample in manifest["samples"]:
        az = float(sample["az"])
        el = float(sample["el"])
        if only_az is not None and int(round(az)) != int(round(only_az)):
            continue
        if only_el is not None and int(round(el)) != int(round(only_el)):
            continue

        ref = render_dir / sample["file"]
        stem = f"{ship}_az{az_tag(az)}_el{angle_tag(el)}_newmodel_512"
        raw = sprite_dir / f"{stem}.png"
        clean = sprite_dir / f"{stem}_clean.png"
        prompt_file = prompt_dir / f"{stem}.txt"
        prompt = build_prompt(ship, ref, canonical, prompt_suffix)
        jobs.append(SpriteJob(ship, az, el, ref, raw, clean, prompt_file, prompt,
                              canonical_refs=canonical))

    return jobs[:limit] if limit else jobs


def write_batch_specs(jobs: list[SpriteJob], spec_path: Path) -> None:
    specs = []
    for job in jobs:
        specs.append({
            "filename": job.raw_output.name,
            "subject": job.prompt,
            "view": "side",            # prompt/reference carries actual angle
            "pixel_grid": 512,
            "palette_size": 40,
            "render_size": 1024,
            "quality": "high",
            "size": "1024x1024",
            "transparent_bg": False,
            "save_raw": True,
            "reference_image": [str(p) for p in job.all_references],
            "reference_strength": "strict",
            "extra_style": EXTRA_STYLE,
        })
    spec_path.parent.mkdir(parents=True, exist_ok=True)
    spec_path.write_text(json.dumps(specs, indent=2), encoding="utf-8")


def write_job_files(jobs: list[SpriteJob], jobs_jsonl: Path) -> None:
    jobs_jsonl.parent.mkdir(parents=True, exist_ok=True)
    for job in jobs:
        job.prompt_file.parent.mkdir(parents=True, exist_ok=True)
        job.prompt_file.write_text(job.prompt, encoding="utf-8")

    with open(jobs_jsonl, "w", encoding="utf-8") as f:
        for job in jobs:
            rec = {
                "ship": job.ship,
                "az": job.az,
                "el": job.el,
                "reference": str(job.reference),
                "canonical_refs": [str(p) for p in job.canonical_refs],
                "output": str(job.raw_output),
                "clean_output": str(job.clean_output),
                "prompt_file": str(job.prompt_file),
            }
            f.write(json.dumps(rec) + "\n")


def run_generator(job: SpriteJob, cmd_template: str) -> None:
    job.raw_output.parent.mkdir(parents=True, exist_ok=True)
    mapping = {
        "reference": shlex.quote(str(job.reference)),
        "output": shlex.quote(str(job.raw_output)),
        "prompt_file": shlex.quote(str(job.prompt_file)),
        "prompt_json": shlex.quote(json.dumps(job.prompt)),
    }
    cmd = cmd_template.format(**mapping)
    print(f"  gen: {cmd}")
    subprocess.run(cmd, shell=True, check=True, cwd=REPO)


def _load_pixelart_batch_generate():
    spec = importlib.util.spec_from_file_location("_pixelart_batch_generate", PIXELART_BATCH)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Could not load pixelart batch tool at {PIXELART_BATCH}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.batch_generate


_GENERATE_SPRITE_LOCK = threading.Lock()
_GENERATE_SPRITE = None


def _load_pixelart_generate_sprite():
    # Lazy-load once per process. The function itself is stateless; each call
    # creates its own httpx.Client and PIL images, so ThreadPoolExecutor is fine.
    # Lock only protects first import because importlib is not our concurrency
    # playground. Keep the dragons caged.
    global _GENERATE_SPRITE
    with _GENERATE_SPRITE_LOCK:
        if _GENERATE_SPRITE is not None:
            return _GENERATE_SPRITE
        spec = importlib.util.spec_from_file_location("_pixelart_generate_sprite", PIXELART_GENERATE)
        if spec is None or spec.loader is None:
            raise RuntimeError(f"Could not load pixelart tool at {PIXELART_GENERATE}")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        _GENERATE_SPRITE = module.generate_sprite
        return _GENERATE_SPRITE


def run_pixelart_tool(job: SpriteJob, quality: str) -> None:
    """Call the local UC pixelart.generate_sprite plugin directly.

    This is the real path for Mike's updated tool. We bypass pixelart.batch_generate
    because that wrapper currently does not pass reference_image through, which is
    adorable and useless for reference-conditioned sprites. Direct call = no lost
    parameters, fewer mysteries. DRY would prefer fixing the plugin wrapper too,
    but this repo tool shouldn't mutate global Code Puppy plugins unless asked.
    """
    generate_sprite = _load_pixelart_generate_sprite()
    job.raw_output.parent.mkdir(parents=True, exist_ok=True)
    print(f"  pixelart: {job.reference.name} -> {job.raw_output.name}")
    result = generate_sprite(
        subject=job.prompt,
        output_path=str(job.raw_output),
        view="side",                 # prompt/reference carries actual angle
        pixel_grid=512,
        palette_size=40,
        render_size=1024,
        transparent_bg=False,
        size="1024x1024",
        quality=quality,
        extra_style=EXTRA_STYLE,
        save_raw=True,
        reference_image=[str(p) for p in job.all_references],
        reference_strength="strict",
        timeout=240.0,
    )
    print(f"  ok: {result.get('elapsed_seconds', 0):.1f}s  {result.get('final_dimensions')}")


def run_pixelart_parallel(
    jobs: list[SpriteJob],
    quality: str,
    skip_existing: bool,
    workers: int,
    clean: bool,
    preview: bool,
) -> None:
    todo = [j for j in jobs if not (skip_existing and j.raw_output.exists())]
    skipped = len(jobs) - len(todo)
    print(f"parallel: {len(todo)} to generate, {skipped} skipped, workers={workers}")

    def one(job: SpriteJob) -> tuple[str, str, str | None]:
        try:
            run_pixelart_tool(job, quality)
            if clean:
                clean_sprite(job, preview)
            return (job.raw_output.name, "ok", None)
        except Exception as exc:  # noqa: BLE001 - batch must report and continue
            return (job.raw_output.name, "failed", f"{type(exc).__name__}: {exc}")

    ok = failed = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
        futures = {pool.submit(one, job): job for job in todo}
        for fut in concurrent.futures.as_completed(futures):
            name, status, err = fut.result()
            if status == "ok":
                ok += 1
                print(f"  OK     {name}")
            else:
                failed += 1
                print(f"  FAIL   {name}: {err}")

    if clean and skipped:
        # Existing raw outputs may not have clean variants yet. Cheap to ensure.
        for job in jobs:
            if job.raw_output.exists() and not job.clean_output.exists():
                clean_sprite(job, preview)

    print(json.dumps({
        "total": len(jobs),
        "generated": ok,
        "skipped": skipped,
        "failed": failed,
        "workers": workers,
    }, indent=2))


def run_pixelart_batch(jobs: list[SpriteJob], quality: str, skip_existing: bool) -> None:
    batch_generate = _load_pixelart_batch_generate()
    if not jobs:
        return
    specs = []
    for job in jobs:
        specs.append({
            "filename": job.raw_output.name,
            "subject": job.prompt,
            "view": "side",
            "pixel_grid": 512,
            "palette_size": 40,
            "render_size": 1024,
            "quality": quality,
            "size": "1024x1024",
            "transparent_bg": False,
            "save_raw": True,
            "reference_image": [str(p) for p in job.all_references],
            "reference_strength": "strict",
            "extra_style": EXTRA_STYLE,
        })
    out_dir = str(jobs[0].raw_output.parent)
    manifest = batch_generate(
        specs=specs,
        output_dir=out_dir,
        log_path=str(Path(out_dir) / "batch_log.txt"),
        skip_existing=skip_existing,
        default_view="side",
        default_pixel_grid=512,
        default_palette_size=40,
        default_quality=quality,
        default_extra_style=EXTRA_STYLE,
        stop_on_error=False,
    )
    print(json.dumps({k: manifest[k] for k in ["total", "generated", "skipped", "failed", "elapsed_seconds"]}, indent=2))


def clean_sprite(job: SpriteJob, preview: bool) -> None:
    cleaner = REPO / "tools" / "clean_sprite_alpha.py"
    cmd = [sys.executable, str(cleaner), str(job.raw_output), str(job.clean_output)]
    if preview:
        cmd.append("--preview")
    print(f"  clean: {' '.join(shlex.quote(c) for c in cmd)}")
    subprocess.run(cmd, check=True, cwd=REPO)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--ship", default="tarsus")
    ap.add_argument("--limit", type=int, default=None)
    ap.add_argument("--az", type=float, default=None,
                    help="Generate only the nearest authored azimuth angle, e.g. 270")
    ap.add_argument("--el", type=float, default=None,
                    help="Generate only the nearest authored elevation angle, e.g. -22")
    ap.add_argument("--prompt-suffix", default="",
                    help="Extra art direction appended to every selected frame prompt")
    ap.add_argument("--prompt-suffix-file", type=Path, default=None,
                    help="Read extra art direction from a text file and append it to every selected frame prompt")
    ap.add_argument("--jobs-jsonl", type=Path, default=None)
    ap.add_argument("--use-pixelart-tool", action="store_true",
                    help="Call local UC pixelart.generate_sprite directly with reference_image")
    ap.add_argument("--use-pixelart-batch", action="store_true",
                    help="Call local UC pixelart.batch_generate with reference_image-aware specs")
    ap.add_argument("--quality", default="high", choices=["low", "medium", "high", "auto"],
                    help="quality passed to pixelart.generate_sprite")
    ap.add_argument("--parallel", type=int, default=1,
                    help="Generate with N concurrent pixelart.generate_sprite calls. Use 2-4 unless you enjoy billing demons.")
    ap.add_argument("--generator-cmd", default=None,
                    help="Fallback command template using {reference}, {output}, {prompt_file}, {prompt_json}")
    ap.add_argument("--clean", action="store_true", help="Run alpha cleaner after generation")
    ap.add_argument("--preview", action="store_true", help="When cleaning, also write *_on_black.png")
    ap.add_argument("--force", action="store_true", help="Regenerate even if raw output exists")
    args = ap.parse_args()

    prompt_suffix = args.prompt_suffix
    if args.prompt_suffix_file:
        prompt_suffix += "\n" + args.prompt_suffix_file.read_text(encoding="utf-8")

    jobs = load_jobs(
        args.ship,
        args.limit,
        only_az=args.az,
        only_el=args.el,
        prompt_suffix=prompt_suffix,
    )
    jobs_jsonl = args.jobs_jsonl or (REPO / "assets" / "ships" / args.ship / "sprites" / "jobs.jsonl")
    write_job_files(jobs, jobs_jsonl)
    batch_specs = jobs_jsonl.with_name("pixelart_batch_specs.json")
    write_batch_specs(jobs, batch_specs)

    print(f"jobs: {len(jobs)}")
    print(f"jobs jsonl: {jobs_jsonl}")
    print(f"pixelart batch specs: {batch_specs}")
    print("prompt dir:", jobs[0].prompt_file.parent if jobs else "(none)")

    if args.parallel < 1:
        raise ValueError("--parallel must be >= 1")

    if not args.generator_cmd and not args.use_pixelart_tool and not args.use_pixelart_batch:
        print("\nNo generator selected; wrote prompts/jobs/specs only. Good dog, no API assumptions.")
        print("\nRecommended batch mode:")
        print("  python3 tools/batch_generate_ship_sprites.py --ship tarsus --use-pixelart-batch --clean --preview")
        return

    if args.use_pixelart_batch:
        if args.parallel > 1:
            # The UC batch wrapper is sequential. For real parallelism we call
            # generate_sprite directly per job, but keep this flag as the user's
            # intended mode because it's still semantically "batch generation".
            run_pixelart_parallel(
                jobs,
                args.quality,
                skip_existing=not args.force,
                workers=args.parallel,
                clean=args.clean,
                preview=args.preview,
            )
        else:
            run_pixelart_batch(jobs, args.quality, skip_existing=not args.force)
            if args.clean:
                for job in jobs:
                    if job.raw_output.exists():
                        clean_sprite(job, args.preview)
        return

    for i, job in enumerate(jobs, 1):
        print(f"\n[{i}/{len(jobs)}] az={job.az:03.0f} el={job.el:+05.1f}")
        if job.raw_output.exists() and not args.force:
            print(f"  skip existing: {job.raw_output}")
        else:
            if args.use_pixelart_tool:
                run_pixelart_tool(job, args.quality)
            else:
                run_generator(job, args.generator_cmd)

        if args.clean:
            if not job.raw_output.exists():
                raise FileNotFoundError(f"generator did not create {job.raw_output}")
            clean_sprite(job, args.preview)


if __name__ == "__main__":
    main()

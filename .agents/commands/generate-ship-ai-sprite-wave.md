# Generate Ship AI Sprite Wave

## Purpose

Generate a full AI-created multi-view sprite atlas for any ship using the clean in-engine reference renders.

The pipeline:

1. read `assets/ships/$SHIP/renders/manifest.json`
2. generate one AI sprite per reference vantage point
3. clean alpha/background
4. review generated sprites in contact sheets
5. build `atlas_manifest.json`
6. center all frames into engine-ready cells
7. optionally feather cell edges
8. review final cells

## Parameters

Fill these before running:

```txt
SHIP=<ship slug>                  # e.g. centurion, talon, galaxy, tarsus
QUALITY=high                      # low, medium, high, auto
PARALLEL=2                        # 1 safest; 2-4 faster but pricier/riskier
PROMPT_SUFFIX_FILE=/tmp/${SHIP}_prompt_suffix.txt  # optional but recommended
```

The generator expects clean reference renders here:

```txt
assets/ships/$SHIP/renders/manifest.json
assets/ships/$SHIP/renders/${SHIP}_az*_el*.png
```

If those do not exist or are bad, run:

```txt
.agents/commands/regenerate-ship-reference-renders.md
```

first. Do not generate sprites from bad refs. That is just industrial-scale wrongness.

## Guardrails

- Do **not** start with the full wave. Generate one frame first.
- Use `--force` when replacing stale experiments.
- Do not feather until you are happy with all clean generated frames.
- Do not stage/commit failed attempts unless explicitly asked.
- Keep review contact sheets; they are cheap QA and save eyeballs.
- Watch API costs. `82 × high quality` is not a snack.

## API key

The local pixel art tool uses OpenAI image generation:

```bash
export OPENAI_API_KEY="your_key_here"
```

If this is missing, generation fails before producing sprites.

## Canonical references

The batch tool may attach ship-specific canonical design sheets from `CANONICAL_REFS` in:

```txt
tools/batch_generate_ship_sprites.py
```

Typical path:

```txt
assets/ships/$SHIP/canonical_reference.png
```

Before full generation, inspect the canonical reference if one exists. A bad canonical sheet poisons every generated frame. Delightful. Terrible. Avoid.

## Steps

### 1. Define variables

Example:

```bash
SHIP=centurion
QUALITY=high
PARALLEL=2
PROMPT_SUFFIX_FILE="/tmp/${SHIP}_prompt_suffix.txt"
```

### 2. Check reference render count

```bash
python3 - <<PY
import json
from pathlib import Path
ship = '$SHIP'
p = Path(f'assets/ships/{ship}/renders/manifest.json')
if not p.exists():
    raise SystemExit(f'missing {p}')
m = json.loads(p.read_text())
print('ship', m.get('ship'))
print('samples', len(m.get('samples', [])))
print('radius', m.get('orbit_radius'))
print('elevations', m.get('elevations'))
PY
```

For the standard grid, expect 82 samples.

### 3. Optional prompt suffix

Create a ship-specific prompt suffix if the base prompt needs more design discipline.

Example for a heavy fighter:

```bash
cat > "$PROMPT_SUFFIX_FILE" <<'EOF'
Preserve the ship's exact silhouette and proportions from the primary reference angle.
Keep every angle consistent as the same spacecraft rotating through the atlas.
Use the canonical reference only for paint, material finish, and markings.
Do not add extra wings, fins, antennae, weapon pods, or modules not visible in the reference.
Do not collapse the requested 3D view into a generic side, top, or front view.
EOF
```

Customize this per ship. Do not hard-code Centurion/Talon/Galaxy details into a generic run unless that is the ship being generated. The DRY goblin approves.

### 4. Generate one test frame

Pick a representative angle. `az 45 / el 0` is a good side-ish smoke test for many ships:

```bash
python3 tools/batch_generate_ship_sprites.py \
  --ship "$SHIP" \
  --az 45 \
  --el 0 \
  --prompt-suffix-file "$PROMPT_SUFFIX_FILE" \
  --use-pixelart-tool \
  --quality "$QUALITY" \
  --clean \
  --preview \
  --force
```

If not using a suffix, remove:

```txt
--prompt-suffix-file "$PROMPT_SUFFIX_FILE"
```

Review the preview:

```txt
assets/ships/$SHIP/sprites/${SHIP}_az045_el+000_newmodel_512_clean_on_black.png
```

Expected:

- silhouette matches the reference angle
- no invented geometry
- background cleaned properly
- sprite is readable as pixel art
- no painterly mush
- no starfield/background artifacts
- ship identity is preserved

If it is bad, adjust the prompt suffix or canonical/reference inputs before continuing.

### 5. Generate a small ring

Generate the equator ring before the full wave:

```bash
python3 tools/batch_generate_ship_sprites.py \
  --ship "$SHIP" \
  --el 0 \
  --prompt-suffix-file "$PROMPT_SUFFIX_FILE" \
  --use-pixelart-batch \
  --parallel "$PARALLEL" \
  --quality "$QUALITY" \
  --clean \
  --preview \
  --force
```

If not using a suffix, remove the suffix argument.

Make an equator contact sheet:

```bash
mkdir -p "assets/ships/$SHIP/contact_sheets"
python3 tools/make_sprite_contact_sheet.py \
  --src "assets/ships/$SHIP/sprites" \
  --suffix _newmodel_512_clean.png \
  --out "assets/ships/$SHIP/contact_sheets/${SHIP}_ai_clean_equator.png" \
  --cell 160 \
  --title "$SHIP AI clean sprites - equator"
```

Review:

```txt
assets/ships/$SHIP/contact_sheets/${SHIP}_ai_clean_equator.png
```

Look for:

- smooth rotation from angle to angle
- stable ship proportions
- stable paint scheme
- no side/top/front view collapse
- no swapped top/bottom logic
- no mutation into another ship

### 6. Generate the full wave

Only after the one-frame test and equator ring pass QA:

```bash
python3 tools/batch_generate_ship_sprites.py \
  --ship "$SHIP" \
  --prompt-suffix-file "$PROMPT_SUFFIX_FILE" \
  --use-pixelart-batch \
  --parallel "$PARALLEL" \
  --quality "$QUALITY" \
  --clean \
  --preview \
  --force
```

If not using a suffix, remove the suffix argument.

For the standard grid, this attempts 82 sprites.

Primary generated files:

```txt
assets/ships/$SHIP/sprites/*_newmodel_512.png
assets/ships/$SHIP/sprites/*_newmodel_512.raw.png
assets/ships/$SHIP/sprites/*_newmodel_512_clean.png
assets/ships/$SHIP/sprites/*_newmodel_512_clean_on_black.png
```

The `*_clean.png` files are the human-upright cleaned sprite outputs used for atlas-building. The `*_clean_on_black.png` files are review previews.

### 7. Build full clean sprite contact sheets

```bash
python3 tools/make_sprite_contact_sheet.py \
  --src "assets/ships/$SHIP/sprites" \
  --suffix _newmodel_512_clean.png \
  --out "assets/ships/$SHIP/contact_sheets/${SHIP}_ai_clean_contact.png" \
  --cell 160 \
  --title "$SHIP AI clean sprites" \
  --rows-per-sheet 8
```

Review generated sheets:

```txt
assets/ships/$SHIP/contact_sheets/${SHIP}_ai_clean_contact_part1.png
assets/ships/$SHIP/contact_sheets/${SHIP}_ai_clean_contact_part2.png
```

QA before proceeding:

- [ ] every required angle exists
- [ ] ship identity is consistent
- [ ] silhouette follows the corresponding render angle
- [ ] no top/bottom swaps
- [ ] no side/top/front collapses
- [ ] no background junk
- [ ] no horrible alpha holes
- [ ] no frame is wildly larger/smaller than neighbors

### 8. Retry individual bad frames

If one frame is bad, regenerate only that slot:

```bash
python3 tools/batch_generate_ship_sprites.py \
  --ship "$SHIP" \
  --az 225 \
  --el 30 \
  --prompt-suffix-file "$PROMPT_SUFFIX_FILE" \
  --use-pixelart-tool \
  --quality "$QUALITY" \
  --clean \
  --preview \
  --force
```

Change `--az`/`--el` to the bad frame. Then rebuild the clean contact sheets.

### 9. Build engine atlas manifest

Once clean sprites pass review:

```bash
python3 tools/build_ship_atlas_manifest.py --ship "$SHIP"
```

This writes/updates:

```txt
assets/ships/$SHIP/atlas_manifest.json
```

Frames missing from disk are dropped with warnings. Do not ignore warnings unless partial atlas generation is intentional.

### 10. Center frames into engine cells

Generated sprites are cropped per frame. That is fine for PNG hygiene and awful for runtime view-sphere atlases because the ship appears to slide/scale while rotating.

Normalize them into shared-size transparent cells:

```bash
python3 tools/center_ship_atlas_cells.py \
  --ship "$SHIP" \
  --preview
```

This writes:

```txt
assets/ships/$SHIP/sprites/*_newmodel_512_clean_cell.png
assets/ships/$SHIP/sprites/*_newmodel_512_clean_cell_on_black.png
```

and updates:

```txt
assets/ships/$SHIP/atlas_manifest.json
```

Important: this tool vertically flips engine-facing cells by default. That is intentional. The human-reviewed `*_clean.png` files remain upright; the `*_cell.png` files are engine-ready.

### 11. Feather cell edges

Run a dry-run first:

```bash
python3 tools/feather_sprite_edges.py "$SHIP" --dry-run
```

Review:

```txt
/tmp/np_feather_preview.png
```

If good:

```bash
python3 tools/feather_sprite_edges.py "$SHIP"
```

Do not run feathering twice unless you mean to compound the blur. Double-feathered sprites look like they had a mild allergic reaction.

If the sentinel already exists and you truly need to redo feathering, restore the original cell PNGs from git first, delete the sentinel, then run again. Do not casually use `--force` on already-feathered files.

### 12. Build final engine-cell contact sheets

```bash
python3 tools/make_sprite_contact_sheet.py \
  --src "assets/ships/$SHIP/sprites" \
  --suffix _newmodel_512_clean_cell.png \
  --out "assets/ships/$SHIP/contact_sheets/${SHIP}_ai_cells_contact.png" \
  --cell 160 \
  --title "$SHIP AI engine cells" \
  --rows-per-sheet 8
```

Review:

```txt
assets/ships/$SHIP/contact_sheets/${SHIP}_ai_cells_contact_part1.png
assets/ships/$SHIP/contact_sheets/${SHIP}_ai_cells_contact_part2.png
```

This is the best preview of what the engine-facing atlas will use.

### 13. Optional in-game check

Run the game and inspect the ship atlas/scene:

```bash
cmake --build build -j8
./build/new_privateer
```

Useful debug UI:

```txt
F4  Atlas Grid Viewer
F3  Ship-frame HUD
F2  Sprite Light Editor
```

### 14. Commit

Once happy:

```bash
git add "assets/ships/$SHIP/sprites" \
        "assets/ships/$SHIP/atlas_manifest.json" \
        "assets/ships/$SHIP/contact_sheets"

git commit -m "$SHIP: generate AI sprite atlas"
```

## Troubleshooting

### Generator skipped frames

Old raw outputs exist and `--force` was omitted. Re-run with `--force`.

### No API key

Set:

```bash
export OPENAI_API_KEY="your_key_here"
```

### Sprite has background junk

Make sure `--clean --preview` is used. Also check that the prompt says transparent/no background and that references are clean.

### Shape ignores angle

Use a stricter prompt suffix. Regenerate only the bad frame first, not the whole wave.

### Whole ship mutates between frames

The canonical reference or prompt is too loose/ambiguous. Tighten ship identity details, then rerun a small ring.

### Top/bottom appears swapped

First check the clean human-upright `*_clean.png` contact sheet. Then check engine-facing `*_cell.png` after `center_ship_atlas_cells.py`; that tool intentionally flips cells for runtime texture conventions.

### Runtime ship wobbles/scales

You probably forgot:

```bash
python3 tools/center_ship_atlas_cells.py --ship "$SHIP" --preview
```

## QA checklist

- [ ] clean refs exist and look correct
- [ ] canonical reference checked if present
- [ ] one-frame AI test accepted
- [ ] equator ring accepted
- [ ] full clean contact sheets accepted
- [ ] bad frames regenerated individually
- [ ] atlas manifest built
- [ ] cells centered
- [ ] edge feather dry-run checked
- [ ] edge feather applied once
- [ ] final cell contact sheets accepted
- [ ] optional in-game atlas check passed
- [ ] only intended generated assets staged

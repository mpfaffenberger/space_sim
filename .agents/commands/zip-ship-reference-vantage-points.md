# Zip Ship Reference Vantage Points

## Purpose

Create a zip archive containing a ship's clean reference vantage-point renders, render manifest, and reference contact sheets.

Useful for sharing the reference set with another tool/person/agent without including generated AI sprite outputs.

## Parameters

Fill this before running:

```txt
SHIP=<ship slug>  # e.g. centurion, talon, galaxy, tarsus
```

Output:

```txt
assets/ships/$SHIP/${SHIP}_clean_reference_vantage_points.zip
```

## Guardrails

- Include reference renders only, not AI generated sprites.
- Include `renders/manifest.json`.
- Include reference contact sheets if they exist.
- Verify zip contents with `unzip -l`.

## Steps

### 1. Define the ship

```bash
SHIP=centurion
```

### 2. Create the zip

```bash
python3 - <<PY
from pathlib import Path
from zipfile import ZipFile, ZIP_DEFLATED

ship = '$SHIP'
root = Path('assets/ships') / ship
out = root / f'{ship}_clean_reference_vantage_points.zip'

files = []
files += sorted((root / 'renders').glob(f'{ship}_*.png'))
files.append(root / 'renders' / 'manifest.json')
files += sorted((root / 'contact_sheets').glob(f'{ship}_reference_renders_contact_part*.png'))

missing = [p for p in files if not p.exists()]
if missing:
    raise SystemExit('missing files:\n' + '\n'.join(str(p) for p in missing))

if out.exists():
    out.unlink()

with ZipFile(out, 'w', ZIP_DEFLATED) as z:
    for p in files:
        z.write(p, p.relative_to(root))

print(f'zip: {out}')
print(f'files: {len(files)}')
print(f'render pngs: {len(list((root / "renders").glob(f"{ship}_*.png")))}')
print(f'size_bytes: {out.stat().st_size}')
PY
```

### 3. Verify contents

```bash
ls -lh "assets/ships/$SHIP/${SHIP}_clean_reference_vantage_points.zip"

unzip -l "assets/ships/$SHIP/${SHIP}_clean_reference_vantage_points.zip" | sed -n '1,24p'
echo '...'
unzip -l "assets/ships/$SHIP/${SHIP}_clean_reference_vantage_points.zip" | tail -10
```

Expected contents:

```txt
renders/<ship>_az*_el*.png
renders/manifest.json
contact_sheets/<ship>_reference_renders_contact_part*.png
```

## QA checklist

- [ ] zip exists
- [ ] zip contains render PNGs
- [ ] zip contains `renders/manifest.json`
- [ ] zip contains reference contact sheets if generated
- [ ] zip does not contain `sprites/`
- [ ] zip does not contain AI generated sprite outputs

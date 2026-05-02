# Regenerate Ship Clean Reference Renders

## Purpose

Regenerate clean in-engine reference vantage-point renders for any ship.

These renders are used as image-conditioning inputs for AI sprite generation. They should show the target ship's clay/model silhouette from the authored view sphere, with no gameplay/HUD/background junk.

## Parameters

Fill these before running:

```txt
SHIP=<ship slug>                  # e.g. centurion, talon, galaxy, tarsus
SYSTEM=<atlas capture system>     # e.g. centurion_atlas, talon_atlas, galaxy_atlas, tarsus_atlas
RADIUS=<camera orbit radius>      # e.g. 85 for centurion, ship-dependent
AZ_COUNT=16                       # usually 16 for 22.5-degree steps
ELEVATIONS=-90,-60,-30,0,30,60,90 # 82-view grid: poles + five rings
SETTLE=0.15                       # seconds after camera move before screenshot
```

Recommended output paths:

```txt
assets/ships/$SHIP/renders/
assets/ships/$SHIP/contact_sheets/
```

## Guardrails

- This command does **not** call AI. It only captures reference renders.
- Always use an atlas/capture scene with the target ship placed at world origin.
- Always launch the game with `--capture-clean`.
- Always build first if capture-clean/rendering code changed.
- Always make a local backup before overwriting refs.
- Always spot-check at least one side-ish view and the pole views.
- Keep generated AI sprite files out of this commit unless explicitly requested.

## Preconditions

The capture system should exist:

```txt
assets/systems/$SYSTEM.json
```

It should contain one target mesh/sprite at or near origin. For mesh capture scenes, prefer:

```jsonc
{
  "studio_lighting": true,
  "placed_meshes": [
    {
      "obj": "meshes/ships/<ship>.obj",
      "position": [0, 0, 0],
      "length_meters": 30,
      "clay_mode": true
    }
  ]
}
```

Exact mesh path/orientation/scale are ship-specific. Do not cargo-cult those values. Cargo culting is how you get a very expensive potato atlas.

## Steps

### 1. Define variables

Example:

```bash
SHIP=centurion
SYSTEM=centurion_atlas
RADIUS=85
AZ_COUNT=16
ELEVATIONS=-90,-60,-30,0,30,60,90
SETTLE=0.15
```

### 2. Back up the current render set

```bash
STAMP=$(date +%Y%m%d_%H%M%S)
BACKUP="/tmp/${SHIP}_renders_backup_$STAMP"
mkdir -p "$BACKUP"
cp -a "assets/ships/$SHIP/renders/." "$BACKUP"/
echo "$BACKUP" > "/tmp/${SHIP}_renders_latest_backup.txt"
echo "backup=$BACKUP"
```

If this is a brand-new ship with no `renders/` directory yet:

```bash
mkdir -p "assets/ships/$SHIP/renders"
```

### 3. Build the game

```bash
cmake --build build -j8
```

### 4. Launch the clean capture scene

Stop any old running game first:

```bash
if [ -f /tmp/new_privateer_game.pid ]; then
  OLD=$(cat /tmp/new_privateer_game.pid)
  kill "$OLD" 2>/dev/null || true
  sleep 1
  kill -9 "$OLD" 2>/dev/null || true
fi
```

Launch the requested capture scene:

```bash
./build/new_privateer --system "$SYSTEM" --capture-clean \
  > "/tmp/new_privateer_${SHIP}_capture.log" 2>&1 &
echo $! > /tmp/new_privateer_game.pid
sleep 3
```

Verify it is alive:

```bash
PID=$(cat /tmp/new_privateer_game.pid)
ps -p "$PID" -o pid,etime,command
tail -30 "/tmp/new_privateer_${SHIP}_capture.log"
```

Expected log hints:

```txt
[system] loaded 'assets/systems/<SYSTEM>.json'
[main] capture-clean studio sun enabled; bloom/flare disabled
[dev_remote] listening on 127.0.0.1:47001
```

### 5. Optional one-ring sanity test

Before overwriting the full set, capture just the equator into `/tmp`:

```bash
rm -rf "/tmp/${SHIP}_ref_test_clean"
python3 tools/render_ship_atlas.py \
  --ship "$SHIP" \
  --radius "$RADIUS" \
  --outdir "/tmp/${SHIP}_ref_test_clean" \
  --az-count "$AZ_COUNT" \
  --elevations 0 \
  --settle "$SETTLE"
```

Open a representative image, usually one of:

```txt
/tmp/$SHIP_ref_test_clean/${SHIP}_az045_el+000.png
/tmp/$SHIP_ref_test_clean/${SHIP}_az315_el+000.png
```

Expected:

- target ship visible and centered enough
- flat black background
- no crosshair/HUD
- no starfield
- no flare specks
- orientation is plausible

If the ship is too small or clipped, adjust `RADIUS` or the capture scene scale/orientation before continuing.

### 6. Regenerate the full reference set

```bash
python3 tools/render_ship_atlas.py \
  --ship "$SHIP" \
  --radius "$RADIUS" \
  --outdir "assets/ships/$SHIP/renders" \
  --az-count "$AZ_COUNT" \
  --elevations="$ELEVATIONS" \
  --settle "$SETTLE"
```

For the standard grid, expected result is 82 samples:

```txt
16 azimuths × 5 non-pole elevation rings + 2 pole views = 82
```

### 7. Verify manifest count

```bash
python3 - <<PY
import json
from pathlib import Path
ship = '$SHIP'
m = json.loads(Path(f'assets/ships/{ship}/renders/manifest.json').read_text())
print('ship', m.get('ship'))
print('count', m.get('count'), 'samples', len(m['samples']))
print('radius', m.get('orbit_radius'))
print('azimuths', m.get('azimuths'))
print('elevations', m.get('elevations'))
print('first', m['samples'][:3])
print('last', m['samples'][-3:])
PY
```

### 8. Spot-check important views

Open/check representative files. For the standard grid these usually exist:

```txt
assets/ships/$SHIP/renders/${SHIP}_az000_el-090.png
assets/ships/$SHIP/renders/${SHIP}_az000_el+090.png
assets/ships/$SHIP/renders/${SHIP}_az045_el+000.png
assets/ships/$SHIP/renders/${SHIP}_az315_el+000.png
```

Expected:

- pole views are sane and centered
- side-ish views are upright for human review
- no HUD/crosshair
- no starfield
- no lens flare ghosts
- ship shape is fully visible

### 9. Build reference contact sheets

```bash
mkdir -p "assets/ships/$SHIP/contact_sheets"
rm -f "assets/ships/$SHIP/contact_sheets/${SHIP}_reference_renders_contact_part"*.png

python3 tools/make_sprite_contact_sheet.py \
  --src "assets/ships/$SHIP/renders" \
  --suffix .png \
  --out "assets/ships/$SHIP/contact_sheets/${SHIP}_reference_renders_contact.png" \
  --cell 160 \
  --title "$SHIP regenerated clean reference renders" \
  --rows-per-sheet 8
```

Review:

```txt
assets/ships/$SHIP/contact_sheets/${SHIP}_reference_renders_contact_part1.png
assets/ships/$SHIP/contact_sheets/${SHIP}_reference_renders_contact_part2.png
```

Blank cells in pole columns are expected when the render manifest captures only one canonical azimuth at `el ±90`.

### 10. Optional zip for sharing

See `.agents/commands/zip-ship-reference-vantage-points.md`.

### 11. Commit

Stage only the intended reference work and any engine capture-clean changes:

```bash
git add src/main.cpp \
        "assets/ships/$SHIP/renders" \
        "assets/ships/$SHIP/contact_sheets"

git commit -m "$SHIP: regenerate clean reference renders"
```

If only assets changed, omit `src/main.cpp`.

## QA checklist

- [ ] `cmake --build build -j8` passes
- [ ] capture scene uses the requested `$SYSTEM`
- [ ] game log says capture-clean is enabled
- [ ] render PNG count matches expected grid
- [ ] `manifest.json` sample count is correct
- [ ] side-ish views are upright/plausible
- [ ] top/bottom pole views are sane
- [ ] no crosshair
- [ ] no starfield
- [ ] no lens flare ghosts
- [ ] contact sheets generated
- [ ] no stale AI sprite files staged by accident

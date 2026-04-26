#!/usr/bin/env bash
# tour_ship_sprite_atlas.sh — one-shot debug tour of a ship's sprite atlas.
#
# Boots the game in <ship>_sprite_atlas mode, orbits the camera through the
# 80-cell viewing sphere, screenshots what the live engine actually picks at
# each (az, el), and builds a render-vs-ingame comparison grid.
#
# Why this exists:
#   Spotting orientation/cell-mapping bugs by flying around in-game is brutal —
#   strafing changes both az and el, the eye normalises small mismatches, and
#   you can never quite hit the exact bin you want to inspect. A static 80-cell
#   grid pinned to the same orbit positions both the renderer and the engine
#   use makes mistakes obvious in seconds.
#
# Usage:
#   tools/tour_ship_sprite_atlas.sh talon
#
# Prereqs:
#   - assets/systems/<ship>_sprite_atlas.json exists
#     (contains a single placed_ship_sprite at world origin)
#   - assets/ships/<ship>/renders/ already populated (ground-truth mesh renders)
#   - build/new_privateer is built and runs

set -euo pipefail

SHIP="${1:-talon}"
RADIUS="${2:-35}"
SETTLE="${3:-0.25}"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

SYSTEM_FILE="assets/systems/${SHIP}_sprite_atlas.json"
RENDERS_DIR="assets/ships/${SHIP}/renders"
INGAME_DIR="assets/ships/${SHIP}/sprites_ingame"

[[ -f "$SYSTEM_FILE" ]] || { echo "missing $SYSTEM_FILE — create it first (see assets/systems/talon_sprite_atlas.json for template)"; exit 1; }
[[ -d "$RENDERS_DIR" ]] || { echo "missing $RENDERS_DIR — generate ground-truth renders first via tools/render_ship_atlas.py"; exit 1; }

mkdir -p "$INGAME_DIR"

echo "==> killing any running new_privateer"
pkill -f "build/new_privateer" 2>/dev/null || true
sleep 1

echo "==> booting game (system=${SHIP}_sprite_atlas, capture-clean)"
./build/new_privateer --system "${SHIP}_sprite_atlas" --capture-clean > "/tmp/np_tour_${SHIP}.log" 2>&1 &
GAME_PID=$!
sleep 4

if ! ps -p "$GAME_PID" > /dev/null; then
    echo "✗ game exited; tail of log:"
    tail -20 "/tmp/np_tour_${SHIP}.log"
    exit 1
fi

echo "==> orbiting + screenshotting (80 cells, ~1 minute)"
uv run --quiet --with pillow python3 tools/render_ship_atlas.py \
    --ship "$SHIP" \
    --radius "$RADIUS" \
    --outdir "$INGAME_DIR" \
    --az-count 16 \
    --elevations="-60,-30,0,30,60" \
    --settle "$SETTLE" 2>&1 | tail -3

echo "==> building comparison sheet"
uv run --quiet --with pillow python3 tools/compare_sprite_atlas_to_render.py --ship "$SHIP"

echo "==> game still running on PID $GAME_PID; kill when done with: pkill -f build/new_privateer"
echo "==> comparison sheet: /tmp/diag/${SHIP}_render_vs_ingame.png"

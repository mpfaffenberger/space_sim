#!/usr/bin/env bash
# -----------------------------------------------------------------------------
# gen_skybox.sh — regenerate a skybox cubemap for a given seed.
#
# Usage:
#   tools/gen_skybox.sh <seed> [options...]
#
# Options forwarded to skyboxgen (see `skyboxgen --help` for all of them):
#   --resolution N           Face resolution (default 1024)
#   --nebula-count N         Force N nebulae (default: random 1-5)
#   --nebula-intensity F     Brightness multiplier (default 1.0)
#   --nebula-falloff F       Coverage (LOWER = denser clouds; default 1.0)
#   --nebula-scale F         Detail multiplier (default 1.0)
#   --no-stars / --no-nebulae / --no-sun
#   --sun-dir X,Y,Z
#
# Presets (eyeballed to actually look good, not just be bright):
#   --rich      Dramatic but readable: 3 nebulae, moderate boost. <- sweet spot
#   --intense   Alias for --rich, kept for muscle memory.
#   --wild      Unhinged: 5 layers + 2x intensity. The sky is now plaid.
#   --sparse    Thin wisps, lots of black space.
#
# Examples:
#   tools/gen_skybox.sh troy
#   tools/gen_skybox.sh purple_haze --intense
#   tools/gen_skybox.sh clean_space --no-nebulae
#   tools/gen_skybox.sh gas_giant_zone --nebula-count 5 --nebula-intensity 3 \
#                                       --nebula-falloff 0.3
# -----------------------------------------------------------------------------
set -euo pipefail

if [[ $# -lt 1 ]]; then
    echo "usage: $0 <seed> [options...]" >&2
    exit 2
fi

SEED="$1"; shift

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/.." && pwd)"
SKYBOXGEN="${SKYBOXGEN:-${ROOT}/../skyboxgen/build/skyboxgen}"

if [[ ! -x "${SKYBOXGEN}" ]]; then
    echo "error: skyboxgen not found at ${SKYBOXGEN}" >&2
    echo "       set SKYBOXGEN=/path/to/skyboxgen, or build it:" >&2
    echo "         (cd ../skyboxgen && cmake --build build)" >&2
    exit 1
fi

# ---- Parse our options (everything else we forward to skyboxgen). -----------
RES=1024
# new_privateer renders its own 3D sun in world space, so by default we tell
# skyboxgen NOT to paint one into the cubemap (two suns = bug). If a caller
# really wants a painted sun (e.g. a dead system with no local star, or
# aesthetic experimentation), they can pass --with-painted-sun.
EXTRA=(--no-sun)
while [[ $# -gt 0 ]]; do
    case "$1" in
        --rich|--intense)
            # 3 overlapping nebulae, gentle brightness bump, slightly denser
            # coverage. Tuned for "I want vibes, not a screensaver or an
            # eye chart." Keeps black voids between layers so stars poke
            # through.
            EXTRA+=(--nebula-count 2 --nebula-intensity 1.15
                    --nebula-falloff 0.95 --nebula-scale 1.1)
            shift
            ;;
        --wild)
            # For when you specifically want psychedelic Saturday-morning-
            # cartoon space. Expect severe alpha saturation.
            EXTRA+=(--nebula-count 5 --nebula-intensity 2.0
                    --nebula-falloff 0.5 --nebula-scale 1.3)
            shift
            ;;
        --sparse)
            # Mostly black with a few bright wisps. Classic Wing Commander.
            EXTRA+=(--nebula-count 2 --nebula-intensity 0.9
                    --nebula-falloff 1.6 --nebula-scale 0.9)
            shift
            ;;
        --with-painted-sun)
            # Remove the default --no-sun from EXTRA. Rarely useful now, but
            # here if you want it (e.g. debugging cubemap face orientation).
            EXTRA=("${EXTRA[@]/--no-sun/}")
            shift
            ;;
        --resolution)
            RES="$2"; shift 2
            ;;
        *)
            EXTRA+=("$1")
            shift
            ;;
    esac
done

OUT="${ROOT}/assets/skybox/${SEED}"
mkdir -p "${OUT}"

echo "[gen_skybox] seed='${SEED}'  res=${RES}  flags=${EXTRA[*]:-<none>}"
"${SKYBOXGEN}" \
    --seed "${SEED}" \
    --resolution "${RES}" \
    --out "${OUT}" \
    --prefix "${SEED}" \
    "${EXTRA[@]}"

echo "[gen_skybox] done. launch with:"
echo "  ./build/new_privateer --seed ${SEED}"

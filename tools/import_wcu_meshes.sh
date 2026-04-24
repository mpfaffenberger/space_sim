#!/usr/bin/env bash
# -----------------------------------------------------------------------------
# import_wcu_meshes.sh — bulk-convert Privateer WCU ship BFXM meshes to OBJ
# using Vega Strike's `vega-meshtool`.
#
# Output lives in assets/meshes/ships/ and is gitignored — the models are
# derived from the original Privateer game (Origin/EA IP), fine for a
# personal homage but not for redistribution via our public repo.
#
# Usage:
#   tools/import_wcu_meshes.sh                      # picks a curated ship set
#   tools/import_wcu_meshes.sh --all                # try every BFXM it finds
#   tools/import_wcu_meshes.sh --src /other/path    # override WCU source dir
#
# Requires ../privateer_wcu or --src <dir> to exist with bin/vega-meshtool
# + units/ inside.
# -----------------------------------------------------------------------------

set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
WCU="${HERE}/../privateer_wcu"
MODE=curated

while [[ $# -gt 0 ]]; do
    case "$1" in
        --all)    MODE=all;          shift ;;
        --src)    WCU="$2";          shift 2 ;;
        -h|--help)
            sed -n '3,14p' "$0"; exit 0 ;;
        *) echo "unknown flag: $1" >&2; exit 2 ;;
    esac
done

TOOL="${WCU}/bin/vega-meshtool"
if [[ ! -x "${TOOL}" ]]; then
    echo "error: vega-meshtool not found at ${TOOL}" >&2
    echo "  pass --src <dir> or symlink privateer_wcu next to new_privateer" >&2
    exit 1
fi

OUT_DIR="${HERE}/assets/meshes/ships"
mkdir -p "${OUT_DIR}"

# Curated set — the flagship Privateer + Wing Commander ships that are known
# to convert cleanly. Any new additions just need to be listed here as
# `<subdir>/<basename>`.
CURATED=(
    # Privateer civilian / merchant
    tarsus/tarsus
    centurion/centurion
    galaxy/galaxy
    orion/orion
    demon/demon
    paradigm/paradigm
    # Kilrathi fighters
    krant/krant
    dralthi/dralthi
    salthi/salthi
    gothri/gothri
    grikath/grikath
    sartha/sartha
    kamekh/kamekh
    # Confed fighters
    ferret/ferret
    hornet/hornet
    stiletto/stiletto
    sabre/sabre
    raptor/raptor
    scimitar/scimitar
    gladius/gladius
    # Misc
    talon/talon
    broadsword/broadsword
    steltek_fighter/steltek_fighter
    # Stations & bases
    mining_base/mining_base
    hospital_base/pirate_base
    refinery/refinery
)

# Build the work list.
if [[ "${MODE}" == "curated" ]]; then
    WORK=( "${CURATED[@]}" )
else
    WORK=()
    while IFS= read -r f; do
        rel="${f#${WCU}/units/}"       # strip prefix
        WORK+=( "${rel%.bfxm}" )
    done < <(find "${WCU}/units" -name "*.bfxm" 2>/dev/null | sort)
fi

ok=0; fail=0; skip=0
for entry in "${WORK[@]}"; do
    src="${WCU}/units/${entry}.bfxm"
    name="$(basename "${entry}")"
    dst="${OUT_DIR}/${name}.obj"

    if [[ ! -f "${src}" ]]; then
        echo "  skip ${name}  (not found)"
        skip=$((skip+1)); continue
    fi

    # vega-meshtool resolves referenced textures relative to CWD, so we cd
    # into the WCU root. The tool RELIABLY crashes (SIGTRAP) during cleanup
    # AFTER successfully writing the output — an ancient Vega Strike bug
    # that no one's going to fix. So we ignore the exit code and judge
    # success by whether the OBJ file materialised at a sane size.
    rm -f "${dst}"
    ( cd "${WCU}" && "${TOOL}" -i "units/${entry}.bfxm" -o "${dst}" \
        --convert BFXM Wavefront create >/dev/null 2>&1 ) || true

    # vega-meshtool's SIGTRAP during cleanup kills the final buffer
    # flush, so the file always ends mid-token (e.g. `vt 0.27208`
    # or `f 749/26`). That last partial line is lost data we can't
    # recover — but we CAN trim the file to its last complete
    # (newline-terminated) line so the OBJ loader gets clean input.
    # No-op if the file already ends in '\n'.
    if [[ -s "${dst}" ]]; then
        python3 - "${dst}" <<'PY'
import os, sys
p = sys.argv[1]
with open(p, 'rb') as f:
    data = f.read()
i = data.rfind(b'\n')
if i >= 0 and i + 1 != len(data):
    os.truncate(p, i + 1)
PY
    fi

    if [[ -s "${dst}" ]]; then
        verts=$(grep -c "^v " "${dst}" || true)
        faces=$(grep -c "^f " "${dst}" || true)

        ship_dir="${WCU}/units/$(dirname ${entry})"
        base="$(basename ${entry})"
        out_stem="${dst%.obj}"
        tex=""

        # -------------------------------------------------------------
        # Per-submesh material extraction.
        #
        # vega-meshtool will emit N XMesh files (0_0.xmesh, 1_0.xmesh, …)
        # — one per submesh — when given output name `0_0.xmesh`. We run
        # it into a scratch dir, parse each XMesh's `<Mesh>` opening tag
        # for texture attributes, and synthesize a sidecar JSON listing
        # the per-submesh texture files. The OBJ's `usemtl tex0_0…texN_0`
        # directives align 1-to-1 with these files by index.
        #
        # We also COPY every referenced texture file next to the OBJ
        # (sips-converted to PNG, same basename). The loader never has
        # to reach back into the WCU tree at runtime.
        # -------------------------------------------------------------
        tmp_xmesh_dir=$(mktemp -d)
        ( cd "${tmp_xmesh_dir}" && "${TOOL}" \
            -i "${src}" -o "0_0.xmesh" \
            --convert BFXM XMesh create >/dev/null 2>&1 ) || true

        python3 - "${tmp_xmesh_dir}" "${ship_dir}" "${out_stem}" <<'PY' || true
import glob, json, os, re, subprocess, sys
xmesh_dir, ship_dir, out_stem = sys.argv[1:4]

# Walk XMesh files in numeric order: 0_0.xmesh, 1_0.xmesh, …
def submesh_index(p):
    m = re.match(r"(\d+)_0\.xmesh$", os.path.basename(p))
    return int(m.group(1)) if m else 1_000_000
files = sorted(glob.glob(os.path.join(xmesh_dir, "*.xmesh")), key=submesh_index)

# Vega Strike slot convention:
#   texture   = diffuse
#   texture1  = specular
#   texture3  = glow/emissive        (texture2 = reserved/detail, unused by us)
#   texture4  = normal/bump
SLOTS = {"diffuse": "texture", "spec": "texture1",
         "glow":    "texture3", "normal": "texture4"}

def copy_tex(src_name):
    """Convert src_name (whatever format) → <out_stem dir>/<stem>.png.
    WCU lies about extensions (many .png files are DDS); sips handles
    that transparently. Returns the new basename or None on failure."""
    if not src_name: return None
    src = os.path.join(ship_dir, src_name)
    if not os.path.isfile(src): return None
    stem = os.path.splitext(os.path.basename(src_name))[0]
    out_dir = os.path.dirname(out_stem)
    dst = os.path.join(out_dir, stem + ".png")
    if not os.path.isfile(dst):   # dedup across submeshes sharing textures
        try:
            subprocess.run(["sips", "-s", "format", "png", src,
                            "--out", dst],
                           stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, check=True)
        except subprocess.CalledProcessError:
            return None
    return stem + ".png"

materials = []
for idx, f in enumerate(files):
    with open(f, "r", errors="replace") as fh:
        head = fh.read(2048)
    m = re.search(r"<Mesh\b([^>]*)>", head)
    if not m: continue
    attrs = dict(re.findall(r'(\w+)="([^"]*)"', m.group(1)))
    mat = {"name": f"tex{idx}_0"}
    for slot, xkey in SLOTS.items():
        copied = copy_tex(attrs.get(xkey))
        if copied: mat[slot] = copied
    # Record alpha blending hint — the glass domes of mining_base etc.
    # set blend="ONE INVSRCALPHA". Renderer can use this to pick a
    # transparent pipeline. Everything else is "ONE ZERO" = opaque.
    if attrs.get("blend", "ONE ZERO").upper() != "ONE ZERO":
        mat["alpha_blend"] = True
    materials.append(mat)

if materials:
    with open(out_stem + ".materials.json", "w") as fh:
        json.dump({"materials": materials}, fh, indent=2)
    print(f"__MAT_COUNT__ {len(materials)}")
PY
        # Materials JSON existence + entry count is the source of truth.
        if [[ -f "${out_stem}.materials.json" ]]; then
            mat_n=$(python3 -c "import json,sys; print(len(json.load(open(sys.argv[1]))['materials']))" "${out_stem}.materials.json")
            tex+="M${mat_n}"
        fi
        rm -rf "${tmp_xmesh_dir}"

        # -------------------------------------------------------------
        # Legacy single-texture fallback: still produce <name>.png /
        # <name>_spec.png / <name>_glow.png for meshes that either had
        # no per-submesh materials (weird exporter) OR get referenced
        # by code that hasn't been updated to consume materials.json.
        # This keeps older PlacedMesh code paths working during the
        # transition and is harmless when the material sidecar wins.
        # -------------------------------------------------------------
        for cand in "diff.png" "body.jpg"                    \
                    "structure.png"  "main_diff.png"         \
                    "structure_pirate.png" "tower_pirate.png" \
                    "${base}.png"  "${base}.jpg"             \
                    "${base^}.png" "${base^}.jpg"            \
                    "${base,}.png" "${base,}.jpg"; do
            if [[ -f "${ship_dir}/${cand}" ]]; then
                sips -s format png "${ship_dir}/${cand}" \
                    --out "${out_stem}.png" >/dev/null 2>&1 && tex+="D"
                break
            fi
        done
        for cand in "spec.png" "spec.jpg" "spec_0_0.texture" \
                    "structurespec.png" "main_spec.png"; do
            if [[ -f "${ship_dir}/${cand}" ]]; then
                sips -s format png "${ship_dir}/${cand}" \
                    --out "${out_stem}_spec.png" >/dev/null 2>&1 && tex+="S"
                break
            fi
        done
        for cand in "glow.png" "glow.jpg" "glow_0_0.texture" \
                    "structureglow.png" "main_glow.png"; do
            if [[ -f "${ship_dir}/${cand}" ]]; then
                sips -s format png "${ship_dir}/${cand}" \
                    --out "${out_stem}_glow.png" >/dev/null 2>&1 && tex+="G"
                break
            fi
        done

        [[ -n "${tex}" ]] && tex="+${tex}"
        printf "  ok   %-20s  v=%5d  f=%5d  %s\n" "${name}" "${verts}" "${faces}" "${tex}"
        ok=$((ok+1))
    else
        echo "  FAIL ${name}  (no output produced)"
        rm -f "${dst%.obj}.mtl"
        fail=$((fail+1))
    fi
done

echo ""
echo "[import_wcu_meshes] ${ok} converted, ${fail} failed, ${skip} skipped."
echo "output: ${OUT_DIR}"

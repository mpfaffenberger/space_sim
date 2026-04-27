// -----------------------------------------------------------------------------
// atlas_grid_viewer.cpp — F4 grid + swap + resize tool. See header for the
// design discussion; this file is the meat.
// -----------------------------------------------------------------------------

#include "atlas_grid_viewer.h"

#include "ship_sprite.h"
#include "sprite.h"

#include "imgui.h"
#include "sokol_imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace atlas_grid_viewer {

// ---- module-static state ---------------------------------------------------
//
// Visibility default = hidden. Same rationale as debug_panel / light editor:
// dev affordance, shouldn't clutter screenshots or hijack mouse input on
// startup. F4 reveals it on demand.
static bool        g_visible = false;
// Which atlas is being inspected. Empty until first build() call selects
// one (the first map entry, alphabetically — std::unordered_map's iteration
// order isn't stable, but for two ships the result is "Tarsus or Talon"
// either of which is a fine default; the user picks whatever they want
// from the combo on first interaction).
static std::string g_selected_atlas_key;
// Which frame inside the current atlas is selected. -1 = nothing. Index
// into ShipSpriteAtlas::frames (the manifest's natural order). Stays
// stable across SWAP because we mutate the frame's az/el/dir fields in
// place — the index points at the same memory either way.
static int         g_selected_frame_idx = -1;

// ---- helpers ---------------------------------------------------------------

// Strip "ships/<ship>/atlas_manifest" → "<ship>". Used so save_tuning can
// label the JSON's "ship" field correctly without us having to plumb the
// ship name through the runtime data.
static std::string ship_name_from_key(const std::string& key) {
    // key looks like "ships/tarsus/atlas_manifest". Find the segment
    // between the first and second '/'.
    const size_t a = key.find('/');
    if (a == std::string::npos) return key;
    const size_t b = key.find('/', a + 1);
    if (b == std::string::npos) return key.substr(a + 1);
    return key.substr(a + 1, b - a - 1);
}

// Convert SpriteArt::name (which is "assets/<stem>" with no .png) to the
// manifest's "sprite" field convention ("<stem>.png", no leading "assets/").
// We need this to match runtime frames against manifest samples in the
// generated swap-persistence script.
static std::string art_name_to_manifest_sprite(const std::string& name) {
    std::string s = name;
    constexpr const char* prefix = "assets/";
    if (s.rfind(prefix, 0) == 0) s.erase(0, std::strlen(prefix));
    s += ".png";
    return s;
}

// Recompute the unit-vector direction on the view sphere after an (az, el)
// mutation. Mirrors the formula in load_ship_sprite_atlas — we keep the
// two in sync by hand because the loader runs at startup only and we
// can't lean on it for runtime swap edits. If the formula ever drifts,
// see ShipSpriteFrame::dir for the canonical comment.
static void recompute_dir(ShipSpriteFrame& f) {
    constexpr float kPi = 3.14159265358979323846f;
    const float az_rad = f.az_deg * kPi / 180.0f;
    const float el_rad = f.el_deg * kPi / 180.0f;
    f.dir = HMM_V3(std::cos(el_rad) * std::sin(az_rad),
                   std::sin(el_rad),
                   std::cos(el_rad) * std::cos(az_rad));
}

// ---- persistence -----------------------------------------------------------

// Write atlas_manifest.tuning.json — same shape that ship_sprite.cpp's
// loader recognises (ship, frames[{az, el, scale, roll_deg}]). We always
// emit ALL frames, not just the modified ones, so a single tuning file
// is a self-contained snapshot. Cheap (82 lines) and avoids partial-state
// confusion if the file already has stale entries.
//
// Note: the loader matches tuning entries to runtime frames by (az, el),
// so the values we write here must reflect each frame's CURRENT post-swap
// labels. They already do — we just mirror frame.az_deg / .el_deg.
static void save_tuning(const ShipSpriteAtlas& atlas) {
    const std::filesystem::path path =
        std::filesystem::path("assets") / (atlas.key + ".tuning.json");
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path);
    out << "{\n  \"ship\": \"" << ship_name_from_key(atlas.key) << "\",\n"
        << "  \"frames\": [\n";
    for (size_t i = 0; i < atlas.frames.size(); ++i) {
        const ShipSpriteFrame& f = atlas.frames[i];
        out << "    { \"az\": "       << f.az_deg
            << ", \"el\": "           << f.el_deg
            << ", \"scale\": "        << f.scale
            << ", \"roll_deg\": "     << f.roll_deg << " }"
            << (i + 1 == atlas.frames.size() ? "\n" : ",\n");
    }
    out << "  ]\n}\n";
    std::printf("[atlas_grid_viewer] wrote %s (%zu frames)\n",
                path.string().c_str(), atlas.frames.size());
}

// Build a self-contained Python snippet that rewrites
// assets/<atlas.key>.json so each sample's (az, el) match the current
// runtime values. Mike copies the snippet → pastes into a shell → runs
// it. Idempotent: pasting twice produces identical output.
//
// Why a Python helper rather than a C++ writer? Our json::Value uses an
// unordered_map internally, so emitting it loses field order. Python's
// json module preserves dict insertion order, which means re-saved
// manifests keep their human-friendly key ordering and produce minimal
// diffs. The Python is small enough to embed verbatim — no separate
// file or PATH lookup needed.
//
// Match strategy: we key the override dict by the manifest's "sprite"
// field (relative path, with .png). That's stable under sample reordering
// in the manifest, so a future re-sort of samples doesn't break the swap
// history. Falls back gracefully — samples whose sprite isn't in the dict
// are left untouched.
static void copy_swap_script(const ShipSpriteAtlas& atlas) {
    std::string py;
    py.reserve(4096);

    py += "python3 - <<'PY'\n";
    py += "# Generated by F4 atlas grid viewer. Pastes the current\n";
    py += "# in-engine (az, el) labels back into the on-disk manifest.\n";
    py += "import json, pathlib\n";
    py += "p = pathlib.Path('assets/" + atlas.key + ".json')\n";
    py += "m = json.loads(p.read_text())\n";
    py += "overrides = {\n";
    for (const ShipSpriteFrame& f : atlas.frames) {
        if (!f.art) continue;
        char line[256];
        std::snprintf(line, sizeof(line),
            "    %-90s: (%g, %g),\n",
            ("'" + art_name_to_manifest_sprite(f.art->name) + "'").c_str(),
            f.az_deg, f.el_deg);
        py += line;
    }
    py += "}\n";
    py += "for s in m['samples']:\n";
    py += "    key = s.get('sprite')\n";
    py += "    if key in overrides:\n";
    py += "        s['az'], s['el'] = overrides[key]\n";
    py += "p.write_text(json.dumps(m, indent=2) + '\\n')\n";
    py += "print(f'[swap-persist] updated {p} ({len(overrides)} sample labels)')\n";
    py += "PY\n";

    ImGui::SetClipboardText(py.c_str());
    std::printf("[atlas_grid_viewer] copied %zu-byte swap-persistence script "
                "to clipboard\n", py.size());
}

// ---- lifecycle -------------------------------------------------------------

void init() {
    std::printf("[atlas_grid_viewer] ready — F4 to toggle\n");
}

void shutdown() {}

bool handle_event(const sapp_event* e) {
    if (e->type == SAPP_EVENTTYPE_KEY_DOWN &&
        e->key_code == SAPP_KEYCODE_F4) {
        g_visible = !g_visible;
        return true;     // consumed — don't let it leak to the game
    }
    return false;
}

// ---- build -----------------------------------------------------------------

void build(std::unordered_map<std::string, ShipSpriteAtlas>& atlases) {
    if (!g_visible) return;

    // Pre-size the window to fit a 16-column grid + inspector pane comfortably.
    // Once the user resizes manually ImGui remembers the choice via its .ini.
    ImGui::SetNextWindowSize(ImVec2(1280.0f, 720.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Ship Atlas Grid Viewer (F4)", &g_visible)) {
        ImGui::End();
        return;
    }

    if (atlases.empty()) {
        ImGui::TextDisabled("No ship atlases loaded — nothing to inspect.");
        ImGui::End();
        return;
    }

    // First-run + stale-key recovery: pick a sane default if our memory
    // of last frame's selection no longer matches a loaded atlas.
    if (g_selected_atlas_key.empty() ||
        atlases.find(g_selected_atlas_key) == atlases.end()) {
        g_selected_atlas_key = atlases.begin()->first;
        g_selected_frame_idx = -1;
    }

    // Ship picker — a flat combo over loaded atlases. Two ships in the
    // common case so a tab bar would be overkill, but a combo also scales
    // gracefully if more atlases get loaded later.
    if (ImGui::BeginCombo("ship", g_selected_atlas_key.c_str())) {
        for (auto& [key, _atlas] : atlases) {
            const bool is_sel = (key == g_selected_atlas_key);
            if (ImGui::Selectable(key.c_str(), is_sel)) {
                g_selected_atlas_key = key;
                g_selected_frame_idx = -1;     // reset selection on ship change
            }
            if (is_sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ShipSpriteAtlas& atlas = atlases.at(g_selected_atlas_key);

    // Index frames by elevation row. std::map keeps elevations sorted —
    // we then iterate in REVERSE so the highest-el row (camera looking
    // straight down at the ship) renders at the top of the grid, matching
    // the spatial intuition of "above is up". Within each row we sort by
    // az ascending so (az=0) is leftmost.
    std::map<int, std::vector<int>> rows;
    for (int i = 0; i < (int)atlas.frames.size(); ++i) {
        rows[(int)std::round(atlas.frames[i].el_deg)].push_back(i);
    }
    for (auto& [_el, indices] : rows) {
        std::sort(indices.begin(), indices.end(), [&](int a, int b) {
            return atlas.frames[a].az_deg < atlas.frames[b].az_deg;
        });
    }

    // Layout: grid on the left, inspector on the right. Sized so the
    // grid's 16 cells + el labels fit comfortably at 64-px thumbnails;
    // the inspector takes the remainder of the window width.
    constexpr float kCellSize    = 64.0f;
    constexpr float kGridWidth   = kCellSize * 16.0f + 90.0f;  // + el-label gutter
    const ImVec4    kSelColor    (1.00f, 0.60f, 0.00f, 1.0f);  // hi-vis amber
    const ImVec4    kIdleColor   (0.20f, 0.20f, 0.20f, 1.0f);

    ImGui::BeginChild("##grid", ImVec2(kGridWidth, 0), true);
    ImGui::TextUnformatted("Click a cell to select. Click another to swap (az, el).");
    ImGui::TextDisabled("Cells: %zu  |  selection: %s",
                        atlas.frames.size(),
                        g_selected_frame_idx < 0
                            ? "(none)"
                            : "click another cell to swap, or click selection again to clear");
    ImGui::Separator();

    // High-el rows first → low-el rows last. Visually matches "looking
    // down at the ship from above" → top of grid; "from below" → bottom.
    for (auto rit = rows.rbegin(); rit != rows.rend(); ++rit) {
        const int el = rit->first;
        const std::vector<int>& indices = rit->second;

        // Row label gutter — fixed width so cells line up across rows
        // even when pole rows have only one entry.
        ImGui::AlignTextToFramePadding();
        ImGui::Text("el %+4d", el);
        ImGui::SameLine(80.0f);

        for (size_t i = 0; i < indices.size(); ++i) {
            const int idx = indices[i];
            ShipSpriteFrame& f = atlas.frames[idx];
            const bool is_selected = (g_selected_frame_idx == idx);

            ImGui::PushID(idx);
            // Highlight selected cells with a thick amber border. We use
            // the frame border so it's drawn around the ImageButton's
            // outline rather than overlaying the texture.
            ImGui::PushStyleColor(ImGuiCol_Border,
                                  is_selected ? kSelColor : kIdleColor);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize,
                                is_selected ? 3.0f : 1.0f);

            if (f.art) {
                const ImVec2 size(kCellSize, kCellSize);
                if (ImGui::ImageButton("##cell",
                                       simgui_imtextureid(f.art->hull.view),
                                       size)) {
                    if (g_selected_frame_idx < 0) {
                        // First pick — just select.
                        g_selected_frame_idx = idx;
                    } else if (g_selected_frame_idx == idx) {
                        // Re-click on selected → deselect.
                        g_selected_frame_idx = -1;
                    } else {
                        // SWAP. We move (az, el) labels between the two
                        // frames; the SpriteArt* (PNG content) stays put.
                        // Net effect: the cell-selector now picks the
                        // first cell's PNG when the runtime view direction
                        // matches the second cell's old (az, el), and
                        // vice versa.
                        ShipSpriteFrame& a = atlas.frames[g_selected_frame_idx];
                        ShipSpriteFrame& b = atlas.frames[idx];
                        std::swap(a.az_deg,  b.az_deg);
                        std::swap(a.el_deg,  b.el_deg);
                        // dir is a precomputed cache of (az, el) — recompute
                        // from scratch rather than swapping, in case future
                        // refactors widen ShipSpriteFrame and forget this.
                        recompute_dir(a);
                        recompute_dir(b);
                        g_selected_frame_idx = -1;
                    }
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(
                        "az %.0f  el %.0f\nscale %.2f  roll %.1f°\nfile: %s",
                        f.az_deg, f.el_deg, f.scale, f.roll_deg,
                        f.art->name.c_str());
                }
            } else {
                // Defensive — shouldn't happen post-load, but keep the
                // grid layout intact if some frame failed to load its art.
                ImGui::Dummy(ImVec2(kCellSize, kCellSize));
            }

            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
            ImGui::PopID();

            if (i + 1 < indices.size()) ImGui::SameLine();
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("##inspector", ImVec2(0, 0), true);
    if (g_selected_frame_idx < 0 ||
        g_selected_frame_idx >= (int)atlas.frames.size()) {
        ImGui::TextDisabled("(click a cell to select it for inspection)");
    } else {
        ShipSpriteFrame& f = atlas.frames[g_selected_frame_idx];
        ImGui::Text("Selected: az %.0f, el %.0f", f.az_deg, f.el_deg);
        ImGui::TextDisabled("%s",
                            f.art ? f.art->name.c_str() : "(no art)");

        if (f.art) {
            // Aspect-correct preview. 256 along the longest axis matches
            // the size used in the existing F2 / debug panel previews —
            // big enough to read fine details but doesn't dominate the
            // window so we still see the grid alongside it.
            const float longest = (float)std::max(f.art->hull_w, f.art->hull_h);
            const float pw = 256.0f * (float)f.art->hull_w / longest;
            const float ph = 256.0f * (float)f.art->hull_h / longest;
            ImGui::Image(simgui_imtextureid(f.art->hull.view), ImVec2(pw, ph));
        }

        // Live-mutate scale + roll. Bounds chosen pragmatically: scale
        // ≥ 0.3× catches things like over-cropped pole cells; ≤ 2.5×
        // is the upper end before the cell starts clipping outside its
        // billboard. Roll is the full ±180° because elevation cells can
        // need any rotation to align with cap_up.
        ImGui::SliderFloat("scale",       &f.scale,    0.3f,   2.5f, "%.3f");
        ImGui::SliderFloat("roll (deg)",  &f.roll_deg, -180.0f, 180.0f, "%.1f");

        if (ImGui::Button("clear selection")) g_selected_frame_idx = -1;
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Persistence");
    if (ImGui::Button("Save tuning (.tuning.json)")) {
        save_tuning(atlas);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Writes assets/%s.tuning.json with the current\n"
            "scale + roll_deg for every cell. The loader picks\n"
            "those up next time the engine starts.",
            atlas.key.c_str());
    }

    if (ImGui::Button("Copy swap-persistence script")) {
        copy_swap_script(atlas);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Copies a one-liner Python heredoc to the clipboard.\n"
            "Paste it into a shell at the repo root to rewrite\n"
            "assets/%s.json with the current in-engine\n"
            "(az, el) labels for every sample. Idempotent.",
            atlas.key.c_str());
    }
    ImGui::EndChild();

    ImGui::End();
}

} // namespace atlas_grid_viewer

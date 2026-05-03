// -----------------------------------------------------------------------------
// sprite_light_editor.cpp — ImGui light-placement tool.
//
// Architecture:
//   * File-scope state: visible flag, selected sprite/light indices,
//     whether the user is mid-drag.
//   * build() does everything: sprite picker, click-capture image widget,
//     per-light side panel, save/reload buttons.
//   * JSON load/save is hand-rolled serialization — the format is small
//     enough (array of flat objects) that bringing in a writer library
//     would be overkill. Read uses the existing json.h parser.
// -----------------------------------------------------------------------------

#include "sprite_light_editor.h"
#include "json.h"

#include "imgui.h"
#include "sokol_imgui.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>

namespace sprite_light_editor {

// ---------------------------------------------------------------------------
// Persistent editor state.
// ---------------------------------------------------------------------------

// Visibility: hidden by default. F2 toggles. Starting visible would clutter
// every screenshot — same rationale as debug_panel's g_visible.
static bool g_visible = false;

// Which entry is being edited. The editor presents a flat list:
// indices [0, sprites.size())               → placed scene SpriteObjects
// indices [sprites.size(), total)            → entries from extra_arts
// `-1` before any item exists. Clamped otherwise.
static int  g_sel_sprite = 0;

// Index into the selected target's lights vector, or -1 if no selection.
// Cleared when switching targets or deleting the currently-selected light.
static int  g_sel_light  = -1;

// ---- Auto-save state -------------------------------------------------
// Per-target snapshot of last-saved lights, keyed by the target's STRING
// NAME (not its address!). The earlier pointer-keyed implementation broke
// catastrophically because main.cpp rebuilds the EditableArt vector every
// frame, so `&extra.name` is a different address each frame even though
// it holds the same string content. The pointer compare always succeeded
// at "target switched" → snapshot reset every frame → user edits never
// triggered a save (except when the heap allocator happened to reuse
// addresses, which was random and lost authoring across hundreds of cells).
//
// String-keyed map fixes that AND naturally tracks all targets at once:
// every frame we walk the full editable list, compare in-memory to
// snapshot, save any that differ. Touching cells you're not currently
// viewing is a non-issue because their `cur_lights` doesn't change unless
// something modified them.
static std::unordered_map<std::string, std::vector<LightSpot>> g_save_snapshots;

static bool lights_equal(const std::vector<LightSpot>& a,
                         const std::vector<LightSpot>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const LightSpot& x = a[i];
        const LightSpot& y = b[i];
        // Field-by-field. NOT memcmp — LightSpot has trailing padding
        // bytes after `kind` that vary based on previous stack contents,
        // and those would defeat change-detection while looking byte-
        // different on consecutive frames.
        if (x.u != y.u || x.v != y.v
         || x.color.X != y.color.X || x.color.Y != y.color.Y
         || x.color.Z != y.color.Z
         || x.size != y.size || x.hz != y.hz
         || x.phase != y.phase || x.kind != y.kind) return false;
    }
    return true;
}

// Pixel-to-world zoom of the sprite preview. 1.0 = the PNG renders at its
// native pixel size; higher zooms help hit small hull features (antenna
// tips, radar dish). Clamped to [1, 4].
static float g_zoom = 1.0f;

// ---- Sticky last-used spot properties ------------------------------------
//
// New light spots inherit colour/kind/hz/phase from the last placed-or-edited
// spot, so authoring runs of identical lights (e.g. five green strobes on
// the same radar dish, or twelve red nav-edges along a wing) doesn't require
// re-picking the same colour + kind + hz + phase from scratch every click.
// Size is intentionally NOT sticky — the user's existing workflow is to
// place at the default 5 px and rescale globally with the python crush
// tool, so changing size-default is a footgun (would silently bloat new
// placements).
//
// Initialised to the historical defaults (red / steady / 0 / 0). Updated:
//   - any time the user edits the selected spot's colour, kind, hz, or phase
//   - any time a new spot is placed (so back-to-back placements stay
//     identical without ever touching the right-column editor)
//
// Shift-click for blue is preserved for muscle-memory: it overrides the
// sticky colour for that one placement (and updates the sticky to blue,
// so the next plain click also gets blue — "shift = switch to the OTHER
// colour, then keep going" matches what users naturally expect).
static HMM_Vec3  g_sticky_color = HMM_V3(1.00f, 0.10f, 0.10f);   // hot red
static float     g_sticky_hz    = 0.0f;
static float     g_sticky_phase = 0.0f;
static LightKind g_sticky_kind  = LightKind::Steady;

// ---------------------------------------------------------------------------
// Event handling
// ---------------------------------------------------------------------------

void init() {
    // Nothing to set up — ImGui is owned by debug_panel which is init'd
    // before us in main.cpp. This function exists for symmetry with other
    // subsystems and to give future initialisation a home.
    std::printf("[light_editor] ready — F2 to toggle\n");
}

bool handle_event(const sapp_event* e) {
    if (e->type == SAPP_EVENTTYPE_KEY_DOWN &&
        e->key_code == SAPP_KEYCODE_F2) {
        g_visible = !g_visible;
        return true;   // consumed
    }
    return false;      // let debug_panel / game input see it
}

// ---------------------------------------------------------------------------
// JSON load / save (flat array of lights)
// ---------------------------------------------------------------------------
//
// Format (example):
//   [
//     { "u": 0.12, "v": 0.55, "color": [255, 50, 40], "size": 120, "hz": 1.0,
//       "phase": 0.0, "kind": "strobe" },
//     ...
//   ]
// Colors are stored as 0-255 bytes for human readability; we convert to
// 0-1 floats at load/save boundaries. Kind is a string ("steady"/"pulse"/
// "strobe") for the same reason — authoring-friendly.

static std::string kind_to_string(LightKind k) {
    switch (k) {
    case LightKind::Steady: return "steady";
    case LightKind::Pulse:  return "pulse";
    case LightKind::Strobe: return "strobe";
    }
    return "steady";
}

static LightKind string_to_kind(const std::string& s) {
    if (s == "pulse")  return LightKind::Pulse;
    if (s == "strobe") return LightKind::Strobe;
    return LightKind::Steady;
}

bool load_lights_sidecar(const std::string& sprite_base_path,
                         std::vector<LightSpot>& out) {
    const std::string path = sprite_base_path + ".lights.json";
    // Most sprites don't have a sidecar — pre-flight the existence check
    // ourselves so json::parse_file's "cannot open" warning is reserved
    // for actual misconfigurations (file present but unparseable).
    if (!std::filesystem::exists(path)) return false;
    json::Value v = json::parse_file(path);
    if (!v.is_array()) return false;

    out.clear();
    for (const auto& entry : v.as_array()) {
        if (!entry.is_object()) continue;
        LightSpot ls{};
        if (auto* p = entry.find("u"))     ls.u     = p->as_float();
        if (auto* p = entry.find("v"))     ls.v     = p->as_float();
        if (auto* p = entry.find("size"))  ls.size  = p->as_float();
        if (auto* p = entry.find("hz"))    ls.hz    = p->as_float();
        if (auto* p = entry.find("phase")) ls.phase = p->as_float();
        if (auto* p = entry.find("kind") ; p && p->is_string())
            ls.kind = string_to_kind(p->as_string());
        if (auto* p = entry.find("color"); p && p->is_array() &&
                                            p->as_array().size() >= 3) {
            const auto& a = p->as_array();
            ls.color.X = (float)a[0].as_number() / 255.0f;
            ls.color.Y = (float)a[1].as_number() / 255.0f;
            ls.color.Z = (float)a[2].as_number() / 255.0f;
        }
        out.push_back(ls);
    }
    std::printf("[light_editor] loaded %zu lights from %s\n",
                out.size(), path.c_str());
    return true;
}

bool save_lights_sidecar(const std::string& sprite_base_path,
                         const std::vector<LightSpot>& lights) {
    const std::string path = sprite_base_path + ".lights.json";
    std::ofstream f(path);
    if (!f) {
        std::fprintf(stderr, "[light_editor] cannot open %s for write\n",
                     path.c_str());
        return false;
    }
    // Hand-serialise. Two-space indent, one light per line for git diffs.
    f << "[\n";
    for (size_t i = 0; i < lights.size(); ++i) {
        const LightSpot& ls = lights[i];
        const int r = (int)std::round(ls.color.X * 255.0f);
        const int g = (int)std::round(ls.color.Y * 255.0f);
        const int b = (int)std::round(ls.color.Z * 255.0f);
        f << "  { "
          << "\"u\": "     << ls.u     << ", "
          << "\"v\": "     << ls.v     << ", "
          << "\"color\": [" << r << ", " << g << ", " << b << "], "
          << "\"size\": "  << ls.size  << ", "
          << "\"hz\": "    << ls.hz    << ", "
          << "\"phase\": " << ls.phase << ", "
          << "\"kind\": \"" << kind_to_string(ls.kind) << "\" }";
        if (i + 1 < lights.size()) f << ",";
        f << "\n";
    }
    f << "]\n";
    std::printf("[light_editor] saved %zu lights to %s\n",
                lights.size(), path.c_str());
    return true;
}

// ---------------------------------------------------------------------------
// UI helpers
// ---------------------------------------------------------------------------

// Colour picker swatch for a LightSpot. Writes back into `color` in-place.
// Uses ImGui::ColorEdit3 with the NoInputs flag to keep the widget compact.
static void color_picker(HMM_Vec3& color, const char* id) {
    float rgb[3] = { color.X, color.Y, color.Z };
    if (ImGui::ColorEdit3(id, rgb,
                          ImGuiColorEditFlags_NoInputs |
                          ImGuiColorEditFlags_NoLabel)) {
        color.X = rgb[0];
        color.Y = rgb[1];
        color.Z = rgb[2];
    }
}

static const char* kKindLabels[] = { "steady", "pulse", "strobe" };

// ---------------------------------------------------------------------------
// Per-frame UI
// ---------------------------------------------------------------------------

void build(std::vector<SpriteObject>& sprites,
           const std::vector<EditableArt>& extra_arts) {
    if (!g_visible) return;

    // Unified target resolution. Each selectable item maps to (art, lights
    // vector, sidecar stem). Placed sprites edit the instance's lights;
    // extra arts edit the asset's `light_spots` directly so all in-world
    // billboards using that art see the change live (essential for ship-
    // sprite atlases where the SpriteObject is rebuilt each frame).
    const int total = (int)sprites.size() + (int)extra_arts.size();
    if (total == 0) return;
    g_sel_sprite = std::max(0, std::min(g_sel_sprite, total - 1));

    auto resolve_target = [&](int idx,
                              const SpriteArt*& out_art,
                              std::vector<LightSpot>*& out_lights,
                              const std::string*& out_name) {
        if (idx < (int)sprites.size()) {
            SpriteObject& s = sprites[idx];
            out_art    = s.art;
            out_lights = &s.lights;
            out_name   = s.art ? &s.art->name : nullptr;
        } else {
            const EditableArt& e = extra_arts[idx - (int)sprites.size()];
            out_art    = e.art;
            out_lights = e.art ? &e.art->light_spots : nullptr;
            out_name   = &e.name;
        }
    };

    const SpriteArt*        sel_art    = nullptr;
    std::vector<LightSpot>* sel_lights = nullptr;
    const std::string*      sel_name   = nullptr;
    resolve_target(g_sel_sprite, sel_art, sel_lights, sel_name);
    if (!sel_art || !sel_lights || !sel_name) return;

    ImGui::SetNextWindowSize(ImVec2(1200, 760), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Sprite Light Editor (F2)", &g_visible)) {
        ImGui::End();
        return;
    }

    // ---- Top toolbar row ------------------------------------------------
    // Layout: [<] [>] [\u2192\u2205] [stem dropdown] [auto-save \u25cf] [reload] [zoom]
    // Prev/next step linearly. The "\u2192\u2205" jump button skips ahead to
    // the next item with NO authored lights — invaluable when you've cycled
    // through 80 cells and need to find which 11 you missed. The dropdown
    // marks each item with \u2713 (authored, has lights) or \u00b7 (empty)
    // so you can scan the whole atlas for gaps without leaving the popup.
    auto step_selection = [&](int delta) {
        g_sel_sprite = ((g_sel_sprite + delta) % total + total) % total;
        g_sel_light  = -1;
    };
    auto target_lights_count = [&](int idx) -> int {
        const SpriteArt*        art    = nullptr;
        std::vector<LightSpot>* lights = nullptr;
        const std::string*      name   = nullptr;
        resolve_target(idx, art, lights, name);
        return (lights ? (int)lights->size() : -1);
    };
    if (ImGui::ArrowButton("##prev", ImGuiDir_Left)) step_selection(-1);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Previous sprite");
    ImGui::SameLine();
    if (ImGui::ArrowButton("##next", ImGuiDir_Right)) step_selection(+1);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Next sprite");
    ImGui::SameLine();
    if (ImGui::Button("->0")) {
        // Find next item (wrapping) with zero lights. Stops at current if
        // nothing else is empty. Cap at `total` iterations so an all-full
        // atlas doesn't infinite-loop.
        for (int step = 1; step <= total; ++step) {
            const int idx = ((g_sel_sprite + step) % total + total) % total;
            if (target_lights_count(idx) == 0) {
                g_sel_sprite = idx;
                g_sel_light  = -1;
                break;
            }
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Jump to next sprite with no lights");
    ImGui::SameLine();

    // Tally for the toolbar status line — "57/80 authored" makes "how
    // many cells am I really done with" answerable at a glance, instead
    // of relying on `ls | wc -l` from a terminal.
    int authored_count = 0;
    for (int i = 0; i < total; ++i) {
        if (target_lights_count(i) > 0) ++authored_count;
    }

    ImGui::SetNextItemWidth(-360.0f);   // leave room for buttons + zoom + tally
    if (ImGui::BeginCombo("##sprite_combo", sel_name->c_str())) {
        for (int i = 0; i < total; ++i) {
            const SpriteArt*        art    = nullptr;
            std::vector<LightSpot>* lights = nullptr;
            const std::string*      name   = nullptr;
            resolve_target(i, art, lights, name);
            if (!art || !name) continue;
            const bool is_sel = (i == g_sel_sprite);
            const bool authored = (lights && !lights->empty());
            // Prefix marker: "v" for authored, "-" for empty. Plain ASCII
            // because the imgui default font may not have ✓/✗ glyphs.
            char label[256];
            std::snprintf(label, sizeof(label), "%s  %s",
                          authored ? "[v]" : "[ ]", name->c_str());
            if (ImGui::Selectable(label, is_sel)) {
                g_sel_sprite = i;
                g_sel_light  = -1;
            }
            if (is_sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::TextColored(
        authored_count == total ? ImVec4(0.4f, 1.0f, 0.4f, 1.0f)
                                : ImVec4(1.0f, 0.85f, 0.30f, 1.0f),
        "%d/%d", authored_count, total);
    // No "save" button — every change auto-writes the sidecar at end of
    // frame (see auto-save block at the bottom of build()). Status text
    // confirms the target file path so the user knows where edits land.
    ImGui::SameLine();
    ImGui::TextDisabled("auto-save \xee\x9c\xa0");   // visual cue (degree mark — font-safe)
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Edits write to %s.lights.json automatically",
                          sel_name->c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button("reload")) {
        std::vector<LightSpot> fresh;
        if (load_lights_sidecar(*sel_name, fresh)) {
            *sel_lights = std::move(fresh);
            g_sel_light = -1;
            // Sync snapshot so auto-save doesn't immediately re-write
            // the file we just loaded from.
            g_save_snapshots[*sel_name] = *sel_lights;
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Reload from disk, dropping in-memory edits");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::SliderFloat("zoom", &g_zoom, 1.0f, 6.0f, "%.1fx");

    ImGui::Separator();

    // ---- Two-column layout: image on left, light list/editor on right ---
    // The image pane auto-fits the available content region (so a 1022-px
    // ship cell doesn't push the right panel off-screen). Zoom slider
    // operates ON TOP of the fit-size and triggers scrollbars when >1x.
    std::vector<LightSpot>& cur_lights = *sel_lights;

    constexpr float kRightPanelMin = 340.0f;
    constexpr float kImageColPadding = 16.0f;
    const ImVec2  region        = ImGui::GetContentRegionAvail();
    const float   image_col_w   = std::max(160.0f,
                                  region.x - kRightPanelMin - kImageColPadding);
    const float   fit_size      = std::max(64.0f,
                                  std::min(image_col_w, region.y - 8.0f));
    const float   img_size      = fit_size * g_zoom;

    ImGui::BeginChild("image_col",
                      ImVec2(image_col_w, 0),
                      ImGuiChildFlags_Border,
                      ImGuiWindowFlags_HorizontalScrollbar);
    {
        const ImVec2 img_pos = ImGui::GetCursorScreenPos();
        ImGui::Image(simgui_imtextureid(sel_art->hull.view),
                     ImVec2(img_size, img_size));

        // Click handling. Must be checked AFTER Image so the InvisibleButton
        // trick is unnecessary — Image acts as an item and IsItemClicked()
        // works directly on it.
        const bool image_hovered = ImGui::IsItemHovered();
        const bool image_clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);

        // Image-local mouse position in UV (0..1), valid only when hovered.
        ImVec2 mouse_uv{0, 0};
        if (image_hovered) {
            const ImVec2 m = ImGui::GetMousePos();
            mouse_uv.x = (m.x - img_pos.x) / img_size;
            mouse_uv.y = (m.y - img_pos.y) / img_size;
        }

        // Draw gizmo circles for every existing light. Draw order: the
        // Image is already drawn; overlay with ImDrawList so circles sit
        // on top of the texture.
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // Gizmo hit test: pixel radius on screen. Larger than the visual
        // radius so small lights are still clickable.
        const float hit_radius = 10.0f;
        int hit_light = -1;
        float hit_dist2 = hit_radius * hit_radius;

        for (int i = 0; i < (int)cur_lights.size(); ++i) {
            const LightSpot& ls = cur_lights[i];
            const ImVec2 c{ img_pos.x + ls.u * img_size,
                            img_pos.y + ls.v * img_size };
            const ImU32 col = IM_COL32(
                (int)(ls.color.X * 255),
                (int)(ls.color.Y * 255),
                (int)(ls.color.Z * 255), 220);
            const ImU32 outline = (i == g_sel_light) ?
                IM_COL32(255, 255, 255, 255) : IM_COL32(0, 0, 0, 255);

            dl->AddCircleFilled(c, 5.0f, col);
            dl->AddCircle(c, 6.0f, outline, 0, 2.0f);

            if (image_hovered) {
                const float dx = ImGui::GetMousePos().x - c.x;
                const float dy = ImGui::GetMousePos().y - c.y;
                const float d2 = dx * dx + dy * dy;
                if (d2 < hit_dist2) { hit_dist2 = d2; hit_light = i; }
            }
        }

        // Click dispatch:
        //   - hit on gizmo → select it
        //   - hit on empty area → spawn new light at that UV, select it
        if (image_clicked) {
            if (hit_light >= 0) {
                g_sel_light = hit_light;
            } else if (mouse_uv.x >= 0 && mouse_uv.x <= 1 &&
                       mouse_uv.y >= 0 && mouse_uv.y <= 1) {
                // Shift-click → switch to blue (port nav / cool accents) AND
                // make blue the new sticky colour for subsequent clicks; plain
                // click → use whatever sticky colour is currently set
                // (defaults to hot red on first launch). kind/hz/phase ALWAYS
                // come from the sticky values so authoring runs of identical
                // lights doesn't require re-picking everything per spot.
                const bool shift = ImGui::GetIO().KeyShift;
                if (shift) g_sticky_color = HMM_V3(0.20f, 0.55f, 1.00f);
                LightSpot ls{};
                ls.u = mouse_uv.x;
                ls.v = mouse_uv.y;
                ls.color = g_sticky_color;
                ls.size  = 5.0f;     // see g_sticky_color comment re: size
                ls.hz    = g_sticky_hz;
                ls.phase = g_sticky_phase;
                ls.kind  = g_sticky_kind;
                cur_lights.push_back(ls);
                g_sel_light = (int)cur_lights.size() - 1;
            }
        }

        // Drag-to-move the selected light. Only active while LMB is held
        // AND the drag started on the selected gizmo (to avoid grabbing a
        // light every time the user clicks nearby).
        if (g_sel_light >= 0 && image_hovered &&
            ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f)) {
            LightSpot& ls = cur_lights[g_sel_light];
            ls.u = std::min(1.0f, std::max(0.0f, mouse_uv.x));
            ls.v = std::min(1.0f, std::max(0.0f, mouse_uv.y));
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // ---- Right column: light list + selected-light editor ---------------
    ImGui::BeginChild("edit_col", ImVec2(0, 0), ImGuiChildFlags_Border);
    {
        ImGui::Text("%zu light%s", cur_lights.size(),
                    cur_lights.size() == 1 ? "" : "s");
        ImGui::SameLine();
        if (ImGui::Button("+ add")) {
            // Same sticky values as click-spawn (kept in sync so users get a
            // consistent first-time experience whichever entry path they
            // use). Shift-modifier intentionally not honoured here — the
            // button is for "give me a light I'll position later", not
            // "give me one in this colour".
            LightSpot ls{};
            ls.u = 0.5f; ls.v = 0.5f;
            ls.color = g_sticky_color;
            ls.size  = 5.0f;
            ls.hz    = g_sticky_hz;
            ls.phase = g_sticky_phase;
            ls.kind  = g_sticky_kind;
            cur_lights.push_back(ls);
            g_sel_light = (int)cur_lights.size() - 1;
        }

        // ---- Extrapolation helpers ----------------------------------
        // Authoring 80 ship-atlas cells from scratch is brutal; the
        // common workflow is to author one cell well, then copy/mirror
        // its lights onto the neighbouring az/el cells and tweak. These
        // buttons grab the prev/next item's lights wholesale (replacing
        // the current list) — "mirror X" flips u→1-u for the opposite
        // side of the ship (e.g. authoring az=90 then mirror-copying to
        // az=270 with the port/starboard swap baked in).
        ImGui::Separator();
        ImGui::TextUnformatted("copy lights from neighbour:");
        auto copy_from = [&](int delta, bool mirror_x) {
            if (total <= 1) return;
            const int src_idx = ((g_sel_sprite + delta) % total + total) % total;
            const SpriteArt*        src_art    = nullptr;
            std::vector<LightSpot>* src_lights = nullptr;
            const std::string*      src_name   = nullptr;
            resolve_target(src_idx, src_art, src_lights, src_name);
            if (!src_lights) return;
            cur_lights = *src_lights;     // value-copy, preserves color/hz/etc
            if (mirror_x) {
                for (LightSpot& ls : cur_lights) ls.u = 1.0f - ls.u;
            }
            g_sel_light = -1;
        };
        if (ImGui::Button("< prev"))           copy_from(-1, false);
        ImGui::SameLine();
        if (ImGui::Button("next >"))           copy_from(+1, false);
        ImGui::SameLine();
        if (ImGui::Button("< prev (mirror X)")) copy_from(-1, true);
        ImGui::SameLine();
        if (ImGui::Button("next > (mirror X)")) copy_from(+1, true);

        ImGui::Separator();

        // Scrollable list — each row: color swatch + kind label + u/v.
        ImGui::BeginChild("light_list", ImVec2(0, 160),
                          ImGuiChildFlags_Border);
        for (int i = 0; i < (int)cur_lights.size(); ++i) {
            LightSpot& ls = cur_lights[i];
            ImGui::PushID(i);
            char label[128];
            std::snprintf(label, sizeof(label),
                          "#%d  %s  @(%.2f,%.2f)", i,
                          kKindLabels[(int)ls.kind], ls.u, ls.v);
            if (ImGui::Selectable(label, i == g_sel_light)) {
                g_sel_light = i;
            }
            ImGui::PopID();
        }
        ImGui::EndChild();

        ImGui::Separator();

        if (g_sel_light >= 0 && g_sel_light < (int)cur_lights.size()) {
            LightSpot& ls = cur_lights[g_sel_light];
            ImGui::Text("Selected light #%d", g_sel_light);

            ImGui::DragFloat("u", &ls.u, 0.002f, 0.0f, 1.0f, "%.3f");
            ImGui::DragFloat("v", &ls.v, 0.002f, 0.0f, 1.0f, "%.3f");

            // Edits to colour / hz / phase / kind also update the
            // module-static stickies so the next placed spot inherits the
            // value the user just dialled in. Size is intentionally NOT
            // sticky (see g_sticky_color comment).
            float rgb[3] = { ls.color.X, ls.color.Y, ls.color.Z };
            if (ImGui::ColorEdit3("color", rgb)) {
                ls.color.X = rgb[0];
                ls.color.Y = rgb[1];
                ls.color.Z = rgb[2];
                g_sticky_color = ls.color;
            }

            ImGui::SliderFloat("size",  &ls.size,  5.0f, 300.0f, "%.0f");
            if (ImGui::SliderFloat("hz",    &ls.hz,    0.0f, 5.0f,   "%.2f")) {
                g_sticky_hz = ls.hz;
            }
            if (ImGui::SliderFloat("phase", &ls.phase, 0.0f, 1.0f,   "%.2f")) {
                g_sticky_phase = ls.phase;
            }

            int kind_i = (int)ls.kind;
            if (ImGui::Combo("kind", &kind_i, kKindLabels,
                             IM_ARRAYSIZE(kKindLabels))) {
                ls.kind = (LightKind)kind_i;
                g_sticky_kind = ls.kind;
            }

            ImGui::Separator();
            if (ImGui::Button("delete")) {
                cur_lights.erase(cur_lights.begin() + g_sel_light);
                g_sel_light = -1;
            }
        } else {
            ImGui::TextDisabled("Click a light to edit, or click the sprite to add one.");
        }
    }
    ImGui::EndChild();

    // -----------------------------------------------------------------
    // Auto-save: walk EVERY editable target, save any whose lights have
    // changed since their last snapshot. First sighting of a target seeds
    // its snapshot from current state without saving (matches what was
    // just loaded from disk). Walking all targets per-frame catches edits
    // even if the user has navigated away from the cell — important
    // because copy-from-prev mutates a cell's lights in place and the
    // user might switch off it before this code runs.
    //
    // O(N_targets) per frame; N is ~85 (mining base + 80 ship cells).
    // lights_equal is field-by-field on small vectors, microseconds total.
    // -----------------------------------------------------------------
    for (int i = 0; i < total; ++i) {
        const SpriteArt*        art    = nullptr;
        std::vector<LightSpot>* lights = nullptr;
        const std::string*      name   = nullptr;
        resolve_target(i, art, lights, name);
        if (!lights || !name) continue;
        auto it = g_save_snapshots.find(*name);
        if (it == g_save_snapshots.end()) {
            // First sighting — adopt current state as the baseline. The
            // contents already match disk because load_sprite_art seeded
            // light_spots from the sidecar at startup.
            g_save_snapshots.emplace(*name, *lights);
        } else if (!lights_equal(*lights, it->second)) {
            save_lights_sidecar(*name, *lights);
            it->second = *lights;
        }
    }

    ImGui::End();
}

} // namespace sprite_light_editor

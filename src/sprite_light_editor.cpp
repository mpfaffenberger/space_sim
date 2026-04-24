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
#include <fstream>
#include <sstream>
#include <string>

namespace sprite_light_editor {

// ---------------------------------------------------------------------------
// Persistent editor state.
// ---------------------------------------------------------------------------

// Visibility: hidden by default. F2 toggles. Starting visible would clutter
// every screenshot — same rationale as debug_panel's g_visible.
static bool g_visible = false;

// Which sprite in the passed vector is being edited. `-1` before any sprite
// exists or when the vector is empty; clamped otherwise.
static int  g_sel_sprite = 0;

// Index into the selected sprite's lights vector, or -1 if no selection.
// Cleared when switching sprites or deleting the currently-selected light.
static int  g_sel_light  = -1;

// Pixel-to-world zoom of the sprite preview. 1.0 = the PNG renders at its
// native pixel size; higher zooms help hit small hull features (antenna
// tips, radar dish). Clamped to [1, 4].
static float g_zoom = 1.0f;

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

void build(std::vector<SpriteObject>& sprites) {
    if (!g_visible) return;
    if (sprites.empty()) return;

    g_sel_sprite = std::max(0, std::min(g_sel_sprite, (int)sprites.size() - 1));
    SpriteObject& sprite = sprites[g_sel_sprite];
    if (!sprite.art) return;

    ImGui::SetNextWindowSize(ImVec2(900, 620), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Sprite Light Editor (F2)", &g_visible)) {
        ImGui::End();
        return;
    }

    // ---- Sprite selector + action buttons -------------------------------
    if (ImGui::BeginCombo("sprite", sprite.art->name.c_str())) {
        for (int i = 0; i < (int)sprites.size(); ++i) {
            if (!sprites[i].art) continue;
            const bool is_sel = (i == g_sel_sprite);
            if (ImGui::Selectable(sprites[i].art->name.c_str(), is_sel)) {
                g_sel_sprite = i;
                g_sel_light  = -1;    // reset selection when changing sprite
            }
            if (is_sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("save sidecar")) {
        save_lights_sidecar(sprite.art->name, sprite.lights);
    }
    ImGui::SameLine();
    if (ImGui::Button("reload sidecar")) {
        std::vector<LightSpot> fresh;
        if (load_lights_sidecar(sprite.art->name, fresh)) {
            sprite.lights = std::move(fresh);
            g_sel_light = -1;
        }
    }
    ImGui::SameLine();
    ImGui::SliderFloat("zoom", &g_zoom, 1.0f, 4.0f, "%.1fx");

    ImGui::Separator();

    // ---- Two-column layout: image on left, light list/editor on right ---
    // Calculated so the image never forces the window wider than the user
    // dragged it — if the window is narrower than needed, we just scroll.
    const float img_size = sprite.art->hull_w * g_zoom;

    ImGui::BeginChild("image_col", ImVec2(img_size + 20, 0),
                      ImGuiChildFlags_Border);
    {
        const ImVec2 img_pos = ImGui::GetCursorScreenPos();
        ImGui::Image(simgui_imtextureid(sprite.art->hull.view),
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

        for (int i = 0; i < (int)sprite.lights.size(); ++i) {
            const LightSpot& ls = sprite.lights[i];
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
                LightSpot ls{};
                ls.u = mouse_uv.x;
                ls.v = mouse_uv.y;
                ls.color = HMM_V3(1.0f, 0.85f, 0.30f);  // warm yellow default
                ls.size  = 60.0f;
                ls.hz    = 0.0f;
                ls.phase = 0.0f;
                ls.kind  = LightKind::Steady;
                sprite.lights.push_back(ls);
                g_sel_light = (int)sprite.lights.size() - 1;
            }
        }

        // Drag-to-move the selected light. Only active while LMB is held
        // AND the drag started on the selected gizmo (to avoid grabbing a
        // light every time the user clicks nearby).
        if (g_sel_light >= 0 && image_hovered &&
            ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f)) {
            LightSpot& ls = sprite.lights[g_sel_light];
            ls.u = std::min(1.0f, std::max(0.0f, mouse_uv.x));
            ls.v = std::min(1.0f, std::max(0.0f, mouse_uv.y));
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // ---- Right column: light list + selected-light editor ---------------
    ImGui::BeginChild("edit_col", ImVec2(0, 0), ImGuiChildFlags_Border);
    {
        ImGui::Text("%zu light%s", sprite.lights.size(),
                    sprite.lights.size() == 1 ? "" : "s");
        ImGui::SameLine();
        if (ImGui::Button("+ add")) {
            LightSpot ls{};
            ls.u = 0.5f; ls.v = 0.5f;
            ls.color = HMM_V3(1.0f, 0.85f, 0.30f);
            ls.size = 60.0f;
            sprite.lights.push_back(ls);
            g_sel_light = (int)sprite.lights.size() - 1;
        }
        ImGui::Separator();

        // Scrollable list — each row: color swatch + kind label + u/v.
        ImGui::BeginChild("light_list", ImVec2(0, 160),
                          ImGuiChildFlags_Border);
        for (int i = 0; i < (int)sprite.lights.size(); ++i) {
            LightSpot& ls = sprite.lights[i];
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

        if (g_sel_light >= 0 && g_sel_light < (int)sprite.lights.size()) {
            LightSpot& ls = sprite.lights[g_sel_light];
            ImGui::Text("Selected light #%d", g_sel_light);

            ImGui::DragFloat("u", &ls.u, 0.002f, 0.0f, 1.0f, "%.3f");
            ImGui::DragFloat("v", &ls.v, 0.002f, 0.0f, 1.0f, "%.3f");

            float rgb[3] = { ls.color.X, ls.color.Y, ls.color.Z };
            if (ImGui::ColorEdit3("color", rgb)) {
                ls.color.X = rgb[0];
                ls.color.Y = rgb[1];
                ls.color.Z = rgb[2];
            }

            ImGui::SliderFloat("size",  &ls.size,  5.0f, 300.0f, "%.0f");
            ImGui::SliderFloat("hz",    &ls.hz,    0.0f, 5.0f,   "%.2f");
            ImGui::SliderFloat("phase", &ls.phase, 0.0f, 1.0f,   "%.2f");

            int kind_i = (int)ls.kind;
            if (ImGui::Combo("kind", &kind_i, kKindLabels,
                             IM_ARRAYSIZE(kKindLabels))) {
                ls.kind = (LightKind)kind_i;
            }

            ImGui::Separator();
            if (ImGui::Button("delete")) {
                sprite.lights.erase(sprite.lights.begin() + g_sel_light);
                g_sel_light = -1;
            }
        } else {
            ImGui::TextDisabled("Click a light to edit, or click the sprite to add one.");
        }
    }
    ImGui::EndChild();

    ImGui::End();
}

} // namespace sprite_light_editor

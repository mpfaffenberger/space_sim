// -----------------------------------------------------------------------------
// debug_panel.cpp — Dear ImGui panel implementation.
//
// The panel is organised as a per-ship collapsible header with length +
// rotation + position sliders. Each slider writes directly into the live
// PlacedMesh struct, so the effect is instant on the next frame. When
// you're happy with the numbers, copy them back into the system JSON.
// -----------------------------------------------------------------------------

#include "debug_panel.h"
#include "mesh_render.h"   // PlacedMesh

// Dear ImGui + sokol backend. The sokol_imgui.h header is both the
// declaration and implementation — we define SOKOL_IMGUI_IMPL here so it
// expands into real code exactly once in the whole program.
#include "imgui.h"
#define SOKOL_IMGUI_IMPL
#include "sokol_imgui.h"

#include <cstdio>

namespace debug_panel {

// Hidden by default — the panel is a dev affordance that clutters every
// screenshot and overlaps half the scene. Ctrl+M toggles it back on.
// This flag is file-scope static because there's exactly one panel and
// persisting the intent to show/hide across frames is inherently global.
static bool g_visible = false;

void init() {
    simgui_desc_t d{};
    d.logger.func = nullptr;     // use sokol's default logger
    simgui_setup(&d);
    std::printf("[debug_panel] Dear ImGui initialised (hidden; Ctrl+M to toggle)\n");
}

void shutdown() {
    simgui_shutdown();
}

bool handle_event(const sapp_event* e) {
    // Intercept the toggle combo BEFORE forwarding to ImGui so it works
    // even when a widget has keyboard focus. Ctrl rather than Cmd on
    // macOS — Cmd+M is reserved by the OS for "minimise window", which
    // would be an incredibly frustrating collision.
    if (e->type == SAPP_EVENTTYPE_KEY_DOWN &&
        e->key_code == SAPP_KEYCODE_M &&
        (e->modifiers & SAPP_MODIFIER_CTRL)) {
        g_visible = !g_visible;
        return true;                // consumed — don't let the game see it
    }
    return simgui_handle_event(e);
}

// Clamp helper — ImGui sliders need sane bounds, and ship sizes span
// three orders of magnitude (fighters → capital ships).
static constexpr float kLengthMin  =    50.0f;
static constexpr float kLengthMax  = 10000.0f;
static constexpr float kPosRange   = 50000.0f;

void build(std::vector<PlacedMesh>& placed_meshes) {
    // Always tick ImGui's frame — keeps the backend's state machine happy
    // even when no window is visible, so flipping `g_visible` mid-game
    // doesn't confuse Dear ImGui about whether a frame is in flight.
    simgui_new_frame({
        sapp_width(),
        sapp_height(),
        sapp_frame_duration(),
        sapp_dpi_scale(),
    });

    if (!g_visible) return;

    if (ImGui::Begin("Ship Sizer", &g_visible,
                     ImGuiWindowFlags_AlwaysAutoResize)) {

        ImGui::TextUnformatted("Live per-ship tweaks. Transcribe to JSON when happy.");
        ImGui::Separator();

        // One collapsible section per ship. ImGui generates a unique ID
        // from the name string — collision-free as long as no two ships
        // share the same OBJ path, which they don't in our flow.
        for (auto& pm : placed_meshes) {
            ImGui::PushID(pm.name.c_str());

            if (ImGui::CollapsingHeader(pm.name.c_str(),
                                        ImGuiTreeNodeFlags_DefaultOpen)) {
                // Length slider — we present it as "meters along longest
                // axis" because that's the mental model the JSON uses.
                // Convert to/from the underlying `scale` on the fly.
                const float ext = pm.mesh.longest_extent();
                float length = pm.scale * ext;
                if (ImGui::SliderFloat("length (m)", &length,
                                       kLengthMin, kLengthMax,
                                       "%.0f", ImGuiSliderFlags_Logarithmic)) {
                    if (ext > 1e-6f) pm.scale = length / ext;
                }

                // Rotation — split into three separate sliders, degrees,
                // wraparound [-180, 180]. HandmadeMath multiplies in RH
                // convention so these match the JSON `euler_deg` field.
                ImGui::SliderFloat("pitch (deg)", &pm.euler_deg.X, -180.0f, 180.0f, "%.1f");
                ImGui::SliderFloat("yaw (deg)",   &pm.euler_deg.Y, -180.0f, 180.0f, "%.1f");
                ImGui::SliderFloat("roll (deg)",  &pm.euler_deg.Z, -180.0f, 180.0f, "%.1f");

                // Position — range is symmetric around origin and wide
                // enough to yeet any ship into the outer reaches of the
                // sector if needed.
                ImGui::DragFloat3("position", &pm.position.X, 10.0f,
                                  -kPosRange, kPosRange, "%.0f");

                ImGui::ColorEdit3("tint", &pm.body_tint.X);
                ImGui::SliderFloat("spec", &pm.spec_amount, 0.0f, 2.0f, "%.2f");

                // Copy-to-clipboard — emit the JSON snippet for this
                // ship so Mike can paste it straight into troy.json.
                if (ImGui::Button("copy JSON")) {
                    char buf[512];
                    std::snprintf(buf, sizeof(buf),
                        "{ \"obj\": \"%s\", "
                        "\"position\": [%.0f, %.0f, %.0f], "
                        "\"euler_deg\": [%.1f, %.1f, %.1f], "
                        "\"length_meters\": %.0f, "
                        "\"tint\": [%.2f, %.2f, %.2f], "
                        "\"spec\": %.2f }",

                        pm.name.c_str(),
                        pm.position.X, pm.position.Y, pm.position.Z,
                        pm.euler_deg.X, pm.euler_deg.Y, pm.euler_deg.Z,
                        pm.scale * ext,
                        pm.body_tint.X, pm.body_tint.Y, pm.body_tint.Z,
                        pm.spec_amount);
                    ImGui::SetClipboardText(buf);
                }
            }
            ImGui::PopID();
        }
    }
    ImGui::End();
}

void render() {
    // Must be called inside an active swapchain pass — issues the
    // draw commands for whatever was built this frame.
    simgui_render();
}

} // namespace debug_panel

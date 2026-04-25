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
#include "ship_sprite.h"

// Dear ImGui + sokol backend. The sokol_imgui.h header is both the
// declaration and implementation — we define SOKOL_IMGUI_IMPL here so it
// expands into real code exactly once in the whole program.
#include "imgui.h"
#define SOKOL_IMGUI_IMPL
#include "sokol_imgui.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>

namespace debug_panel {

// Hidden by default — the panel is a dev affordance that clutters every
// screenshot and overlaps half the scene. Ctrl+M toggles it back on.
// This flag is file-scope static because there's exactly one panel and
// persisting the intent to show/hide across frames is inherently global.
static bool g_visible = false;
static std::unordered_map<std::string, std::string> g_prompt_notes;

static std::string basename(std::string path) {
    const size_t slash = path.find_last_of("/");
    if (slash != std::string::npos) path = path.substr(slash + 1);
    return path;
}

static std::string trimmed_imgui_buffer(std::string s) {
    const size_t nul = s.find('\0');
    if (nul != std::string::npos) s.resize(nul);
    return s;
}

static void save_ship_tuning(const ShipSpriteAtlas& atlas) {
    const std::filesystem::path path = std::filesystem::path("assets") / (atlas.key + ".tuning.json");
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path);
    out << "{\n  \"ship\": \"tarsus\",\n  \"frames\": [\n";
    for (size_t i = 0; i < atlas.frames.size(); ++i) {
        const ShipSpriteFrame& f = atlas.frames[i];
        out << "    { \"az\": " << f.az_deg
            << ", \"el\": " << f.el_deg
            << ", \"scale\": " << f.scale
            << ", \"roll_deg\": " << f.roll_deg << " }";
        out << (i + 1 == atlas.frames.size() ? "\n" : ",\n");
    }
    out << "  ]\n}\n";
}

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

static void build_atlas_inspector(std::vector<ShipSpriteObject>& ship_sprites) {
    if (ship_sprites.empty()) return;

    if (!ImGui::CollapsingHeader("Ship Sprite Atlas Inspector",
                                 ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }

    ImGui::TextWrapped("Scrub atlas az/el while the camera stays fixed. Use this to find weak generated frames before spending more API money like a gremlin.");
    ImGui::Separator();

    for (int i = 0; i < (int)ship_sprites.size(); ++i) {
        ShipSpriteObject& ship = ship_sprites[i];
        if (!ship.atlas) continue;

        ImGui::PushID(i);
        ImGui::Checkbox("manual frame override", &ship.manual_frame_enabled);
        ImGui::SameLine();
        ImGui::Text("current: az %.0f  el %.0f", ship.debug_last_az_deg, ship.debug_last_el_deg);

        ImGui::SliderFloat("manual az", &ship.manual_az_deg, 0.0f, 315.0f, "%.0f deg");
        ImGui::SliderFloat("manual el", &ship.manual_el_deg, -68.0f, 68.0f, "%.0f deg");

        ShipSpriteFrame* frame = const_cast<ShipSpriteFrame*>(choose_ship_sprite_frame_by_angles(
            *ship.atlas, ship.manual_az_deg, ship.manual_el_deg));
        if (frame && frame->art) {
            ImGui::Text("nearest frame: az %.0f  el %.0f", frame->az_deg, frame->el_deg);
            ImGui::SliderFloat("frame scale", &frame->scale, 0.5f, 1.8f, "%.3f");
            ImGui::SliderFloat("frame roll", &frame->roll_deg, -20.0f, 20.0f, "%.2f deg");
            if (ImGui::Button("save frame tuning")) {
                save_ship_tuning(*ship.atlas);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("writes assets/%s.tuning.json", ship.atlas->key.c_str());

            const float longest = (float)std::max(frame->art->hull_w, frame->art->hull_h);
            const float preview_w = 260.0f * (float)frame->art->hull_w / longest;
            const float preview_h = 260.0f * (float)frame->art->hull_h / longest;
            ImGui::Image(simgui_imtextureid(frame->art->hull.view), ImVec2(preview_w, preview_h));

            const std::string key = frame->art->name;
            const std::filesystem::path tweak_dir = "assets/ships/tarsus/sprites/prompt_tweaks";
            const std::filesystem::path tweak_file = tweak_dir / (basename(key) + ".txt");
            std::string& notes = g_prompt_notes[key];
            if (notes.empty() && std::filesystem::exists(tweak_file)) {
                std::ifstream in(tweak_file);
                notes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            }
            notes.resize(1024, '\0');
            if (ImGui::InputTextMultiline("prompt tuning notes", notes.data(), notes.size(), ImVec2(420, 100))) {
                const size_t nul = notes.find('\0');
                if (nul != std::string::npos) notes.resize(nul);
            }
            if (ImGui::Button("save notes")) {
                std::filesystem::create_directories(tweak_dir);
                std::ofstream out(tweak_file);
                out << trimmed_imgui_buffer(notes);
            }
            ImGui::SameLine();
            if (ImGui::Button("copy regenerate command")) {
                char cmd[1200];
                std::snprintf(cmd, sizeof(cmd),
                    "python3 tools/batch_generate_ship_sprites.py --ship tarsus --az %.0f --el %.0f --prompt-suffix-file %s --use-pixelart-tool --clean --preview --force",
                    frame->az_deg, frame->el_deg, tweak_file.string().c_str());
                ImGui::SetClipboardText(cmd);
            }
            ImGui::TextDisabled("notes file: %s", tweak_file.string().c_str());
        }
        ImGui::PopID();
    }
}

void build(std::vector<PlacedMesh>& placed_meshes,
           std::vector<ShipSpriteObject>& ship_sprites) {
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
        build_atlas_inspector(ship_sprites);
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

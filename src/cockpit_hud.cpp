// cockpit_hud.cpp — implementation. See header for the elevator pitch.
//
// Architectural notes:
//   * Module is *pure draw*, no state of its own. All inputs come
//     through build()'s parameters; all outputs go through ImGui's
//     foreground drawlist or short-lived ImGui windows. Restartable
//     and testable in isolation.
//   * Per-frame allocations are zero — even the kind→colour lookup
//     compares short std::strings that almost always intern to a
//     small fixed set. If kinds proliferate, swap to an enum.
//   * Two engine quirks are honoured here, both also called out in
//     main.cpp's older HUD code:
//       (1) ImGui drawlist coords are LOGICAL pixels — divide
//           sapp_width()/_height() by sapp_dpi_scale() before use.
//       (2) Our perspective pipeline maps world-up to +screen_y
//           (no NDC Y-flip), so target projection skips the
//           textbook (1 - ndc_y) inversion.
#include "cockpit_hud.h"

#include "armor.h"
#include "camera.h"
#include "perception.h"
#include "shield.h"
#include "ship.h"
#include "ship_class.h"
#include "ship_sprite.h"
#include "system_def.h"

#include "imgui.h"
#include "sokol_app.h"
#include "sokol_imgui.h"   // simgui_imtextureid for sprite thumbnails

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>

namespace cockpit_hud {

namespace {

// ---- shared helpers ------------------------------------------------------

// Logical-pixel framebuffer dims. ImGui windows + drawlists use these,
// not raw sapp_width()/_height() which return Retina-scaled physical pixels.
struct ScreenSize { float w, h, dpi; };
ScreenSize screen_size() {
    const float dpi = sapp_dpi_scale();
    return { (float)sapp_width()  / dpi,
             (float)sapp_height() / dpi,
             dpi };
}

// Palette — a small, deliberate set of HUD colours so every panel sings
// the same tune. Amber matches our nav-target reticle; cyan/green/blue
// are radar dot colours per nav kind.
static const ImU32 kAmber    = IM_COL32(255, 217,  77, 240);
static const ImU32 kDimAmber = IM_COL32(180, 150,  60, 200);
static const ImU32 kCyan     = IM_COL32(120, 220, 255, 240);
static const ImU32 kGreen    = IM_COL32(120, 240, 140, 240);
static const ImU32 kBlueP    = IM_COL32( 90, 160, 255, 240);
static const ImU32 kHudWhite = IM_COL32(220, 230, 235, 220);
static const ImU32 kPanelBg  = IM_COL32( 10,  14,  20, 220);

// Map a nav kind to its radar/MFD dot colour. String compare is fine —
// nav_points is small and this loop is dwarfed by ImGui call overhead.
ImU32 color_for_kind(const std::string& kind) {
    if (kind == "jump")    return kCyan;
    if (kind == "station") return kGreen;
    if (kind == "planet")  return kBlueP;
    return kHudWhite;
}

// Common flag set for HUD windows: locked-in-place, no chrome, no input.
constexpr ImGuiWindowFlags kHudWindowFlags =
    ImGuiWindowFlags_NoTitleBar         | ImGuiWindowFlags_NoResize        |
    ImGuiWindowFlags_NoMove             | ImGuiWindowFlags_NoScrollbar     |
    ImGuiWindowFlags_NoCollapse         | ImGuiWindowFlags_NoSavedSettings |
    ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav           |
    ImGuiWindowFlags_NoInputs;

// Push the standard HUD window styling (dark-bg + amber border). Pair
// with pop_hud_style() — uses 2 colours + 2 vars.
void push_hud_style() {
    ImGui::PushStyleColor(ImGuiCol_WindowBg, kPanelBg);
    ImGui::PushStyleColor(ImGuiCol_Border,   kAmber);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2(8.0f, 6.0f));
}
void pop_hud_style() {
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

// ---- gun crosshair (centre) ---------------------------------------------
//
// Distinct from the nav reticle in role and colour: this one says "here
// is where the guns will fire" and never moves; the amber reticle says
// "here is where the target is" and slides around. Two pieces of data,
// two visuals — never overload one symbol with two meanings.
//
// Also doubles as the fly-by-wire dead-zone visualisation: a faint
// extra ring (radius matching Camera::mouse_dead_zone × half-screen)
// shows where the cursor stops driving turn. Inside the ring → ship
// flies straight. Outside → ship turns.
void draw_crosshair(bool fly_by_wire) {
    const auto s = screen_size();
    const float cx = s.w * 0.5f, cy = s.h * 0.5f;
    auto* dl = ImGui::GetForegroundDrawList();

    constexpr float arm = 10.0f;     // half-length of each crosshair tick
    constexpr float gap = 4.0f;      // empty space at the centre

    dl->AddLine(ImVec2(cx - arm, cy), ImVec2(cx - gap, cy), kHudWhite, 1.5f);
    dl->AddLine(ImVec2(cx + gap, cy), ImVec2(cx + arm, cy), kHudWhite, 1.5f);
    dl->AddLine(ImVec2(cx, cy - arm), ImVec2(cx, cy - gap), kHudWhite, 1.5f);
    dl->AddLine(ImVec2(cx, cy + gap), ImVec2(cx, cy + arm), kHudWhite, 1.5f);
    dl->AddCircleFilled(ImVec2(cx, cy), 1.5f, kHudWhite);

    // Dead-zone ring — only meaningful while flying. Ratio mirrors the
    // Camera::mouse_dead_zone default (0.05); keep them numerically in
    // sync if you change one. (TODO: pass dead_zone in if it ever
    // becomes per-ship tunable.)
    if (fly_by_wire) {
        const float dz_radius = std::min(cx, cy) * 0.05f;
        const ImU32 dz_col    = IM_COL32(220, 230, 235, 70);
        dl->AddCircle(ImVec2(cx, cy), dz_radius, dz_col, 0, 1.0f);
    }
}

// ---- aim cursor (mouse position, fly-by-wire mode) -----------------------
//
// In fly-by-wire mode the OS cursor is hidden (sapp_show_mouse(false)).
// We draw our own amber crosshair-cursor at the mouse position so the
// pilot has a clear visual for 'where I'm pointing the nose'. In free-
// cursor mode this draws nothing — the OS cursor is back, and rendering
// our own would just double up.
void draw_aim_cursor(float mouse_x, float mouse_y, bool fly_by_wire) {
    if (!fly_by_wire) return;
    auto* dl = ImGui::GetForegroundDrawList();
    constexpr float r = 5.0f;
    dl->AddCircle(ImVec2(mouse_x, mouse_y), r, kAmber, 0, 1.5f);
    // Tiny tick marks at N/E/S/W of the cursor — reads as 'crosshair'
    // even from peripheral vision without crowding the centre dot.
    constexpr float tick = 3.0f, gap = 1.5f;
    dl->AddLine(ImVec2(mouse_x, mouse_y - r - gap),
                ImVec2(mouse_x, mouse_y - r - gap - tick), kAmber, 1.5f);
    dl->AddLine(ImVec2(mouse_x, mouse_y + r + gap),
                ImVec2(mouse_x, mouse_y + r + gap + tick), kAmber, 1.5f);
    dl->AddLine(ImVec2(mouse_x - r - gap, mouse_y),
                ImVec2(mouse_x - r - gap - tick, mouse_y), kAmber, 1.5f);
    dl->AddLine(ImVec2(mouse_x + r + gap, mouse_y),
                ImVec2(mouse_x + r + gap + tick, mouse_y), kAmber, 1.5f);
}

// ---- nav target reticle --------------------------------------------------
//
// Floats over the projected screen position of the selected nav point.
// Edge-clamps with a chevron when off-screen / behind the camera.
void draw_nav_reticle(const Camera& cam, const StarSystem& system, int selected_nav) {
    if (selected_nav < 0 || selected_nav >= (int)system.nav_points.size()) return;

    const auto&    nav    = system.nav_points[selected_nav];
    const HMM_Vec3 target = nav.position;
    const HMM_Vec3 d      = HMM_SubV3(target, cam.position);
    const float    len    = HMM_LenV3(d);

    const auto s = screen_size();
    const float fb_w = s.w, fb_h = s.h;
    const float cx = fb_w * 0.5f, cy = fb_h * 0.5f;
    constexpr float margin = 40.0f;

    const float fwd_dot   = HMM_DotV3(d, cam.forward());
    const float right_dot = HMM_DotV3(d, cam.right());
    const float up_dot    = HMM_DotV3(d, cam.up());
    const bool  behind    = (fwd_dot <= 0.0f);

    float sx = cx, sy = cy;
    bool  clamped = behind;

    if (!behind) {
        // NOTE: NO (1 - ndc_y) flip here — engine quirk, see header.
        const float    aspect = fb_w / fb_h;
        const HMM_Mat4 vp     = HMM_MulM4(cam.projection(aspect), cam.view());
        const HMM_Vec4 ph     = { target.X, target.Y, target.Z, 1.0f };
        const HMM_Vec4 clip   = HMM_MulM4V4(vp, ph);
        const float    ndc_x  = clip.X / clip.W;
        const float    ndc_y  = clip.Y / clip.W;
        sx = (ndc_x * 0.5f + 0.5f) * fb_w;
        sy = (ndc_y * 0.5f + 0.5f) * fb_h;
        clamped = !(sx >= margin && sx <= fb_w - margin &&
                    sy >= margin && sy <= fb_h - margin);
    }

    if (clamped) {
        const float sign = behind ? -1.0f : 1.0f;
        float dx = sign * right_dot;
        float dy = sign * up_dot;          // engine Y quirk: no flip
        const float dlen = std::sqrt(dx * dx + dy * dy);
        if (dlen < 1e-6f) { dx = 0.0f; dy = 1.0f; }
        else              { dx /= dlen; dy /= dlen; }
        const float max_x = cx - margin;
        const float max_y = cy - margin;
        const float tx = std::abs(dx) > 1e-6f ? max_x / std::abs(dx) : 1e9f;
        const float ty = std::abs(dy) > 1e-6f ? max_y / std::abs(dy) : 1e9f;
        const float t  = std::min(tx, ty);
        sx = cx + dx * t;
        sy = cy + dy * t;
    }

    auto* dl = ImGui::GetForegroundDrawList();
    constexpr float r = 14.0f;
    dl->AddCircle(ImVec2(sx, sy), r, kAmber, 0, 2.0f);
    const float tick_in = r * 0.45f, tick_out = r * 0.85f;
    dl->AddLine(ImVec2(sx - tick_out, sy), ImVec2(sx - tick_in, sy), kAmber, 2.0f);
    dl->AddLine(ImVec2(sx + tick_in,  sy), ImVec2(sx + tick_out, sy), kAmber, 2.0f);
    dl->AddLine(ImVec2(sx, sy - tick_out), ImVec2(sx, sy - tick_in), kAmber, 2.0f);
    dl->AddLine(ImVec2(sx, sy + tick_in),  ImVec2(sx, sy + tick_out), kAmber, 2.0f);

    if (clamped) {
        const float sign = behind ? -1.0f : 1.0f;
        float dx = sign * right_dot, dy = sign * up_dot;
        const float dlen = std::sqrt(dx * dx + dy * dy);
        if (dlen > 1e-6f) { dx /= dlen; dy /= dlen; }
        const float arr_d = r + 6.0f, arr_t = arr_d + 9.0f;
        const float perp_x = -dy, perp_y = dx;
        const ImVec2 tip { sx + dx * arr_t, sy + dy * arr_t };
        const ImVec2 b1  { sx + dx * arr_d + perp_x * 5.0f,
                           sy + dy * arr_d + perp_y * 5.0f };
        const ImVec2 b2  { sx + dx * arr_d - perp_x * 5.0f,
                           sy + dy * arr_d - perp_y * 5.0f };
        dl->AddTriangleFilled(tip, b1, b2, kAmber);
    }

    char buf[32];
    if (len < 10000.0f) std::snprintf(buf, sizeof(buf), "%.0f u",   len);
    else                std::snprintf(buf, sizeof(buf), "%.1f k u", len * 0.001f);
    const ImVec2 ts = ImGui::CalcTextSize(buf);
    const bool below_ok = (sy + r + 4.0f + ts.y) < (fb_h - 4.0f);
    const float label_y = below_ok ? sy + r + 4.0f : sy - r - 4.0f - ts.y;
    dl->AddText(ImVec2(sx - ts.x * 0.5f, label_y), kAmber, buf);
}

// ---- target MFD (bottom-right) -------------------------------------------
//
// Compact data panel — TARGET name, DIST, AZ/EL. No graphs, no
// animation; the player's reading this during combat and doesn't
// need eye-candy fighting for attention.
// Bottom-right NAV panel — info on the currently-cycled nav point
// (selected with the N key). Renamed from "TARGET" since a real ship
// target panel now sits separately (top-right) — "target" should mean
// "the ship I'm shooting at", not "the nav point I'm flying to".
void draw_nav_mfd(const Camera& cam, const StarSystem& system, int selected_nav) {
    const auto s = screen_size();
    constexpr float w = 240.0f, h = 96.0f, margin = 16.0f;

    ImGui::SetNextWindowPos(ImVec2(s.w - w - margin, s.h - h - margin),
                            ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.55f);
    push_hud_style();

    if (ImGui::Begin("##nav_mfd", nullptr, kHudWindowFlags)) {
        ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
        ImGui::TextUnformatted("NAV");
        ImGui::PopStyleColor();
        ImGui::Separator();

        if (selected_nav < 0 || selected_nav >= (int)system.nav_points.size()) {
            ImGui::PushStyleColor(ImGuiCol_Text, kDimAmber);
            ImGui::TextUnformatted("[ NO NAV SELECTED ]");
            ImGui::TextUnformatted("press N to cycle");
            ImGui::PopStyleColor();
        } else {
            const auto& nav = system.nav_points[selected_nav];
            const HMM_Vec3 d = HMM_SubV3(nav.position, cam.position);
            const float len  = HMM_LenV3(d);

            const float fwd_dot   = HMM_DotV3(d, cam.forward());
            const float right_dot = HMM_DotV3(d, cam.right());
            const float up_dot    = HMM_DotV3(d, cam.up());
            constexpr float kRad2Deg = 57.2957795f;
            const float az_deg = std::atan2(right_dot, fwd_dot) * kRad2Deg;
            const float horiz  = std::sqrt(fwd_dot * fwd_dot + right_dot * right_dot);
            const float el_deg = std::atan2(up_dot, horiz) * kRad2Deg;

            std::string kind_up = nav.kind;
            for (char& c : kind_up) c = (char)std::toupper((unsigned char)c);

            ImGui::PushStyleColor(ImGuiCol_Text, kHudWhite);
            ImGui::Text("%s  %s", kind_up.c_str(), nav.name.c_str());
            if (len < 10000.0f) ImGui::Text("DIST  %7.0f u",   len);
            else                ImGui::Text("DIST  %6.1f k u", len * 0.001f);
            ImGui::Text("AZ %+4.0f  EL %+3.0f", az_deg, el_deg);
            ImGui::PopStyleColor();
        }
    }
    ImGui::End();
    pop_hud_style();
}

// ---- radar MFD (bottom-left) ---------------------------------------------
//
// Top-down radar in the camera's local frame: player at centre as a
// triangle, radar +Y = camera forward, radar +X = starboard. World-
// space nav points project onto the camera's right/forward plane and
// scale into the disc; beyond max_range, dots clamp to the rim with
// no extra fanfare. A slow sweep line (~one revolution per 4 s)
// keeps the HUD feeling 'live' even when nothing is moving.
//
// Top-right TARGET panel — info on the currently-locked SHIP target
// (T-key cycles). Shows a per-frame thumbnail of the target sprite
// (picked via the same camera-relative az/el lookup the main render
// uses), class label, faction stance, distance, and shield/armor
// breakdown per facing as horizontal bars.
//
// Distinct from the NAV panel (bottom-right) which tracks the
// nav-point N-key cycle. Both panels can be active simultaneously.
void draw_target_mfd(const Camera& cam, const std::vector<Ship>& ships,
                     uint32_t target_ship_id) {
    // Resolve target id -> Ship*. Linear scan; ships count is small.
    const Ship* target = nullptr;
    for (const Ship& s : ships) {
        if (s.id == target_ship_id) { target = &s; break; }
    }

    const auto sz = screen_size();
    constexpr float w = 280.0f, h = 152.0f, margin = 16.0f;
    ImGui::SetNextWindowPos(ImVec2(sz.w - w - margin, margin), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.55f);
    push_hud_style();

    if (ImGui::Begin("##target_mfd", nullptr, kHudWindowFlags)) {
        ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
        ImGui::TextUnformatted("TARGET");
        ImGui::PopStyleColor();
        ImGui::Separator();

        if (!target || !target->alive) {
            ImGui::PushStyleColor(ImGuiCol_Text, kDimAmber);
            ImGui::TextUnformatted("[ NO TARGET ]");
            ImGui::TextUnformatted("press T to cycle");
            ImGui::PopStyleColor();
        } else {
            // Left column: sprite thumbnail (if available). Use the
            // camera-relative cell selector so the thumbnail matches
            // what the player sees out the window.
            constexpr float thumb_w = 80.0f, thumb_h = 80.0f;
            if (target->sprite && target->sprite->atlas) {
                const ShipSpriteFrame* f = choose_ship_sprite_frame(
                    *target->sprite->atlas, *target->sprite, cam);
                if (f && f->art) {
                    ImGui::Image(simgui_imtextureid(f->art->hull.view),
                                 ImVec2(thumb_w, thumb_h));
                } else {
                    ImGui::Dummy(ImVec2(thumb_w, thumb_h));
                }
            } else {
                ImGui::Dummy(ImVec2(thumb_w, thumb_h));
            }
            ImGui::SameLine();

            // Right column: identity + range + stance + HP bars.
            ImGui::BeginGroup();

            const char* class_name = target->klass
                ? target->klass->display_name.c_str()
                : (target->is_player ? "PLAYER" : "?");
            ImGui::PushStyleColor(ImGuiCol_Text, kHudWhite);
            ImGui::Text("%s", class_name);
            ImGui::PopStyleColor();

            // Faction + stance — find the player's perception entry to
            // get the stance the AI uses (so target-panel colors match
            // the on-screen indicator + radar). Distance from the
            // contact entry too — already filtered by radar range.
            const char*  fac_name = target->klass
                ? faction::to_name(target->faction) : "?";
            float        dist_m   = 0.0f;
            Stance       stance   = Stance::Neutral;
            if (!ships.empty() && ships.front().is_player) {
                for (const PerceivedContact& c : ships.front().perception.visible) {
                    if (c.ship_id == target_ship_id) {
                        dist_m = c.distance_m;
                        stance = c.stance;
                        break;
                    }
                }
            }
            const ImU32 stance_col =
                (stance == Stance::Hostile) ? IM_COL32(255,  90,  90, 255)
              : (stance == Stance::Allied)  ? IM_COL32( 90, 255, 110, 255)
              :                                IM_COL32(255, 220,  60, 255);
            const char* stance_str =
                (stance == Stance::Hostile) ? "HOSTILE"
              : (stance == Stance::Allied)  ? "ALLIED"
              :                                "NEUTRAL";

            ImGui::PushStyleColor(ImGuiCol_Text, stance_col);
            ImGui::Text("%s  %s", fac_name, stance_str);
            ImGui::PopStyleColor();

            ImGui::PushStyleColor(ImGuiCol_Text, kHudWhite);
            if (dist_m < 10000.0f) ImGui::Text("DIST  %6.0f m",  dist_m);
            else                   ImGui::Text("DIST  %5.1f km", dist_m * 0.001f);
            ImGui::PopStyleColor();

            ImGui::EndGroup();

            // Bottom: per-facing shield + armor bars. F / A / S labels
            // (Fore / Aft / Side); each bar fills proportionally to
            // current vs. max for that facing. Shield max comes from
            // the fitted ShieldType, armor max from class hull +
            // fitted ArmorType.
            ImGui::Separator();
            const ShipClass* k = target->klass;
            float shield_max[3] = {0,0,0}, armor_max[3] = {0,0,0};
            if (k) {
                if (k->default_shield) {
                    shield_max[0] = k->default_shield->front_cm;
                    shield_max[1] = k->default_shield->back_cm;
                    shield_max[2] = k->default_shield->side_cm;
                }
                armor_max[0] = k->armor_fore_cm;
                armor_max[1] = k->armor_aft_cm;
                armor_max[2] = k->armor_side_cm;
                if (k->default_armor) {
                    armor_max[0] += k->default_armor->front_cm;
                    armor_max[1] += k->default_armor->back_cm;
                    armor_max[2] += k->default_armor->side_cm;
                }
            }
            const float shield_cur[3] = { target->shield_fore_cm,
                                          target->shield_aft_cm,
                                          target->shield_side_cm };
            const float armor_cur[3]  = { target->armor_fore_cm,
                                          target->armor_aft_cm,
                                          target->armor_side_cm };
            const char* facing_lbl[3] = { "F", "A", "S" };

            ImGui::PushStyleColor(ImGuiCol_FrameBg,        IM_COL32(20,20,30,180));
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram,  IM_COL32(80,160,255,220));
            for (int i = 0; i < 3; ++i) {
                const float frac = shield_max[i] > 0.0f
                    ? std::clamp(shield_cur[i] / shield_max[i], 0.0f, 1.0f) : 0.0f;
                char buf[24];
                std::snprintf(buf, sizeof(buf), "S%s %.0f/%.0f",
                              facing_lbl[i], shield_cur[i], shield_max[i]);
                ImGui::ProgressBar(frac, ImVec2(80.0f, 14.0f), buf);
                if (i < 2) ImGui::SameLine();
            }
            ImGui::PopStyleColor(2);

            ImGui::PushStyleColor(ImGuiCol_FrameBg,        IM_COL32(20,20,30,180));
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram,  IM_COL32(255,140,60,220));
            for (int i = 0; i < 3; ++i) {
                const float frac = armor_max[i] > 0.0f
                    ? std::clamp(armor_cur[i] / armor_max[i], 0.0f, 1.0f) : 0.0f;
                char buf[24];
                std::snprintf(buf, sizeof(buf), "A%s %.0f/%.0f",
                              facing_lbl[i], armor_cur[i], armor_max[i]);
                ImGui::ProgressBar(frac, ImVec2(80.0f, 14.0f), buf);
                if (i < 2) ImGui::SameLine();
            }
            ImGui::PopStyleColor(2);
        }
    }
    ImGui::End();
    pop_hud_style();
}

void draw_radar_mfd(const Camera& cam, const StarSystem& system, int selected_nav,
                    const std::vector<Ship>& ships, uint32_t target_ship_id) {
    const auto s = screen_size();
    constexpr float w = 168.0f, h = 168.0f, margin = 16.0f;

    ImGui::SetNextWindowPos(ImVec2(margin, s.h - h - margin), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.55f);

    ImGui::PushStyleColor(ImGuiCol_WindowBg, kPanelBg);
    ImGui::PushStyleColor(ImGuiCol_Border,   kAmber);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2(0.0f, 0.0f));

    if (ImGui::Begin("##radar_mfd", nullptr, kHudWindowFlags)) {
        ImVec2      p0 = ImGui::GetWindowPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        const ImVec2 ctr  = ImVec2(p0.x + w * 0.5f, p0.y + h * 0.5f);
        const float  rad  = std::min(w, h) * 0.5f - 6.0f;
        constexpr float max_range = 250000.0f;       // u — beyond this, clamp to rim

        // Concentric range rings + crosshair. Drawn before sweep so the
        // sweep line passes over them.
        static const ImU32 grid     = IM_COL32(120, 160, 130,  90);
        static const ImU32 grid_dim = IM_COL32(120, 160, 130,  50);
        dl->AddCircle(ctr, rad,         grid,     0, 1.0f);
        dl->AddCircle(ctr, rad * 0.66f, grid_dim, 0, 1.0f);
        dl->AddCircle(ctr, rad * 0.33f, grid_dim, 0, 1.0f);
        dl->AddLine(ImVec2(ctr.x - rad, ctr.y), ImVec2(ctr.x + rad, ctr.y), grid_dim, 1.0f);
        dl->AddLine(ImVec2(ctr.x, ctr.y - rad), ImVec2(ctr.x, ctr.y + rad), grid_dim, 1.0f);

        // Sweep — 12 lines at decreasing alpha to fake a comet trail.
        // Cheap and reads as 'rotating beam' instantly.
        const float t = (float)ImGui::GetTime();
        const float sweep_a = std::fmod(t * 0.25f, 1.0f) * 6.2831853f - 1.5707963f;
        for (int i = 0; i < 12; ++i) {
            const float a   = sweep_a - i * 0.04f;
            const float ca  = std::cos(a), sa = std::sin(a);
            const int   alf = (int)(180 * (1.0f - i / 12.0f));
            const ImU32 col = IM_COL32(140, 220, 150, alf);
            dl->AddLine(ctr,
                        ImVec2(ctr.x + ca * rad, ctr.y + sa * rad),
                        col, 1.5f);
        }

        // Plot every nav point. Camera-relative (right_dot, fwd_dot) maps
        // straight onto the radar's (X, -Y) plane: forward = up on screen.
        for (int i = 0; i < (int)system.nav_points.size(); ++i) {
            const auto& nav = system.nav_points[i];
            const HMM_Vec3 d = HMM_SubV3(nav.position, cam.position);
            const float fwd_dot   = HMM_DotV3(d, cam.forward());
            const float right_dot = HMM_DotV3(d, cam.right());
            const float plane_len = std::sqrt(fwd_dot * fwd_dot + right_dot * right_dot);

            float dx_norm, dy_norm;
            if (plane_len < 1.0f) { dx_norm = 0.0f; dy_norm = 0.0f; }
            else                  { dx_norm =  right_dot / plane_len;
                                    dy_norm = -fwd_dot   / plane_len; }   // forward = up on radar

            const float r_norm = std::min(plane_len / max_range, 1.0f);
            const ImVec2 dot { ctr.x + dx_norm * r_norm * rad,
                               ctr.y + dy_norm * r_norm * rad };
            const ImU32  col = color_for_kind(nav.kind);
            const bool   sel = (i == selected_nav);
            const float  dot_r = sel ? 4.5f : 2.5f;
            dl->AddCircleFilled(dot, dot_r, col);
            if (sel) dl->AddCircle(dot, dot_r + 2.5f, kAmber, 0, 1.5f);
        }

        // Plot ship contacts from the player's perception. Same
        // camera-relative projection as the nav loop above; stance
        // colors mirror the on-screen target indicator (red=hostile,
        // green=allied, yellow=neutral) so the radar reads at a glance.
        // Ships at < ~14% of radar (35 km vs 250 km) cluster near
        // center — that's the steady-state engagement bubble; nav
        // points spread out farther because they're system-scale
        // (planets, jump points 100+ km away).
        if (!ships.empty() && ships.front().is_player) {
            const Ship& player = ships.front();
            for (const PerceivedContact& c : player.perception.visible) {
                // Reconstruct world position from cached unit + distance.
                const HMM_Vec3 contact_pos =
                    HMM_AddV3(player.position, HMM_MulV3F(c.to_unit, c.distance_m));
                const HMM_Vec3 d = HMM_SubV3(contact_pos, cam.position);
                const float fwd_dot   = HMM_DotV3(d, cam.forward());
                const float right_dot = HMM_DotV3(d, cam.right());
                const float plane_len = std::sqrt(fwd_dot * fwd_dot + right_dot * right_dot);

                float dx_norm, dy_norm;
                if (plane_len < 1.0f) { dx_norm = 0.0f; dy_norm = 0.0f; }
                else                  { dx_norm =  right_dot / plane_len;
                                        dy_norm = -fwd_dot   / plane_len; }

                const float r_norm = std::min(plane_len / max_range, 1.0f);
                const ImVec2 dot { ctr.x + dx_norm * r_norm * rad,
                                   ctr.y + dy_norm * r_norm * rad };

                const ImU32 col =
                    (c.stance == Stance::Hostile) ? IM_COL32(255,  90,  90, 255)
                  : (c.stance == Stance::Allied)  ? IM_COL32( 90, 255, 110, 255)
                  :                                 IM_COL32(255, 220,  60, 255);
                const bool   sel = (c.ship_id == target_ship_id);
                const float  dot_r = sel ? 4.0f : 2.5f;
                dl->AddCircleFilled(dot, dot_r, col);
                if (sel) dl->AddCircle(dot, dot_r + 2.5f, kAmber, 0, 1.5f);
            }
        }

        // Player ship — small triangle at centre pointing 'up' (forward).
        const ImVec2 p_tip { ctr.x,        ctr.y - 6.0f };
        const ImVec2 p_bl  { ctr.x - 4.0f, ctr.y + 4.0f };
        const ImVec2 p_br  { ctr.x + 4.0f, ctr.y + 4.0f };
        dl->AddTriangleFilled(p_tip, p_bl, p_br, kHudWhite);

        // Faint label so first-time players know what they're looking at.
        dl->AddText(ImVec2(p0.x + 6.0f, p0.y + 4.0f), kDimAmber, "RADAR");
    }
    ImGui::End();

    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

} // anonymous namespace

void build(const Camera& cam, const StarSystem& system, int selected_nav,
           float mouse_x, float mouse_y, bool fly_by_wire,
           const std::vector<Ship>& ships, uint32_t target_ship_id) {
    draw_crosshair(fly_by_wire);
    draw_aim_cursor(mouse_x, mouse_y, fly_by_wire);
    draw_nav_reticle(cam, system, selected_nav);
    draw_nav_mfd   (cam, system, selected_nav);
    draw_target_mfd(cam, ships, target_ship_id);
    draw_radar_mfd (cam, system, selected_nav, ships, target_ship_id);
}

// ---- big navmap overlay --------------------------------------------------
//
// Centered fullscreen-ish window with a top-down projection of the
// system. World-space (X, Z) -> map (X, Y), auto-scaled so all nav
// points fit with margin. Clickable nav-point dots; the player position
// gets a triangle marker; ship contacts (player perception) are
// stance-coloured pips overlaid. Closes on the X button or ESC.
//
// World-up (cam.up) ignored — this is a system-overhead map, not a
// 3D viewport. Future enhancement: a second small panel showing
// vertical (X, Y) projection so altitude is also legible.
void build_navmap(const Camera& cam, const StarSystem& system,
                  int& selected_nav_in_out,
                  const std::vector<Ship>& ships,
                  bool& shown_in_out) {
    if (!shown_in_out) return;

    const auto sz = screen_size();
    const float w = std::min(sz.w * 0.85f, 1100.0f);
    const float h = std::min(sz.h * 0.85f,  800.0f);
    ImGui::SetNextWindowPos(ImVec2((sz.w - w) * 0.5f, (sz.h - h) * 0.5f),
                            ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.92f);

    push_hud_style();
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse
                           | ImGuiWindowFlags_NoResize
                           | ImGuiWindowFlags_NoSavedSettings;
    bool open = true;
    if (ImGui::Begin("NAVIGATION MAP   (Alt+N to close)", &open, flags)) {

        // Compute bounding box of all nav points + camera so the auto-
        // scale frames everything visible. Adds the camera so the
        // player marker is always inside the box.
        float min_x = cam.position.X, max_x = cam.position.X;
        float min_z = cam.position.Z, max_z = cam.position.Z;
        for (const auto& nv : system.nav_points) {
            if (nv.position.X < min_x) min_x = nv.position.X;
            if (nv.position.X > max_x) max_x = nv.position.X;
            if (nv.position.Z < min_z) min_z = nv.position.Z;
            if (nv.position.Z > max_z) max_z = nv.position.Z;
        }
        const float span_x = std::max(1.0f, max_x - min_x);
        const float span_z = std::max(1.0f, max_z - min_z);
        // Equal-aspect scaling — pick the smaller of the two so both
        // axes fit. 90% margin so dots don't sit on the panel edge.
        const ImVec2 area_p0 = ImGui::GetCursorScreenPos();
        const ImVec2 area_sz = ImGui::GetContentRegionAvail();
        const float  scale = 0.92f * std::min(area_sz.x / span_x,
                                              area_sz.y / span_z);
        // Center of the data in world space; we'll project so it
        // lands at the center of the map area.
        const float cx_w = 0.5f * (min_x + max_x);
        const float cz_w = 0.5f * (min_z + max_z);
        const ImVec2 ctr = ImVec2(area_p0.x + area_sz.x * 0.5f,
                                  area_p0.y + area_sz.y * 0.5f);
        auto to_screen = [&](float wx, float wz) {
            return ImVec2(ctr.x + (wx - cx_w) * scale,
                          ctr.y + (wz - cz_w) * scale);
        };

        ImDrawList* dl = ImGui::GetWindowDrawList();

        // Background panel (the window bg already fills it; this is
        // the actual map area outline).
        dl->AddRect(area_p0,
                    ImVec2(area_p0.x + area_sz.x, area_p0.y + area_sz.y),
                    IM_COL32(80, 100, 120, 200), 0.0f, 0, 1.0f);

        // Ship contacts under nav points so navs don't get hidden by
        // densely packed ship dots.
        if (!ships.empty() && ships.front().is_player) {
            const Ship& player = ships.front();
            for (const PerceivedContact& c : player.perception.visible) {
                const HMM_Vec3 p = HMM_AddV3(
                    player.position, HMM_MulV3F(c.to_unit, c.distance_m));
                const ImVec2 sp = to_screen(p.X, p.Z);
                const ImU32 col =
                    (c.stance == Stance::Hostile) ? IM_COL32(255,  90,  90, 230)
                  : (c.stance == Stance::Allied)  ? IM_COL32( 90, 255, 110, 230)
                  :                                  IM_COL32(255, 220,  60, 230);
                dl->AddCircleFilled(sp, 3.0f, col, 8);
            }
        }

        // Nav points — clickable. Hit-test in screen space against a
        // generous radius so dots are easy to click. Selecting a nav
        // mirrors the N-key cycle's effect (sets selected_nav).
        const ImVec2 mouse = ImGui::GetMousePos();
        const bool mouse_in_panel =
            mouse.x >= area_p0.x && mouse.x <= area_p0.x + area_sz.x &&
            mouse.y >= area_p0.y && mouse.y <= area_p0.y + area_sz.y;
        const bool clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left)
                          && mouse_in_panel;
        for (int i = 0; i < (int)system.nav_points.size(); ++i) {
            const auto& nv = system.nav_points[i];
            const ImVec2 sp = to_screen(nv.position.X, nv.position.Z);
            const ImU32  col = color_for_kind(nv.kind);
            const bool   sel = (i == selected_nav_in_out);
            const float  r   = sel ? 8.0f : 5.0f;
            dl->AddCircleFilled(sp, r, col, 16);
            if (sel) dl->AddCircle(sp, r + 3.0f, kAmber, 0, 1.5f);
            // Label.
            dl->AddText(ImVec2(sp.x + r + 4.0f, sp.y - 7.0f),
                        kHudWhite, nv.name.c_str());
            // Click hit-test.
            if (clicked) {
                const float dxs = mouse.x - sp.x;
                const float dys = mouse.y - sp.y;
                if (dxs * dxs + dys * dys < (r + 8.0f) * (r + 8.0f)) {
                    selected_nav_in_out = i;
                }
            }
        }

        // Player marker — small triangle at camera position, pointing
        // along camera-forward projected onto the XZ plane.
        const ImVec2 pp = to_screen(cam.position.X, cam.position.Z);
        const HMM_Vec3 cf = cam.forward();
        const float fx = cf.X, fz = cf.Z;
        const float fl = std::sqrt(fx * fx + fz * fz);
        const float ux = (fl > 1e-3f) ? (fx / fl) : 0.0f;
        const float uz = (fl > 1e-3f) ? (fz / fl) : 1.0f;
        constexpr float kSize = 9.0f;
        const ImVec2 tip { pp.x + ux * kSize,           pp.y + uz * kSize };
        const ImVec2 bl  { pp.x - ux * kSize * 0.4f - uz * kSize * 0.6f,
                            pp.y - uz * kSize * 0.4f + ux * kSize * 0.6f };
        const ImVec2 br  { pp.x - ux * kSize * 0.4f + uz * kSize * 0.6f,
                            pp.y - uz * kSize * 0.4f - ux * kSize * 0.6f };
        dl->AddTriangleFilled(tip, bl, br, kHudWhite);

        // Footer help.
        ImGui::SetCursorScreenPos(ImVec2(area_p0.x, area_p0.y + area_sz.y + 4.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, kDimAmber);
        ImGui::TextUnformatted("Click a nav point to select it.  Alt+N to close.");
        ImGui::PopStyleColor();
    }
    ImGui::End();
    pop_hud_style();

    // ESC closes too. Title-bar X also flips `open` to false.
    if (!open || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        shown_in_out = false;
    }
}

} // namespace cockpit_hud

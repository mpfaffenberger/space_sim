// cockpit_hud.h — modern flight-sim HUD overlay (ship-agnostic).
//
// Per-frame ImGui draws for the in-flight HUD:
//
//   * gun crosshair          — small static '+' at screen centre,
//                              indicates where weapons will fire
//   * nav target reticle     — amber circle that floats over the
//                              currently-selected waypoint, with
//                              edge-clamp + chevron when off-screen
//   * target MFD             — bottom-right ImGui panel showing
//                              TARGET / DIST / AZ-EL data block
//   * radar MFD              — bottom-left top-down radar disc with
//                              colour-coded nav dots and sweep line
//
// All draws happen via ImGui (drawlists for primitives, regular
// windows for MFD frames) so they composite naturally with the
// debug_panel + sprite_light_editor widgets that share the same
// frame. Call cockpit_hud::build() once per frame, between
// simgui_new_frame() and simgui_render().
//
// The HUD is intentionally ship-agnostic. Per-ship cosmetic 'cockpit
// rails' (decorative side/bottom hull sprites that vary by hull) are
// a separate future system that will composite UNDER these widgets.
#pragma once

struct Camera;
struct StarSystem;

namespace cockpit_hud {

// One call, one frame.
//
//   selected_nav   index into system.nav_points; -1 = no target
//                  (reticle hidden, target MFD shows placeholder).
//   mouse_x/y      logical-pixel mouse position; drives the aim cursor.
//   fly_by_wire    true = aim cursor drawn (player is flying with
//                  mouse). false = cursor mode (OS cursor visible,
//                  ship doesn't turn). We HIDE the in-game aim
//                  reticle in cursor mode so we don't double-draw.
void build(const Camera& cam, const StarSystem& system, int selected_nav,
           float mouse_x, float mouse_y, bool fly_by_wire);

} // namespace cockpit_hud

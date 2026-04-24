#pragma once
// -----------------------------------------------------------------------------
// debug_panel.h — Dear ImGui-based live-tweak UI for the ship showcase.
//
// Keeps all the ImGui wiring (sokol_imgui init/frame/render) in one place
// so main.cpp doesn't have to care about the overlay's lifecycle, just
// when to kick off a frame and when to hand off back to the scene.
//
// Philosophy: the panel *shouldn't exist* in a shipped game. It's a dev
// affordance — a slider-fest to figure out what numbers look right. Once
// Mike's happy with a set of values he transcribes them into the system
// JSON and we can #ifdef the whole thing out for a release build later.
// -----------------------------------------------------------------------------

#include "sokol_app.h"

struct PlacedMesh;
struct StarSystem;
#include <vector>

namespace debug_panel {

// Lifecycle — call exactly once each.
void init();
void shutdown();

// Input forwarding. Call from sapp's event_cb; returns true if ImGui ate
// the event (so the scene's camera / input handler should ignore it).
bool handle_event(const sapp_event* e);

// Two-phase per-frame flow so the panel can live inside the existing
// composite swapchain pass instead of needing its own. Doing two
// swapchain passes per frame causes drawable-reacquisition flicker on
// Metal — unforgivable for a debug tool that's supposed to help us see.
//
//   build()   — call once per frame BEFORE any sg_begin_pass. Starts
//               the ImGui frame and populates widgets + reads back
//               slider mutations into the live ship list.
//
//   render()  — call once per frame INSIDE the existing swapchain pass.
//               Issues the actual draw calls.
void build(std::vector<PlacedMesh>& placed_meshes);
void render();

} // namespace debug_panel

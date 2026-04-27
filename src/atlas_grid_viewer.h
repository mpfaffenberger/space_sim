#pragma once
// -----------------------------------------------------------------------------
// atlas_grid_viewer.h — F4-toggled ImGui tool to inspect every cell in a
// ship sprite atlas at once and live-mutate per-cell properties.
//
// Workflow:
//   1. Press F4. A grid window appears showing all 82 cells of the
//      currently-selected ship atlas, laid out by (elevation row, azimuth
//      column). Pole cells (el = ±90°) get their own single-entry rows.
//   2. Click a cell → select. Click another → SWAP their (az, el) labels
//      (the PNGs themselves stay in place; we mutate the runtime
//      ShipSpriteFrame.az_deg / .el_deg / .dir so cell-selection picks
//      the new label for that view direction). Click the same cell again
//      to deselect.
//   3. The right-side inspector shows a 256-px preview of the selected
//      cell plus per-cell `scale` and `roll_deg` sliders, both live —
//      changes take effect on the next render frame.
//   4. "Save tuning" persists scale + roll_deg to atlas_manifest.tuning.
//      json (matches the existing tuning-file format from debug_panel).
//   5. "Copy swap-persistence script" emits a self-contained Python
//      snippet to the clipboard that, when pasted into a shell, rewrites
//      atlas_manifest.json with the current runtime (az, el) labels for
//      every sample. That's how swaps survive an engine restart.
//
// Why this lives in its own module rather than as another section of
// debug_panel.cpp:
//   - The grid view needs ~80 ImageButton widgets per frame; keeping the
//     code beside the unrelated mesh-tweak sliders would bloat one file
//     for no benefit. Separation keeps each tool's surface area legible.
//   - The F2 sprite_light_editor already established the "extra ImGui
//     panel that doesn't own the simgui backend" pattern. We're just
//     following its lead.
//
// Lifecycle ordering relative to peers (see main.cpp):
//   handle_event:  before debug_panel (so F4 wins over imgui focus)
//   build:         AFTER debug_panel::build (which calls simgui_new_frame)
//                  but before simgui_render — same window of opportunity
//                  as sprite_light_editor::build
// -----------------------------------------------------------------------------

#include "sokol_app.h"

#include <string>
#include <unordered_map>

struct ShipSpriteAtlas;

namespace atlas_grid_viewer {

// One-time setup. No allocations beyond the visibility flag; the heavy
// state (selected atlas key, selected frame index) is module-static.
void init();

// Symmetric for cleanup parity with the other panels — currently a no-op.
void shutdown();

// Must run BEFORE debug_panel::handle_event so the F4 toggle works
// regardless of whether ImGui has keyboard focus. Returns true if the
// event was consumed (so main.cpp's input router stops here).
bool handle_event(const sapp_event* e);

// Per-frame UI build. Call once between simgui_new_frame() (issued by
// debug_panel::build) and the final swapchain pass that flushes ImGui
// draw calls. Safe to call when hidden — early-returns without drawing.
//
// Takes the live atlas map by reference because all mutations (label
// swaps, scale/roll edits) write directly into ShipSpriteFrame fields,
// which the existing cell-selection / billboard pipeline reads next
// frame. No copy, no apply step — instant feedback in the scene.
void build(std::unordered_map<std::string, ShipSpriteAtlas>& atlases);

} // namespace atlas_grid_viewer

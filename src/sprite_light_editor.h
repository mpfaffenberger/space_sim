#pragma once
// -----------------------------------------------------------------------------
// sprite_light_editor.h — in-engine ImGui tool to click-author animated
// lights on sprite billboards.
//
// Workflow: press F2. An ImGui window opens showing the currently-loaded
// sprite at 1:1 with small circle gizmos over each existing LightSpot.
// Click on empty image area to add a light. Click on a gizmo to select.
// Use the right-side panel to tweak the selected light's color / size /
// kind / hz / phase. "Save" writes <sprite_base>.lights.json next to the
// sprite PNG so the changes survive a restart.
//
// All edits are LIVE: the in-scene sprite uses the same std::vector<LightSpot>
// we're editing, so Mike sees his lights blink in-scene as he authors them
// (great for positioning relative to actual hull features).
//
// No new render path — purely a data-entry UI. The existing sprite renderer
// already knows how to draw LightSpots via np-0kv.3's sprite_spot pipeline.
// -----------------------------------------------------------------------------

#include "sokol_app.h"
#include "sprite.h"

#include <vector>

namespace sprite_light_editor {

// One-time setup. No heavy allocations — just zeros the persistent editor
// state. Safe to call after debug_panel::init() since it just adds a
// window to the same ImGui context.
void init();

// Must be called BEFORE build() and BEFORE debug_panel::handle_event() so
// the F2 toggle works regardless of ImGui's focus state. Returns true if
// the event was consumed (so main.cpp's input handler should ignore it).
bool handle_event(const sapp_event* e);

// Per-frame UI build. Call once between simgui_new_frame() and the final
// swapchain pass. Takes the live SpriteObject list — mutates it in place
// as the user clicks/drags/deletes. Safe to call when editor is hidden
// (it just returns without drawing).
void build(std::vector<SpriteObject>& sprites);

// Load <sprite_base>.lights.json and REPLACE the sprite's `lights` vector.
// Called once per sprite at startup. Silent no-op if the sidecar doesn't
// exist yet — sprites can ship without any lights authored.
bool load_lights_sidecar(const std::string& sprite_base_path,
                         std::vector<LightSpot>& out);

// Serialise the given lights to <sprite_base>.lights.json. Invoked from
// the editor's Save button. Overwrites any existing file.
bool save_lights_sidecar(const std::string& sprite_base_path,
                         const std::vector<LightSpot>& lights);

} // namespace sprite_light_editor

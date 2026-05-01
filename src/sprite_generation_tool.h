#pragma once
// -----------------------------------------------------------------------------
// sprite_generation_tool.h — F6 ImGui workbench for one-off sprite generation.
//
// This panel is intentionally just a front-end. It writes a JSON request and
// launches tools/generate_ship_sprite_job.py in a background thread. The engine
// stays free of API keys, HTTP clients, and model-specific goblin rituals.
// -----------------------------------------------------------------------------

#include "sokol_app.h"

namespace sprite_generation_tool {

void init();
void shutdown();
bool handle_event(const sapp_event* e);
void build();

} // namespace sprite_generation_tool

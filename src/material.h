#pragma once
// -----------------------------------------------------------------------------
// material.h — per-submesh texture bundle.
//
// A Material groups the four texture slots a submesh can reference:
//   * diffuse  — albedo / base colour
//   * spec     — specular intensity (.r channel used)
//   * glow     — emissive RGB (engines, running lights)
//   * normal   — tangent-space normal map (consumed in Phase C — stored now
//                so the sidecar JSON and loader stay format-stable)
//
// Any slot may be empty (zeroed `sg_image`). The mesh renderer substitutes
// a shared 1×1 neutral fallback at bind time so every draw has the same
// binding topology — no shader permutations for "mesh has no glow".
//
// Alpha blending is a material property (the WCU glass domes carry
// `blend="ONE INVSRCALPHA"`). Recorded here; rendered support comes
// when the transparent pipeline path lands.
// -----------------------------------------------------------------------------

#include "sokol_gfx.h"
#include <string>

struct TextureSlot {
    sg_image image{};
    sg_view  view{};
    bool     valid = false;
};

struct Material {
    std::string  name;             // e.g. "tex0_0" — diagnostic only
    TextureSlot  diffuse;
    TextureSlot  spec;
    TextureSlot  glow;
    TextureSlot  normal;           // reserved for Phase C — loaded but unused
    bool         alpha_blend = false;

    void destroy();                // releases any non-empty slots
};

// Load PNG at `path` into `slot`. Idempotent — returns false and leaves
// `slot` untouched if the file is missing or can't be decoded. Callers
// are expected to try multiple candidate paths (fallback chain).
bool load_texture_png(const std::string& path, TextureSlot& slot);

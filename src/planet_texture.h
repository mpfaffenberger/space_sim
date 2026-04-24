#pragma once
// -----------------------------------------------------------------------------
// planet_texture.h — runtime procedural planet surface textures.
//
// Generates an equirectangular (2:1) RGBA texture in-memory using cheap
// fractal value noise + a preset palette. No disk I/O, no external
// dependencies. Called at startup and handed to the mesh renderer as the
// diffuse map for a planet-sized placed mesh.
//
// Keeping this procedural instead of shipping baked PNGs means:
//   - Zero asset files, zero licensing anxiety
//   - Trivial to add new planet types (tweak palette + noise params)
//   - Every run has the same result (deterministic hash-based noise)
// -----------------------------------------------------------------------------

#include "sokol_gfx.h"

#include <cstdint>
#include <string>

struct PlanetTexture {
    sg_image image{};
    sg_view  view{};
    int      width  = 0;
    int      height = 0;

    void destroy();
};

// Generate a planet texture for a named preset. Unknown presets fall
// back to \"agricultural\" rather than returning an error — a wrong
// colour is less surprising than a disappearing planet.
//
// Supported presets (as of now):
//   \"agricultural\"  — green continents, blue oceans, white polar caps
// Default 4096×2048. Each doubling is 4× the pixel work + 4× the VRAM,
// but a single planet at 32 MB is nothing for a modern GPU and the
// quality jump at close approach is dramatic. Mipmaps are generated
// automatically so distant views stay alias-free without extra work.
PlanetTexture make_planet_texture(const std::string& preset,
                                  int w = 4096, int h = 2048);

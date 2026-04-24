#pragma once
// -----------------------------------------------------------------------------
// asteroid.h — procedural asteroid field.
//
// Generates a handful of distinct "rock mesh variants" (subdivided
// icosahedrons displaced by 3D value-noise, normals re-averaged) and
// scatters N instances of each across a slab-shaped volume. Each instance
// carries position, scale, rotation axis, angular velocity, and color
// tint — all randomised at init and kept static on the GPU forever. Time
// is uniform, so the shader tumbles them for free.
//
// Why variants instead of ONE mesh? With rotation + scale + tint, a single
// rock shape already reads as varied; but at close range the eye picks up
// repeated silhouettes. A handful of variants (say 4) fully defeats this
// for a trivial cost: ~20KB of GPU mesh data and one extra draw call per
// variant.
// -----------------------------------------------------------------------------

#include "HandmadeMath.h"
#include "mesh.h"
#include "sokol_gfx.h"

#include <cstdint>

struct Camera;

struct AsteroidField {
    static constexpr int kVariantCount = 4;

    // ---- configuration (set before init) --------------------------------
    HMM_Vec3 center       = { 3000.0f, -200.0f, 24000.0f };
    HMM_Vec3 half_extent  = { 4000.0f,  800.0f,  4000.0f }; // slab
    float    base_radius  = 45.0f;
    float    size_min     = 0.4f;   // fraction of base_radius
    float    size_max     = 2.6f;
    int      total_count  = 300;
    uint32_t seed         = 0xA57E01DU;

    // ---- GPU resources --------------------------------------------------
    Mesh        variant_mesh[kVariantCount]{};
    sg_buffer   instance_buf[kVariantCount]{};
    int         instance_count[kVariantCount]{};

    sg_shader   shader{};
    sg_pipeline pipeline{};

    bool init();
    void destroy();

    // Draw all variants with one instanced call each. `time_sec` drives
    // tumble animation. `sun_pos` + `sun_color` feed directional lighting.
    // `rim_tint` colors the grazing-angle rim — typically set to the
    // dominant nebula hue so rocks feel lit by the surrounding gas.
    void draw(const Camera& cam, float aspect, float time_sec,
              HMM_Vec3 sun_pos, HMM_Vec3 sun_color,
              HMM_Vec3 rim_tint = {0.45f, 0.25f, 0.55f},
              float rim_strength = 0.55f) const;
};

#pragma once
// -----------------------------------------------------------------------------
// mesh_render.h — draws arbitrary loaded meshes (ships, stations, placeables).
//
// Non-instanced: each PlacedMesh gets its own transform + material and one
// draw call. For a dozen ships in a system this is plenty; if we ever want
// hundreds of the same ship model, we'll add an instanced path alongside.
//
// The MeshRenderer owns the shader + pipeline (shared across all placed
// meshes). Each PlacedMesh owns its own `Mesh` (GPU vertex/index buffers)
// plus a transform and tint. The renderer is a thin draw coordinator.
// -----------------------------------------------------------------------------

#include "HandmadeMath.h"
#include "mesh.h"
#include "sokol_gfx.h"

#include <string>
#include <vector>

struct Camera;

struct PlacedMesh {
    std::string name;        // diagnostic
    Mesh        mesh;
    HMM_Vec3    position     = { 0, 0, 0 };
    HMM_Vec3    euler_deg    = { 0, 0, 0 };   // pitch / yaw / roll
    float       scale        = 1.0f;
    HMM_Vec3    body_tint    = { 1.00f, 1.00f, 1.00f };  // multiplies the texture
    float       spec_amount  = 0.35f;

    // Textures live on `mesh.materials` (one bundle per submesh). Missing
    // slots get substituted at bind time with shared 1×1 neutral fallbacks
    // owned by the MeshRenderer (white for diffuse, gray for spec, black
    // for glow). Nothing lives on PlacedMesh anymore — two PlacedMeshes
    // sharing the same Mesh asset would also share textures, for free
    // (if/when we add mesh sharing).

    // If true, the renderer binds the no-cull pipeline for this mesh.
    // Necessary for imported WCU geometry where the artist wasn't
    // strict about triangle winding — backface culling would eat
    // random surfaces and leave gaping holes. Clean meshes that we
    // generate ourselves (e.g. the icosphere planet) set this false
    // so z-fighting on closed shapes doesn't create "see-through-me"
    // artifacts.
    bool        double_sided = true;

    // Lighting overrides — `< 0` means "use the global default supplied
    // to MeshRenderer::draw". Positive values set the floor/strength
    // per-mesh so a planet can go properly dark on its night side (low
    // ambient_floor) and skip the rim term (which would otherwise fight
    // the atmospheric halo at the silhouette).
    float       ambient_floor = -1.0f;   // < 0 = use global default
    float       rim_strength  = -1.0f;   // < 0 = use global default

    // Optional atmospheric halo. When `atm_thickness > 0` the renderer
    // does a second pass on this mesh, scaled up by (1 + thickness),
    // additive-blended, with a fresnel shader. Ideal for planets.
    // Zero (default) skips the pass entirely — no cost for non-planets.
    float       atm_thickness = 0.0f;          // fraction, e.g. 0.05 = +5%
    HMM_Vec3    atm_color     = { 0.55f, 0.75f, 1.0f };
    float       atm_strength  = 1.0f;
};

struct MeshRenderer {
    // --- Main mesh pipeline (solid / lit draws) -------------------------
    sg_shader   shader{};
    // Two pipelines sharing the same shader + layout, differing only in
    // `cull_mode`. Picked per-mesh at draw time based on PlacedMesh.
    // `double_sided`. Avoids shader permutations / state churn.
    sg_pipeline pipeline_cull_back{};     // clean meshes (our own)
    sg_pipeline pipeline_two_sided{};     // imported meshes (WCU ships)

    // --- Atmosphere halo pipeline ---------------------------------------
    // Draws a slightly-enlarged shell with additive fresnel for planets.
    // Separate shader so the main mesh path stays lean. Same vertex layout.
    sg_shader   atm_shader{};
    sg_pipeline atm_pipeline{};

    sg_sampler  sampler{};        // linear, repeat
    // 1×1 neutral fallbacks — bound when a mesh's corresponding slot
    // is empty so every draw has identical binding topology.
    sg_image    white_1x1{};      // diffuse fallback (1,1,1)
    sg_view     white_1x1_view{};
    sg_image    gray_1x1{};       // spec fallback    (0.5 — moderate shine)
    sg_view     gray_1x1_view{};
    sg_image    black_1x1{};      // glow fallback    (0,0,0 — no emission)
    sg_view     black_1x1_view{};
    sg_image    flat_normal_1x1{};// normal fallback  (128,128,255) = flat tangent-space
    sg_view     flat_normal_1x1_view{};

    bool init();
    void destroy();

    // Draw every PlacedMesh with one call apiece. `sun_pos` / `sun_color`
    // and `rim_tint` plumb the same lighting context we feed asteroids.
    void draw(const std::vector<PlacedMesh>& meshes,
              const Camera& cam, float aspect,
              HMM_Vec3 sun_pos, HMM_Vec3 sun_color,
              HMM_Vec3 rim_tint = { 0.45f, 0.25f, 0.55f },
              float rim_strength = 0.55f) const;
};

// -----------------------------------------------------------------------------
// mesh_render_atmosphere.cpp — planetary atmosphere halo pass.
//
// Owns the atmosphere pipeline + shader and the second-pass draw loop
// for PlacedMesh entries with `atm_thickness > 0`. Lives in its own TU
// because sokol-shdc's generated headers declare `vs_source_metal_macos`
// as a static const array; including two such headers in one TU breaks
// the link. See src/sun.cpp vs src/sun_corona.cpp for the same pattern.
// -----------------------------------------------------------------------------

#include "mesh.h"
#include "mesh_render.h"
#include "render_config.h"

#include "generated/atmosphere.glsl.h"

#include <cstdio>
#include <cstring>
#include <vector>

// Declared in mesh_render.cpp and called from there. Free functions so
// the atmosphere module can't accidentally touch MeshRenderer internals
// that aren't already public on the struct.
bool init_mesh_atmosphere_pipeline(MeshRenderer& m) {
    m.atm_shader = sg_make_shader(atmosphere_shader_desc(sg_query_backend()));

    sg_pipeline_desc ap{};
    ap.shader = m.atm_shader;
    ap.layout.buffers[0].stride = sizeof(MeshVertex);
    ap.layout.attrs[ATTR_atmosphere_a_pos].format    = SG_VERTEXFORMAT_FLOAT3;
    ap.layout.attrs[ATTR_atmosphere_a_pos].offset    = 0;
    ap.layout.attrs[ATTR_atmosphere_a_normal].format = SG_VERTEXFORMAT_FLOAT3;
    ap.layout.attrs[ATTR_atmosphere_a_normal].offset = 12;
    ap.layout.attrs[ATTR_atmosphere_a_uv].format     = SG_VERTEXFORMAT_FLOAT2;
    ap.layout.attrs[ATTR_atmosphere_a_uv].offset     = 24;

    ap.index_type = SG_INDEXTYPE_UINT16;

    // Cull BACK faces → render the *front* of the shell. Because the
    // shell is scaled up relative to the planet, its front faces sit
    // between the camera and the planet surface at every pixel where
    // the planet is visible. That means the atmosphere contributes
    // additive colour *on top of* the planet (thin haze, limb
    // brightening, sunset glow near the terminator) as well as
    // outside the silhouette (halo against space). Cull-front would
    // have restricted the effect to the annular halo ring only —
    // fine for a thin rim, no good for a thick scattering atmosphere.
    ap.cull_mode  = SG_CULLMODE_BACK;

    // Read depth (so objects in front of the atmosphere properly occlude
    // it) but don't write depth — the halo isn't a solid obstruction.
    ap.depth.compare       = SG_COMPAREFUNC_LESS_EQUAL;
    ap.depth.write_enabled = false;

    // Premultiplied-alpha blending: src + dst × (1 - src.a).
    //
    // Pure additive only adds light, which is great for a thin rim
    // halo but can't *obscure* the surface — the planet stays
    // perfectly crisp under arbitrarily bright additive fog. A real
    // thick atmosphere both scatters light toward the camera AND
    // blocks some of the direct surface light, hazing details out.
    //
    // With premultiplied alpha, the shader outputs `rgb = emission`
    // (already multiplied by alpha = density) and `a = coverage`.
    // Destination is attenuated by (1 - coverage), so foggy regions
    // replace the surface with the atmosphere's scattering colour.
    // Non-foggy regions (alpha=0) are a no-op.
    ap.colors[0].pixel_format = kSceneColorFormat;
    ap.colors[0].blend.enabled          = true;
    ap.colors[0].blend.src_factor_rgb   = SG_BLENDFACTOR_ONE;
    ap.colors[0].blend.dst_factor_rgb   = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    ap.colors[0].blend.src_factor_alpha = SG_BLENDFACTOR_ONE;
    ap.colors[0].blend.dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;

    ap.depth.pixel_format = kSceneDepthFormat;
    ap.sample_count       = kSceneSampleCount;

    m.atm_pipeline = sg_make_pipeline(&ap);
    if (sg_query_pipeline_state(m.atm_pipeline) != SG_RESOURCESTATE_VALID) {
        std::fprintf(stderr, "[mesh_render] atmosphere pipeline failed\n");
        return false;
    }
    return true;
}

void destroy_mesh_atmosphere_pipeline(MeshRenderer& m) {
    sg_destroy_pipeline(m.atm_pipeline);
    sg_destroy_shader(m.atm_shader);
}

// Defined in mesh_render.cpp for the solid pass — redeclare here so we
// can compose the model matrix the same way without pulling in every
// internal of the mesh renderer.
HMM_Mat4 model_matrix(HMM_Vec3 position, HMM_Vec3 euler_deg, float scale);

void draw_mesh_atmosphere_pass(const MeshRenderer& m,
                               const std::vector<PlacedMesh>& meshes,
                               const HMM_Mat4& view_proj,
                               HMM_Vec3 sun_pos,
                               HMM_Vec3 camera_pos) {
    bool bound = false;
    for (const auto& pm : meshes) {
        if (pm.atm_thickness <= 0.0f) continue;
        if (!bound) {
            sg_apply_pipeline(m.atm_pipeline);
            bound = true;
        }

        const HMM_Mat4 M = model_matrix(pm.position, pm.euler_deg,
                                        pm.scale * (1.0f + pm.atm_thickness));

        atm_vs_params_t vsp{};
        std::memcpy(vsp.view_proj, &view_proj, sizeof(float) * 16);
        std::memcpy(vsp.model,     &M,         sizeof(float) * 16);

        atm_fs_params_t fsp{};
        fsp.sun_pos[0]    = sun_pos.X; fsp.sun_pos[1] = sun_pos.Y; fsp.sun_pos[2] = sun_pos.Z;
        fsp.sun_pos[3]    = 0.0f;
        fsp.camera_pos[0] = camera_pos.X;
        fsp.camera_pos[1] = camera_pos.Y;
        fsp.camera_pos[2] = camera_pos.Z;
        fsp.camera_pos[3] = 0.0f;
        fsp.atm_color[0]  = pm.atm_color.X;
        fsp.atm_color[1]  = pm.atm_color.Y;
        fsp.atm_color[2]  = pm.atm_color.Z;
        fsp.atm_color[3]  = pm.atm_strength;

        sg_bindings b{};
        b.vertex_buffers[0] = pm.mesh.vbuf;
        b.index_buffer      = pm.mesh.ibuf;
        sg_apply_bindings(&b);
        sg_apply_uniforms(UB_atm_vs_params, SG_RANGE(vsp));
        sg_apply_uniforms(UB_atm_fs_params, SG_RANGE(fsp));
        sg_draw(0, pm.mesh.index_count, 1);
    }
}

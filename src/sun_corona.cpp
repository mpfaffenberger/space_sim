// -----------------------------------------------------------------------------
// sun_corona.cpp — implementation split off from sun.cpp so we can include
// a second shader header (sun_glow.glsl.h) without colliding with sun.glsl.h.
//
// sokol-shdc emits each shader's Metal source as a static-linkage blob named
// by shader stage (`vs_source_metal_macos`, `fs_source_metal_macos`). When
// two such headers end up in the same translation unit, the blobs collide.
// Keeping one header per .cpp is simpler than fighting the naming convention.
//
// The corona state still lives inside `Sun` (see sun.h); these two free
// functions read/write that state to set up and draw the billboard pass.
// -----------------------------------------------------------------------------

#include "sun.h"
#include "render_config.h"

#include "generated/sun_glow.glsl.h"

#include <cstdio>
#include <cstring>

bool sun_corona_init(Sun& s) {
    // Four corners of a unit quad in [-1,+1]^2 — drawn as a triangle strip.
    const float corners[] = {
        -1.0f, -1.0f,
         1.0f, -1.0f,
        -1.0f,  1.0f,
         1.0f,  1.0f,
    };
    sg_buffer_desc vbd{};
    vbd.data = SG_RANGE(corners);
    s.corona_vbuf = sg_make_buffer(&vbd);

    s.corona_shader = sg_make_shader(sun_glow_shader_desc(sg_query_backend()));

    sg_pipeline_desc pd{};
    pd.shader = s.corona_shader;
    pd.layout.attrs[ATTR_sun_glow_a_corner].format = SG_VERTEXFORMAT_FLOAT2;
    pd.primitive_type = SG_PRIMITIVETYPE_TRIANGLE_STRIP;
    pd.cull_mode      = SG_CULLMODE_NONE;

    // Additive blend so the corona only ever brightens the scene.
    pd.colors[0].blend.enabled          = true;
    pd.colors[0].blend.src_factor_rgb   = SG_BLENDFACTOR_SRC_ALPHA;
    pd.colors[0].blend.dst_factor_rgb   = SG_BLENDFACTOR_ONE;
    pd.colors[0].blend.src_factor_alpha = SG_BLENDFACTOR_ONE;
    pd.colors[0].blend.dst_factor_alpha = SG_BLENDFACTOR_ONE;

    // Depth-test yes (geometry in front occludes the glow). Depth-write no
    // (later transparent passes blend cleanly over the corona).
    pd.depth.compare       = SG_COMPAREFUNC_LESS_EQUAL;
    pd.depth.write_enabled = false;
    pd.colors[0].pixel_format = kSceneColorFormat;
    pd.depth.pixel_format     = kSceneDepthFormat;
    pd.sample_count           = kSceneSampleCount;

    s.corona_pipeline = sg_make_pipeline(&pd);
    if (sg_query_pipeline_state(s.corona_pipeline) != SG_RESOURCESTATE_VALID) {
        std::fprintf(stderr, "[sun_corona] pipeline creation failed\n");
        return false;
    }
    return true;
}

void sun_corona_draw(const Sun& s, const Camera& cam, const HMM_Mat4& vp) {
    // Camera-facing billboard basis. Using live camera right/up keeps the
    // quad facing us no matter how we orbit the sun.
    const HMM_Vec3 cam_r = cam.right();
    const HMM_Vec3 cam_u = cam.up();

    sun_glow_vs_params_t vsp{};
    std::memcpy(vsp.view_proj, &vp, sizeof(float) * 16);
    vsp.sun_world_pos[0] = s.position.X;
    vsp.sun_world_pos[1] = s.position.Y;
    vsp.sun_world_pos[2] = s.position.Z;
    vsp.sun_world_pos[3] = s.radius * s.corona_radius_mult;
    vsp.cam_right[0] = cam_r.X; vsp.cam_right[1] = cam_r.Y; vsp.cam_right[2] = cam_r.Z;
    vsp.cam_right[3] = 0.0f;
    vsp.cam_up[0]    = cam_u.X; vsp.cam_up[1]    = cam_u.Y; vsp.cam_up[2]    = cam_u.Z;
    vsp.cam_up[3]    = 0.0f;

    sun_glow_fs_params_t fsp{};
    fsp.glow_color[0] = s.glow_color.X;
    fsp.glow_color[1] = s.glow_color.Y;
    fsp.glow_color[2] = s.glow_color.Z;
    fsp.glow_color[3] = s.corona_alpha;
    fsp.ray_params[0] = s.ray_count;
    fsp.ray_params[1] = s.ray_sharpness;
    fsp.ray_params[2] = s.ray_phase;
    fsp.ray_params[3] = s.ray_strength;

    sg_bindings b{};
    b.vertex_buffers[0] = s.corona_vbuf;

    sg_apply_pipeline(s.corona_pipeline);
    sg_apply_bindings(&b);
    sg_apply_uniforms(UB_sun_glow_vs_params, SG_RANGE(vsp));
    sg_apply_uniforms(UB_sun_glow_fs_params, SG_RANGE(fsp));
    sg_draw(0, 4, 1);
}

void sun_corona_destroy(Sun& s) {
    sg_destroy_pipeline(s.corona_pipeline);
    sg_destroy_shader(s.corona_shader);
    sg_destroy_buffer(s.corona_vbuf);
}

// -----------------------------------------------------------------------------
// sun_gas.cpp — noisy atmospheric haze billboard that wraps the sun.
//
// Separate TU so sun_gas.glsl.h can coexist with sun.glsl.h and
// sun_glow.glsl.h without sokol-shdc's static-symbol collisions biting us.
// Same "free function + Sun& reference" pattern as sun_corona.cpp.
// -----------------------------------------------------------------------------

#include "sun.h"
#include "render_config.h"

#include "generated/sun_gas.glsl.h"

#include <cstdio>
#include <cstring>

bool sun_gas_init(Sun& s) {
    const float corners[] = {
        -1.0f, -1.0f,
         1.0f, -1.0f,
        -1.0f,  1.0f,
         1.0f,  1.0f,
    };
    sg_buffer_desc vbd{};
    vbd.data = SG_RANGE(corners);
    s.gas_vbuf = sg_make_buffer(&vbd);

    s.gas_shader = sg_make_shader(sun_gas_shader_desc(sg_query_backend()));

    sg_pipeline_desc pd{};
    pd.shader = s.gas_shader;
    pd.layout.attrs[ATTR_sun_gas_a_corner].format = SG_VERTEXFORMAT_FLOAT2;
    pd.primitive_type = SG_PRIMITIVETYPE_TRIANGLE_STRIP;
    pd.cull_mode      = SG_CULLMODE_NONE;

    // Additive, like the corona. Gas only ever brightens — never occludes.
    pd.colors[0].blend.enabled          = true;
    pd.colors[0].blend.src_factor_rgb   = SG_BLENDFACTOR_ONE;   // pre-multiplied
    pd.colors[0].blend.dst_factor_rgb   = SG_BLENDFACTOR_ONE;
    pd.colors[0].blend.src_factor_alpha = SG_BLENDFACTOR_ONE;
    pd.colors[0].blend.dst_factor_alpha = SG_BLENDFACTOR_ONE;

    // Depth-test so anything in front of the sun occludes its haze too.
    // No depth-write so later transparent layers still compose cleanly.
    pd.depth.compare       = SG_COMPAREFUNC_LESS_EQUAL;
    pd.depth.write_enabled = false;
    pd.colors[0].pixel_format = kSceneColorFormat;
    pd.depth.pixel_format     = kSceneDepthFormat;
    pd.sample_count           = kSceneSampleCount;

    s.gas_pipeline = sg_make_pipeline(&pd);
    if (sg_query_pipeline_state(s.gas_pipeline) != SG_RESOURCESTATE_VALID) {
        std::fprintf(stderr, "[sun_gas] pipeline creation failed\n");
        return false;
    }
    return true;
}

void sun_gas_draw(const Sun& s, const Camera& cam, const HMM_Mat4& vp, float time_sec) {
    const HMM_Vec3 cam_r = cam.right();
    const HMM_Vec3 cam_u = cam.up();

    sun_gas_vs_params_t vsp{};
    std::memcpy(vsp.view_proj, &vp, sizeof(float) * 16);
    vsp.sun_world_pos[0] = s.position.X;
    vsp.sun_world_pos[1] = s.position.Y;
    vsp.sun_world_pos[2] = s.position.Z;
    vsp.sun_world_pos[3] = s.radius * s.gas_radius_mult;
    vsp.cam_right[0] = cam_r.X; vsp.cam_right[1] = cam_r.Y; vsp.cam_right[2] = cam_r.Z;
    vsp.cam_right[3] = 0.0f;
    vsp.cam_up[0]    = cam_u.X; vsp.cam_up[1]    = cam_u.Y; vsp.cam_up[2]    = cam_u.Z;
    vsp.cam_up[3]    = 0.0f;

    sun_gas_fs_params_t fsp{};
    fsp.gas_color[0] = s.glow_color.X;   // reuse glow color so halo & shell agree
    fsp.gas_color[1] = s.glow_color.Y;
    fsp.gas_color[2] = s.glow_color.Z;
    fsp.gas_color[3] = s.gas_strength;
    fsp.gas_params[0] = time_sec;
    fsp.gas_params[1] = s.gas_noise_scale;
    fsp.gas_params[2] = s.gas_ring_center;
    fsp.gas_params[3] = s.gas_ring_width;

    sg_bindings b{};
    b.vertex_buffers[0] = s.gas_vbuf;

    sg_apply_pipeline(s.gas_pipeline);
    sg_apply_bindings(&b);
    sg_apply_uniforms(UB_sun_gas_vs_params, SG_RANGE(vsp));
    sg_apply_uniforms(UB_sun_gas_fs_params, SG_RANGE(fsp));
    sg_draw(0, 4, 1);
}

void sun_gas_destroy(Sun& s) {
    sg_destroy_pipeline(s.gas_pipeline);
    sg_destroy_shader(s.gas_shader);
    sg_destroy_buffer(s.gas_vbuf);
}

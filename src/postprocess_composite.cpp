// -----------------------------------------------------------------------------
// postprocess_composite.cpp — final blit + lens-flare pass.
//
// Separate TU so post_composite.glsl.h doesn't collide with post_blur.glsl.h.
// Exposes two free functions called by postprocess.cpp:
//
//     init_composite_pipeline / destroy_composite_pipeline
//
// plus the member `PostProcess::composite_to_swapchain`.
// -----------------------------------------------------------------------------

#include "postprocess.h"
#include "rendertargets.h"
#include "render_config.h"

#include "generated/post_composite.glsl.h"

#include "sokol_glue.h"
#include "sokol_debugtext.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

bool init_composite_pipeline(PostProcess& p) {
    p.composite_shader = sg_make_shader(post_composite_shader_desc(sg_query_backend()));

    sg_pipeline_desc pd{};
    pd.shader = p.composite_shader;
    pd.primitive_type         = SG_PRIMITIVETYPE_TRIANGLES;
    pd.colors[0].pixel_format = kSwapchainColorFormat;
    pd.sample_count           = kSceneSampleCount;
    pd.depth.pixel_format     = SG_PIXELFORMAT_NONE;
    p.composite_pipeline = sg_make_pipeline(&pd);

    if (sg_query_pipeline_state(p.composite_pipeline) != SG_RESOURCESTATE_VALID) {
        std::fprintf(stderr, "[post] composite pipeline creation failed\n");
        return false;
    }
    return true;
}

void destroy_composite_pipeline(PostProcess& p) {
    sg_destroy_pipeline(p.composite_pipeline);
    sg_destroy_shader(p.composite_shader);
}

void PostProcess::composite_to_swapchain(const RenderTargets& rt,
                                         HMM_Vec3 sun_world_pos,
                                         const HMM_Mat4& view_proj,
                                         HMM_Vec3 flare_tint,
                                         int fb_w, int fb_h,
                                         const ExtraPassDraw& extra_pass_draw) const {
    // Project the sun into clip / NDC. Disable the flare when the sun is
    // behind the camera or far off-screen, so we don't get ghost images
    // sliding in from the edge of the viewport.
    HMM_Vec4 clip = HMM_MulM4V4(view_proj, HMM_V4(sun_world_pos.X, sun_world_pos.Y, sun_world_pos.Z, 1.0f));
    float ndc_x = 0.0f, ndc_y = 0.0f, flare_i = 0.0f;
    if (clip.W > 0.0f) {
        ndc_x = clip.X / clip.W;
        ndc_y = clip.Y / clip.W;
        const float edge = std::max(std::abs(ndc_x), std::abs(ndc_y));
        flare_i = std::clamp(1.0f - (edge - 0.9f) / 0.3f, 0.0f, 1.0f) * flare_strength;
    }

    sg_pass p{};
    p.swapchain = sglue_swapchain();
    p.action.colors[0].load_action = SG_LOADACTION_CLEAR;
    p.action.colors[0].clear_value = { 0, 0, 0, 1 };
    sg_begin_pass(&p);

    sg_apply_pipeline(composite_pipeline);

    sg_bindings b{};
    b.views[0]    = rt.scene_color_tex;
    b.views[1]    = rt.bloom_b_tex;
    b.samplers[0] = rt.linear_clamp;
    sg_apply_bindings(&b);

    post_composite_params_t u{};
    u.sun_ndc_and_flare[0] = ndc_x;
    u.sun_ndc_and_flare[1] = ndc_y;
    u.sun_ndc_and_flare[2] = flare_i;
    u.sun_ndc_and_flare[3] = (float)fb_w / (float)fb_h;
    u.tint_and_bloom[0] = flare_tint.X;
    u.tint_and_bloom[1] = flare_tint.Y;
    u.tint_and_bloom[2] = flare_tint.Z;
    u.tint_and_bloom[3] = bloom_strength;
    sg_apply_uniforms(UB_post_composite_params, SG_RANGE(u));

    sg_draw(0, 3, 1);
    // HUD and any caller-supplied draws share this same swapchain pass —
    // an extra begin/end pair would cost us a second drawable acquire
    // and Metal punishes that with flicker.
    sdtx_draw();
    if (extra_pass_draw) extra_pass_draw();
    sg_end_pass();
}

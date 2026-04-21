// -----------------------------------------------------------------------------
// postprocess.cpp — owns the blur/bloom pipeline and runs the two-pass
// ping-pong into RenderTargets::bloom_b. Composite lives in a separate TU
// (postprocess_composite.cpp) so its shader header doesn't collide with
// post_blur.glsl.h via sokol-shdc's static source blobs.
// -----------------------------------------------------------------------------

#include "postprocess.h"
#include "rendertargets.h"
#include "render_config.h"

#include "generated/post_blur.glsl.h"

#include <cstdio>

namespace {

void draw_fullscreen_triangle() {
    // No bindings for the vertex stage — the shader derives positions from
    // gl_VertexIndex. But we still need to re-apply bindings for the sampled
    // texture + sampler in each pass.
    sg_draw(0, 3, 1);
}

} // namespace

// Initialises ONLY the blur pipeline. Composite pipeline is owned by
// postprocess_composite.cpp via init_composite() / destroy_composite().
bool init_composite_pipeline(PostProcess& p);
void destroy_composite_pipeline(PostProcess& p);

bool PostProcess::init() {
    blur_shader = sg_make_shader(post_blur_shader_desc(sg_query_backend()));

    sg_pipeline_desc bpd{};
    bpd.shader = blur_shader;
    bpd.primitive_type         = SG_PRIMITIVETYPE_TRIANGLES;
    bpd.colors[0].pixel_format = kBloomColorFormat;
    bpd.sample_count           = kSceneSampleCount;
    bpd.depth.pixel_format     = SG_PIXELFORMAT_NONE;
    blur_pipeline = sg_make_pipeline(&bpd);

    if (sg_query_pipeline_state(blur_pipeline) != SG_RESOURCESTATE_VALID) {
        std::fprintf(stderr, "[post] blur pipeline creation failed\n");
        return false;
    }
    return init_composite_pipeline(*this);
}

void PostProcess::destroy() {
    destroy_composite_pipeline(*this);
    sg_destroy_pipeline(blur_pipeline);
    sg_destroy_shader(blur_shader);
}

void PostProcess::apply_bloom(const RenderTargets& rt) const {
    // -- Pass 1: bright-pass + horizontal blur, scene → bloom_a -----------
    {
        sg_pass p{};
        p.attachments.colors[0] = rt.bloom_a_att;
        p.action.colors[0].load_action = SG_LOADACTION_CLEAR;
        p.action.colors[0].clear_value = { 0, 0, 0, 1 };
        sg_begin_pass(&p);

        sg_apply_pipeline(blur_pipeline);

        sg_bindings b{};
        b.views[0]    = rt.scene_color_tex;
        b.samplers[0] = rt.linear_clamp;
        sg_apply_bindings(&b);

        post_blur_params_t u{};
        u.texel_and_dir[0] = 1.0f / (float)rt.w;
        u.texel_and_dir[1] = 1.0f / (float)rt.h;
        u.texel_and_dir[2] = 1.0f;                  // horizontal
        u.texel_and_dir[3] = 0.0f;
        u.blur_cfg[0] = bloom_blur_px;
        u.blur_cfg[1] = bloom_threshold;            // bright-pass on
        sg_apply_uniforms(UB_post_blur_params, SG_RANGE(u));

        draw_fullscreen_triangle();
        sg_end_pass();
    }

    // -- Pass 2: vertical blur, bloom_a → bloom_b -------------------------
    {
        sg_pass p{};
        p.attachments.colors[0] = rt.bloom_b_att;
        p.action.colors[0].load_action = SG_LOADACTION_CLEAR;
        p.action.colors[0].clear_value = { 0, 0, 0, 1 };
        sg_begin_pass(&p);

        sg_apply_pipeline(blur_pipeline);

        sg_bindings b{};
        b.views[0]    = rt.bloom_a_tex;
        b.samplers[0] = rt.linear_clamp;
        sg_apply_bindings(&b);

        post_blur_params_t u{};
        u.texel_and_dir[0] = 1.0f / (float)rt.bw;
        u.texel_and_dir[1] = 1.0f / (float)rt.bh;
        u.texel_and_dir[2] = 0.0f;
        u.texel_and_dir[3] = 1.0f;                  // vertical
        u.blur_cfg[0] = bloom_blur_px;
        u.blur_cfg[1] = 0.0f;                       // no bright-pass second time
        sg_apply_uniforms(UB_post_blur_params, SG_RANGE(u));

        draw_fullscreen_triangle();
        sg_end_pass();
    }
}

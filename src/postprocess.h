#pragma once
// -----------------------------------------------------------------------------
// postprocess.h — bloom + lens flare + final composite pipeline.
//
// Owns the pipelines & shader modules. RenderTargets supplies the actual
// GPU memory. Use:
//
//     post.init();
//     ...
//     sg_begin_pass(scene attachments);
//     // draw everything
//     sg_end_pass();
//     post.apply_bloom(rt);
//     post.composite_to_swapchain(rt, ...);
//
// Two passes for bloom (brightpass+blurH into bloom_a, then blurV into
// bloom_b), one pass to composite and draw the flare.
// -----------------------------------------------------------------------------

#include "sokol_gfx.h"
#include "HandmadeMath.h"

struct RenderTargets;

struct PostProcess {
    // Bloom/blur pipeline — used twice per frame with different uniforms.
    sg_shader   blur_shader{};
    sg_pipeline blur_pipeline{};

    // Final composite pipeline — writes to the swapchain.
    sg_shader   composite_shader{};
    sg_pipeline composite_pipeline{};

    // Tunables ------------------------------------------------------------
    // Defaults calibrated for "I can still see the scene" rather than
    // "whole screen is a nova." Crank bloom_strength to 0.9+ if you want
    // the more aggressive Freelancer-ads vibe.
    float bloom_threshold = 0.85f;  // bright-pass cutoff before blur
    float bloom_blur_px   = 2.5f;   // blur radius multiplier (quarter-res)
    float bloom_strength  = 0.45f;  // how strongly bloom adds back
    float flare_strength  = 0.7f;   // overall flare intensity

    bool init();
    void destroy();

    // Run the two bloom passes. Result lives in rt.bloom_b_color.
    void apply_bloom(const RenderTargets& rt) const;

    // Composite scene + bloom + flare to the swapchain. `sun_world_pos` and
    // `view_proj` are used to project the sun into NDC for the flare.
    void composite_to_swapchain(const RenderTargets& rt,
                                HMM_Vec3 sun_world_pos,
                                const HMM_Mat4& view_proj,
                                HMM_Vec3 flare_tint,
                                int fb_w, int fb_h) const;
};

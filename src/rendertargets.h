#pragma once
// -----------------------------------------------------------------------------
// rendertargets.h — offscreen attachments used by the post-process pipeline.
//
// sokol's newer API treats `sg_view` as the universal indirection layer:
// you create views onto images, and those views are what you bind as
// color/depth attachments in a pass, OR as texture samples in a shader.
// For each image we therefore keep two views: one for writing (as an
// attachment) and one for reading (as a texture).
//
// Resizing is out of scope for now — window is fixed at startup.
// -----------------------------------------------------------------------------

#include "sokol_gfx.h"

struct RenderTargets {
    int w = 0, h = 0;        // scene resolution
    int bw = 0, bh = 0;      // bloom resolution (typically w/2, h/2)

    // ---- scene target ---------------------------------------------------
    sg_image scene_color{};
    sg_image scene_depth{};
    sg_view  scene_color_att{};   // write (attachment)
    sg_view  scene_depth_att{};
    sg_view  scene_color_tex{};   // read (texture for post)

    // ---- bloom ping-pong (half-res, no depth) ---------------------------
    sg_image bloom_a_color{};
    sg_image bloom_b_color{};
    sg_view  bloom_a_att{}, bloom_a_tex{};
    sg_view  bloom_b_att{}, bloom_b_tex{};

    // ---- shared sampler --------------------------------------------------
    sg_sampler linear_clamp{};

    bool init(int width, int height);
    void destroy();
};

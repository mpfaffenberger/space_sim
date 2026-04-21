#include "rendertargets.h"
#include "render_config.h"

#include <cstdio>

namespace {

sg_image make_color_image(int w, int h, sg_pixel_format fmt) {
    sg_image_desc d{};
    d.usage.color_attachment = true;
    d.width        = w;
    d.height       = h;
    d.pixel_format = fmt;
    d.sample_count = kSceneSampleCount;
    return sg_make_image(&d);
}

sg_image make_depth_image(int w, int h) {
    sg_image_desc d{};
    d.usage.depth_stencil_attachment = true;
    d.width        = w;
    d.height       = h;
    d.pixel_format = kSceneDepthFormat;
    d.sample_count = kSceneSampleCount;
    return sg_make_image(&d);
}

sg_view color_attachment_view(sg_image img) {
    sg_view_desc d{};
    d.color_attachment.image = img;
    return sg_make_view(&d);
}

sg_view depth_attachment_view(sg_image img) {
    sg_view_desc d{};
    d.depth_stencil_attachment.image = img;
    return sg_make_view(&d);
}

sg_view texture_view(sg_image img) {
    sg_view_desc d{};
    d.texture.image = img;
    return sg_make_view(&d);
}

} // namespace

bool RenderTargets::init(int width, int height) {
    w  = width;  h  = height;
    // Quarter-res bloom: 16× fewer fragments than full-res for the blur
    // passes, and since bloom is low-frequency by design the visual
    // quality cost is near-zero. Biggest single perf win in the post path.
    bw = w / 4;  bh = h / 4;

    // --- scene -----------------------------------------------------------
    scene_color = make_color_image(w, h, kSceneColorFormat);
    scene_depth = make_depth_image(w, h);
    scene_color_att = color_attachment_view(scene_color);
    scene_depth_att = depth_attachment_view(scene_depth);
    scene_color_tex = texture_view(scene_color);

    // --- bloom ping-pong -------------------------------------------------
    bloom_a_color = make_color_image(bw, bh, kBloomColorFormat);
    bloom_b_color = make_color_image(bw, bh, kBloomColorFormat);
    bloom_a_att   = color_attachment_view(bloom_a_color);
    bloom_b_att   = color_attachment_view(bloom_b_color);
    bloom_a_tex   = texture_view(bloom_a_color);
    bloom_b_tex   = texture_view(bloom_b_color);

    // --- sampler ---------------------------------------------------------
    sg_sampler_desc ss{};
    ss.min_filter = SG_FILTER_LINEAR;
    ss.mag_filter = SG_FILTER_LINEAR;
    ss.wrap_u     = SG_WRAP_CLAMP_TO_EDGE;
    ss.wrap_v     = SG_WRAP_CLAMP_TO_EDGE;
    linear_clamp  = sg_make_sampler(&ss);

    const sg_view views[] = {
        scene_color_att, scene_depth_att, scene_color_tex,
        bloom_a_att, bloom_b_att, bloom_a_tex, bloom_b_tex,
    };
    for (sg_view v : views) {
        if (sg_query_view_state(v) != SG_RESOURCESTATE_VALID) {
            std::fprintf(stderr, "[rendertargets] view creation failed\n");
            return false;
        }
    }
    return true;
}

void RenderTargets::destroy() {
    sg_destroy_sampler(linear_clamp);
    sg_destroy_view(bloom_b_tex);
    sg_destroy_view(bloom_a_tex);
    sg_destroy_view(bloom_b_att);
    sg_destroy_view(bloom_a_att);
    sg_destroy_image(bloom_b_color);
    sg_destroy_image(bloom_a_color);
    sg_destroy_view(scene_color_tex);
    sg_destroy_view(scene_depth_att);
    sg_destroy_view(scene_color_att);
    sg_destroy_image(scene_depth);
    sg_destroy_image(scene_color);
}

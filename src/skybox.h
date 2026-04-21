#pragma once
// -----------------------------------------------------------------------------
// skybox.h — owns a cubemap image, a view, a pipeline, and a unit cube.
//
// One instance = one skybox. Create at init, destroy at shutdown, draw
// exactly once per frame before any depth-writing geometry.
// -----------------------------------------------------------------------------

#include "sokol_gfx.h"
#include "camera.h"

#include <string>

struct Skybox {
    sg_image    cubemap{};
    sg_view     tex_view{};   // sokol's new bindings: images route through views
    sg_sampler  sampler{};
    sg_buffer   vbuf{};
    sg_buffer   ibuf{};
    sg_shader   shader{};
    sg_pipeline pipeline{};
    int         index_count = 0;

    // Load a cubemap from <dir>/<prefix>_{right,left,top,bottom,front,back}.png
    bool init(const std::string& dir, const std::string& prefix);

    // Render the sky. `aspect` = framebuffer width/height.
    void draw(const Camera& cam, float aspect) const;

    void destroy();
};

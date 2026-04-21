#pragma once
// -----------------------------------------------------------------------------
// dust.h — infinite parallax dust field.
//
// A fixed VBO of N random points in [-1, +1]^3 gets translated + wrapped in
// the vertex shader to follow the camera, creating an illusion of an
// unbounded field without re-uploading geometry. See shaders/dust.glsl.
// -----------------------------------------------------------------------------

#include "sokol_gfx.h"
#include "camera.h"

struct DustField {
    int   count         = 15000;
    float wrap_extent   = 600.0f;  // bigger cube = longer streaks when cruising
    float point_size_px = 5.0f;    // 2px is eaten by MSAA; 5 reads as 'speck'

    sg_buffer   vbuf{};
    sg_shader   shader{};
    sg_pipeline pipeline{};

    bool init();
    void draw(const Camera& cam, float aspect) const;
    void destroy();
};

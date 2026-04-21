#include "skybox.h"
#include "render_config.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "generated/skybox.glsl.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

// Cubemap face count (sokol validates num_slices == 6 for SG_IMAGETYPE_CUBE).
constexpr int kNumFaces = 6;

// sokol's cubemap face layout in a mip-level blob, in order:
//   slice 0: +X   slice 1: -X   slice 2: +Y
//   slice 3: -Y   slice 4: +Z   slice 5: -Z
//
// The "face" a direction samples is:
//   +X → when direction is (+x, y, z) with |x| dominant and x>0 (right)
//   -X → left
//   +Y → up (top)
//   -Y → down (bottom)
//   +Z → behind the camera (camera looks -Z), so "back"
//   -Z → in front of the camera, so "front"
constexpr std::array<const char*, kNumFaces> kFaceSuffixes = {
    "right",  // +X
    "left",   // -X
    "top",    // +Y
    "bottom", // -Y
    "back",   // +Z
    "front"   // -Z
};

// Unit cube, inside-out draw. Since we set cull_mode=NONE and sample the
// cubemap with the raw vertex position, orientation of the triangles
// doesn't matter — we just need to cover the screen.
constexpr float kCubeVerts[] = {
    -1.0f, -1.0f, -1.0f,   1.0f, -1.0f, -1.0f,
     1.0f,  1.0f, -1.0f,  -1.0f,  1.0f, -1.0f,
    -1.0f, -1.0f,  1.0f,   1.0f, -1.0f,  1.0f,
     1.0f,  1.0f,  1.0f,  -1.0f,  1.0f,  1.0f,
};
constexpr uint16_t kCubeIndices[] = {
    0,2,1, 0,3,2,   4,5,6, 4,6,7,
    0,4,7, 0,7,3,   1,2,6, 1,6,5,
    0,1,5, 0,5,4,   3,7,6, 3,6,2,
};

uint8_t* load_face(const std::string& path, int& w, int& h) {
    int channels = 0;
    uint8_t* px = stbi_load(path.c_str(), &w, &h, &channels, 4);
    if (!px) {
        std::fprintf(stderr, "[skybox] load '%s' failed: %s\n",
                     path.c_str(), stbi_failure_reason());
    }
    return px;
}

// ----- skyboxgen -> Metal/D3D cubemap orientation fix-ups ----------------
//
// skyboxgen renders each face from INSIDE the cube looking outward (GL/WebGL
// convention). D3D / Metal cubemaps expect each face as if viewed from
// OUTSIDE the cube. The difference is a MIRROR, but the mirror axis depends
// on which face we're talking about:
//
//   * 4 lateral faces (+X, -X, +Z, -Z): up-vector in skyboxgen's render is
//     world +Y, same as Metal's face-local +V. So the mismatch is purely
//     horizontal — a HORIZONTAL flip (swap columns) fixes it.
//
//   * top face (+Y): up in skyboxgen is world +Z; Metal POS_Y expects -Z at
//     top. That's a VERTICAL flip (swap rows).
//
//   * bottom face (-Y): up in skyboxgen is world -Z; Metal NEG_Y expects +Z
//     at top. Also a VERTICAL flip.
//
// Net: 4 horizontal flips + 2 vertical flips, and all 6 faces agree at
// every seam.
void mirror_horizontal_rgba8(uint8_t* px, int w, int h) {
    auto rows = reinterpret_cast<uint32_t*>(px);
    for (int y = 0; y < h; ++y) {
        uint32_t* row = rows + y * w;
        for (int x = 0; x < w / 2; ++x) {
            std::swap(row[x], row[w - 1 - x]);
        }
    }
}

void mirror_vertical_rgba8(uint8_t* px, int w, int h) {
    auto rows = reinterpret_cast<uint32_t*>(px);
    for (int y = 0; y < h / 2; ++y) {
        uint32_t* top    = rows + y * w;
        uint32_t* bottom = rows + (h - 1 - y) * w;
        for (int x = 0; x < w; ++x) std::swap(top[x], bottom[x]);
    }
}

} // namespace

bool Skybox::init(const std::string& dir, const std::string& prefix) {
    // ---------------------------------------------------------------------
    // 1. Load six faces into one concatenated RGBA8 blob.
    //    sokol's new API wants the six face payloads back-to-back in a
    //    single sg_range inside mip_levels[0].
    // ---------------------------------------------------------------------
    std::array<uint8_t*, kNumFaces> face_px{};
    int face_w = 0, face_h = 0;

    for (int f = 0; f < kNumFaces; ++f) {
        const std::string path = dir + "/" + prefix + "_" + kFaceSuffixes[f] + ".png";
        int w = 0, h = 0;
        face_px[f] = load_face(path, w, h);
        if (!face_px[f]) {
            for (int g = 0; g < f; ++g) stbi_image_free(face_px[g]);
            return false;
        }
        if (f == 0) { face_w = w; face_h = h; }
        if (w != face_w || h != face_h) {
            std::fprintf(stderr, "[skybox] face size mismatch (%s=%dx%d vs %dx%d)\n",
                         kFaceSuffixes[f], w, h, face_w, face_h);
            for (int g = 0; g <= f; ++g) stbi_image_free(face_px[g]);
            return false;
        }
    }

    const size_t face_bytes = (size_t)face_w * face_h * 4;
    std::vector<uint8_t> blob(face_bytes * kNumFaces);
    for (int f = 0; f < kNumFaces; ++f) {
        // Faces 0..3 are the lateral walls (+X, -X, +Y, -Y order is a bit of
        // a trap — see the kFaceSuffixes table above). We explicitly switch
        // on the *suffix name* rather than on the index to stay robust if
        // someone reorders the table later.
        const std::string name = kFaceSuffixes[f];
        if (name == "top" || name == "bottom") {
            mirror_vertical_rgba8(face_px[f], face_w, face_h);
        } else {
            mirror_horizontal_rgba8(face_px[f], face_w, face_h);
        }
        std::memcpy(blob.data() + face_bytes * f, face_px[f], face_bytes);
        stbi_image_free(face_px[f]);
    }

    // ---------------------------------------------------------------------
    // 2. Image object.
    // ---------------------------------------------------------------------
    sg_image_desc idesc{};
    idesc.type          = SG_IMAGETYPE_CUBE;
    idesc.width         = face_w;
    idesc.height        = face_h;
    idesc.num_slices    = kNumFaces;     // cube has 6 slices
    idesc.num_mipmaps   = 1;
    idesc.pixel_format  = SG_PIXELFORMAT_RGBA8;
    idesc.data.mip_levels[0].ptr  = blob.data();
    idesc.data.mip_levels[0].size = blob.size();
    cubemap = sg_make_image(&idesc);

    if (sg_query_image_state(cubemap) != SG_RESOURCESTATE_VALID) {
        std::fprintf(stderr, "[skybox] sg_make_image failed\n");
        return false;
    }

    // ---------------------------------------------------------------------
    // 3. View onto the image — new sokol indirection between images and
    //    shader binding slots.
    // ---------------------------------------------------------------------
    sg_view_desc vdesc{};
    vdesc.texture.image = cubemap;
    tex_view = sg_make_view(&vdesc);

    // ---------------------------------------------------------------------
    // 4. Sampler — linear + clamp so face seams don't show.
    // ---------------------------------------------------------------------
    sg_sampler_desc sdesc{};
    sdesc.min_filter = SG_FILTER_LINEAR;
    sdesc.mag_filter = SG_FILTER_LINEAR;
    sdesc.wrap_u     = SG_WRAP_CLAMP_TO_EDGE;
    sdesc.wrap_v     = SG_WRAP_CLAMP_TO_EDGE;
    sdesc.wrap_w     = SG_WRAP_CLAMP_TO_EDGE;
    sampler = sg_make_sampler(&sdesc);

    // ---------------------------------------------------------------------
    // 5. Cube geometry.
    // ---------------------------------------------------------------------
    sg_buffer_desc vbd{};
    vbd.data = SG_RANGE(kCubeVerts);
    vbuf = sg_make_buffer(&vbd);

    sg_buffer_desc ibd{};
    ibd.usage.index_buffer = true;
    ibd.data = SG_RANGE(kCubeIndices);
    ibuf = sg_make_buffer(&ibd);
    index_count = (int)(sizeof(kCubeIndices) / sizeof(kCubeIndices[0]));

    // ---------------------------------------------------------------------
    // 6. Shader + pipeline.
    // ---------------------------------------------------------------------
    shader = sg_make_shader(skybox_shader_desc(sg_query_backend()));

    sg_pipeline_desc pd{};
    pd.shader = shader;
    pd.layout.attrs[ATTR_skybox_a_pos].format = SG_VERTEXFORMAT_FLOAT3;
    pd.index_type            = SG_INDEXTYPE_UINT16;
    pd.cull_mode             = SG_CULLMODE_NONE;
    pd.depth.compare         = SG_COMPAREFUNC_LESS_EQUAL;
    pd.depth.write_enabled   = false;   // other geometry drawn on top
    pd.colors[0].pixel_format = kSceneColorFormat;
    pd.depth.pixel_format     = kSceneDepthFormat;
    pd.sample_count           = kSceneSampleCount;
    pipeline = sg_make_pipeline(&pd);

    return true;
}

void Skybox::draw(const Camera& cam, float aspect) const {
    const HMM_Mat4 view = cam.view_rotation_only();
    const HMM_Mat4 proj = cam.projection(aspect);
    const HMM_Mat4 vp   = HMM_MulM4(proj, view);

    vs_params_t params{};
    std::memcpy(params.view_proj, &vp, sizeof(float) * 16);

    sg_bindings b{};
    b.vertex_buffers[0]          = vbuf;
    b.index_buffer               = ibuf;
    b.views[VIEW_u_sky_tex]      = tex_view;
    b.samplers[SMP_u_sky_smp]    = sampler;

    sg_apply_pipeline(pipeline);
    sg_apply_bindings(&b);
    sg_apply_uniforms(UB_vs_params, SG_RANGE(params));
    sg_draw(0, index_count, 1);
}

void Skybox::destroy() {
    sg_destroy_pipeline(pipeline);
    sg_destroy_shader(shader);
    sg_destroy_buffer(ibuf);
    sg_destroy_buffer(vbuf);
    sg_destroy_sampler(sampler);
    sg_destroy_view(tex_view);
    sg_destroy_image(cubemap);
}

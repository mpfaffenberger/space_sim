#include "sun.h"
#include "render_config.h"

#include "generated/sun.glsl.h"

#include <cmath>
#include <cstring>
#include <cstdio>
#include <vector>

namespace {

// Build a unit-radius UV sphere. Position doubles as the normal (valid only
// because the sphere is unit-sized and centered at origin). Vertex shader
// scales + translates to the world-space sun.
void build_uv_sphere(int lat, int lon,
                     std::vector<float>& verts,
                     std::vector<uint16_t>& idx) {
    verts.clear();
    idx.clear();
    verts.reserve((size_t)(lat + 1) * (lon + 1) * 3);
    idx.reserve((size_t)lat * lon * 6);

    for (int i = 0; i <= lat; ++i) {
        const float v     = (float)i / (float)lat;
        const float theta = v * (float)M_PI;             // 0..π
        const float sinT  = std::sin(theta);
        const float cosT  = std::cos(theta);
        for (int j = 0; j <= lon; ++j) {
            const float u    = (float)j / (float)lon;
            const float phi  = u * 2.0f * (float)M_PI;   // 0..2π
            verts.push_back(sinT * std::cos(phi));
            verts.push_back(cosT);
            verts.push_back(sinT * std::sin(phi));
        }
    }

    const int stride = lon + 1;
    for (int i = 0; i < lat; ++i) {
        for (int j = 0; j < lon; ++j) {
            const uint16_t a = (uint16_t)(i * stride + j);
            const uint16_t b = (uint16_t)(a + 1);
            const uint16_t c = (uint16_t)((i + 1) * stride + j);
            const uint16_t d = (uint16_t)(c + 1);
            idx.push_back(a); idx.push_back(c); idx.push_back(b);
            idx.push_back(b); idx.push_back(c); idx.push_back(d);
        }
    }
}

} // namespace

bool Sun::init(int lat_segments, int lon_segments) {
    // ---- solid sphere ----------------------------------------------------
    std::vector<float>    verts;
    std::vector<uint16_t> idx;
    build_uv_sphere(lat_segments, lon_segments, verts, idx);
    index_count = (int)idx.size();

    sg_buffer_desc vbd{};
    vbd.data = { verts.data(), verts.size() * sizeof(float) };
    vbuf = sg_make_buffer(&vbd);

    sg_buffer_desc ibd{};
    ibd.usage.index_buffer = true;
    ibd.data = { idx.data(), idx.size() * sizeof(uint16_t) };
    ibuf = sg_make_buffer(&ibd);

    shader = sg_make_shader(sun_shader_desc(sg_query_backend()));

    sg_pipeline_desc pd{};
    pd.shader = shader;
    pd.layout.attrs[ATTR_sun_a_pos].format = SG_VERTEXFORMAT_FLOAT3;
    pd.index_type          = SG_INDEXTYPE_UINT16;
    pd.cull_mode           = SG_CULLMODE_BACK;
    pd.depth.compare       = SG_COMPAREFUNC_LESS_EQUAL;
    pd.depth.write_enabled = true;
    pd.colors[0].pixel_format = kSceneColorFormat;
    pd.depth.pixel_format     = kSceneDepthFormat;
    pd.sample_count           = kSceneSampleCount;
    pipeline = sg_make_pipeline(&pd);

    if (sg_query_pipeline_state(pipeline) != SG_RESOURCESTATE_VALID) {
        std::fprintf(stderr, "[sun] sphere pipeline creation failed\n");
        return false;
    }

    // ---- corona & gas shell (separate TUs) -------------------------------
    if (!sun_corona_init(*this)) return false;
    if (!sun_gas_init(*this))    return false;
    return true;
}

void Sun::draw(const Camera& cam, float aspect, float time_sec) const {
    const HMM_Mat4 vp = HMM_MulM4(cam.projection(aspect), cam.view());

    // Solid sphere pass.
    vs_params_t vsp{};
    std::memcpy(vsp.view_proj, &vp, sizeof(float) * 16);
    vsp.world_pos[0] = position.X; vsp.world_pos[1] = position.Y;
    vsp.world_pos[2] = position.Z; vsp.world_pos[3] = 0.0f;
    vsp.radius[0] = radius; vsp.radius[1] = 0.0f;
    vsp.radius[2] = 0.0f;   vsp.radius[3] = 0.0f;

    fs_params_t fsp{};
    fsp.core_color[0] = core_color.X; fsp.core_color[1] = core_color.Y;
    fsp.core_color[2] = core_color.Z; fsp.core_color[3] = 1.0f;
    fsp.glow_color[0] = glow_color.X; fsp.glow_color[1] = glow_color.Y;
    fsp.glow_color[2] = glow_color.Z; fsp.glow_color[3] = 1.0f;
    fsp.view_and_tightness[0] = cam.position.X;
    fsp.view_and_tightness[1] = cam.position.Y;
    fsp.view_and_tightness[2] = cam.position.Z;
    fsp.view_and_tightness[3] = rim_tightness;
    fsp.plasma_params[0] = time_sec;
    fsp.plasma_params[1] = granule_scale;
    fsp.plasma_params[2] = plasma_flow;
    fsp.plasma_params[3] = plasma_contrast;

    sg_bindings b{};
    b.vertex_buffers[0] = vbuf;
    b.index_buffer      = ibuf;

    sg_apply_pipeline(pipeline);
    sg_apply_bindings(&b);
    sg_apply_uniforms(UB_vs_params, SG_RANGE(vsp));
    sg_apply_uniforms(UB_fs_params, SG_RANGE(fsp));
    sg_draw(0, index_count, 1);

    // Gas shell pass — drawn AFTER the sphere, additively, so wisps
    // hug the silhouette and bleed outward without a hard boundary.
    sun_gas_draw(*this, cam, vp, time_sec);

    // Outer corona / god rays, on top of gas.
    sun_corona_draw(*this, cam, vp);
}

void Sun::destroy() {
    sun_gas_destroy(*this);
    sun_corona_destroy(*this);
    sg_destroy_pipeline(pipeline);
    sg_destroy_shader(shader);
    sg_destroy_buffer(ibuf);
    sg_destroy_buffer(vbuf);
}

HMM_Vec3 Sun::light_dir_to(HMM_Vec3 world_pos) const {
    return HMM_NormV3(HMM_SubV3(position, world_pos));
}

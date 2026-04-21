#include "dust.h"
#include "render_config.h"

#include "generated/dust.glsl.h"

#include <cstring>
#include <random>
#include <vector>

bool DustField::init() {
    // Deterministic seed so the dust pattern is reproducible between runs.
    // This is pure eye candy so we don't bother exposing the seed as config.
    std::mt19937 rng(0xD0571EC0u);
    std::uniform_real_distribution<float> U(-1.0f, 1.0f);

    std::vector<float> verts;
    verts.reserve((size_t)count * 3);
    for (int i = 0; i < count; ++i) {
        verts.push_back(U(rng));
        verts.push_back(U(rng));
        verts.push_back(U(rng));
    }

    sg_buffer_desc vbd{};
    vbd.data = { verts.data(), verts.size() * sizeof(float) };
    vbuf = sg_make_buffer(&vbd);

    shader = sg_make_shader(dust_shader_desc(sg_query_backend()));

    sg_pipeline_desc pd{};
    pd.shader = shader;
    pd.layout.attrs[ATTR_dust_a_pos].format = SG_VERTEXFORMAT_FLOAT3;
    pd.primitive_type = SG_PRIMITIVETYPE_POINTS;
    // Additive alpha-blend so dust brightens whatever's behind it, never
    // darkens. Depth-test enabled so dust is correctly occluded by solids;
    // depth-write disabled so later transparent things still blend over dust.
    pd.colors[0].blend.enabled         = true;
    pd.colors[0].blend.src_factor_rgb  = SG_BLENDFACTOR_SRC_ALPHA;
    pd.colors[0].blend.dst_factor_rgb  = SG_BLENDFACTOR_ONE;
    pd.colors[0].blend.src_factor_alpha = SG_BLENDFACTOR_ONE;
    pd.colors[0].blend.dst_factor_alpha = SG_BLENDFACTOR_ONE;
    pd.depth.compare       = SG_COMPAREFUNC_LESS_EQUAL;
    pd.depth.write_enabled = false;
    pd.colors[0].pixel_format = kSceneColorFormat;
    pd.depth.pixel_format     = kSceneDepthFormat;
    pd.sample_count           = kSceneSampleCount;
    pipeline = sg_make_pipeline(&pd);
    return sg_query_pipeline_state(pipeline) == SG_RESOURCESTATE_VALID;
}

void DustField::draw(const Camera& cam, float aspect) const {
    const HMM_Mat4 vp = HMM_MulM4(cam.projection(aspect), cam.view());

    vs_params_t vsp{};
    std::memcpy(vsp.view_proj, &vp, sizeof(float) * 16);
    vsp.cam_pos[0] = cam.position.X; vsp.cam_pos[1] = cam.position.Y;
    vsp.cam_pos[2] = cam.position.Z; vsp.cam_pos[3] = 0.0f;
    vsp.field_params[0] = wrap_extent;
    vsp.field_params[1] = point_size_px;
    vsp.field_params[2] = 0.0f;
    vsp.field_params[3] = 0.0f;

    sg_bindings b{};
    b.vertex_buffers[0] = vbuf;

    sg_apply_pipeline(pipeline);
    sg_apply_bindings(&b);
    sg_apply_uniforms(UB_vs_params, SG_RANGE(vsp));
    sg_draw(0, count, 1);
}

void DustField::destroy() {
    sg_destroy_pipeline(pipeline);
    sg_destroy_shader(shader);
    sg_destroy_buffer(vbuf);
}

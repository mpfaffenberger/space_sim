// -----------------------------------------------------------------------------
// mesh_render.cpp — pipeline + per-draw uniform setup for loaded meshes.
// -----------------------------------------------------------------------------

#include "mesh_render.h"
#include "camera.h"
#include "render_config.h"

#include "generated/mesh.glsl.h"

// Atmosphere pipeline lives in its own TU so its generated shader
// header (which reuses identifiers like `vs_source_metal_macos`) doesn't
// collide with mesh.glsl.h inside a single compilation unit.
bool init_mesh_atmosphere_pipeline(MeshRenderer& m);
void destroy_mesh_atmosphere_pipeline(MeshRenderer& m);
void draw_mesh_atmosphere_pass(const MeshRenderer& m,
                               const std::vector<PlacedMesh>& meshes,
                               const HMM_Mat4& view_proj,
                               HMM_Vec3 sun_pos,
                               HMM_Vec3 camera_pos);

#include <cmath>
#include <cstdio>
#include <cstring>

// Build a model matrix from position + euler (deg) + uniform scale.
// Rotation order: yaw (Y) → pitch (X) → roll (Z). "Ship conventions."
// External linkage so mesh_render_atmosphere.cpp can share this instead
// of redefining the same transform logic (DRY, and more importantly:
// any correction to rotation order lands in exactly one place).
HMM_Mat4 model_matrix(HMM_Vec3 pos, HMM_Vec3 euler_deg, float s) {
    const HMM_Vec3 e = {
        euler_deg.X * HMM_DegToRad,
        euler_deg.Y * HMM_DegToRad,
        euler_deg.Z * HMM_DegToRad,
    };
    HMM_Mat4 T  = HMM_Translate(pos);
    HMM_Mat4 Ry = HMM_Rotate_RH(e.Y, HMM_V3(0, 1, 0));
    HMM_Mat4 Rx = HMM_Rotate_RH(e.X, HMM_V3(1, 0, 0));
    HMM_Mat4 Rz = HMM_Rotate_RH(e.Z, HMM_V3(0, 0, 1));
    HMM_Mat4 S  = HMM_Scale(HMM_V3(s, s, s));
    return HMM_MulM4(HMM_MulM4(HMM_MulM4(HMM_MulM4(T, Ry), Rx), Rz), S);
}

bool MeshRenderer::init() {
    shader = sg_make_shader(mesh_shader_desc(sg_query_backend()));

    // ---- shared sampler (linear, repeat) --------------------------------
    sg_sampler_desc ss{};
    ss.min_filter = SG_FILTER_LINEAR;
    ss.mag_filter = SG_FILTER_LINEAR;
    ss.mipmap_filter = SG_FILTER_LINEAR;
    ss.wrap_u = SG_WRAP_REPEAT;
    ss.wrap_v = SG_WRAP_REPEAT;
    sampler = sg_make_sampler(&ss);

    // ---- 1×1 neutral fallbacks (white / gray / black) -------------------
    // Every shader binding slot needs a valid view. When a mesh doesn't
    // provide a specific texture we bind one of these 1-pixel images so
    // the sampling path is uniform and absent slots contribute neutral
    // values (white→diffuse stays albedo-free, gray→moderate shine,
    // black→no glow). Saves a shader permutation per missing slot.
    auto make_1x1 = [](const uint8_t rgba[4], sg_image& img, sg_view& view) {
        sg_image_desc id{};
        id.width = 1;
        id.height = 1;
        id.pixel_format = SG_PIXELFORMAT_RGBA8;
        id.data.mip_levels[0] = { rgba, 4 };
        img = sg_make_image(&id);
        sg_view_desc vd{};
        vd.texture.image = img;
        view = sg_make_view(&vd);
    };
    const uint8_t white[4]       = { 255, 255, 255, 255 };
    const uint8_t gray [4]       = { 128, 128, 128, 255 };
    const uint8_t black[4]       = {   0,   0,   0, 255 };
    // Tangent-space (0, 0, 1) encoded as RGB. Any mesh without a normal
    // map gets this bound and the shader produces an unperturbed surface
    // normal — i.e. smooth shading identical to the pre-normal-map path.
    const uint8_t flat_normal[4] = { 128, 128, 255, 255 };
    make_1x1(white,       white_1x1,       white_1x1_view);
    make_1x1(gray,        gray_1x1,        gray_1x1_view);
    make_1x1(black,       black_1x1,       black_1x1_view);
    make_1x1(flat_normal, flat_normal_1x1, flat_normal_1x1_view);

    sg_pipeline_desc pd{};
    pd.shader = shader;
    pd.layout.buffers[0].stride = sizeof(MeshVertex);
    pd.layout.attrs[ATTR_mesh_a_pos].format     = SG_VERTEXFORMAT_FLOAT3;
    pd.layout.attrs[ATTR_mesh_a_pos].offset     = 0;
    pd.layout.attrs[ATTR_mesh_a_normal].format  = SG_VERTEXFORMAT_FLOAT3;
    pd.layout.attrs[ATTR_mesh_a_normal].offset  = 12;
    pd.layout.attrs[ATTR_mesh_a_uv].format      = SG_VERTEXFORMAT_FLOAT2;
    pd.layout.attrs[ATTR_mesh_a_uv].offset      = 24;

    pd.index_type = SG_INDEXTYPE_UINT16;
    pd.depth.compare       = SG_COMPAREFUNC_LESS_EQUAL;
    pd.depth.write_enabled = true;
    pd.colors[0].pixel_format = kSceneColorFormat;
    pd.depth.pixel_format     = kSceneDepthFormat;
    pd.sample_count           = kSceneSampleCount;

    // Two variants of the same pipeline, differing only in cull mode.
    // Clean geometry (our icosphere) uses CULL_BACK so closed shapes
    // stay closed. Imported meshes (WCU ships / stations) use CULL_NONE
    // because the source art has inconsistent winding — culling any
    // side would punch holes in the hull.
    pd.cull_mode = SG_CULLMODE_BACK;
    pipeline_cull_back = sg_make_pipeline(&pd);
    pd.cull_mode = SG_CULLMODE_NONE;
    pipeline_two_sided = sg_make_pipeline(&pd);
    if (sg_query_pipeline_state(pipeline_cull_back) != SG_RESOURCESTATE_VALID ||
        sg_query_pipeline_state(pipeline_two_sided) != SG_RESOURCESTATE_VALID) {
        std::fprintf(stderr, "[mesh_render] pipeline creation failed\n");
        return false;
    }

    if (!init_mesh_atmosphere_pipeline(*this)) return false;
    return true;
}

void MeshRenderer::destroy() {
    sg_destroy_view(flat_normal_1x1_view); sg_destroy_image(flat_normal_1x1);
    sg_destroy_view(black_1x1_view);  sg_destroy_image(black_1x1);
    sg_destroy_view(gray_1x1_view);   sg_destroy_image(gray_1x1);
    sg_destroy_view(white_1x1_view);  sg_destroy_image(white_1x1);
    sg_destroy_sampler(sampler);
    destroy_mesh_atmosphere_pipeline(*this);
    sg_destroy_pipeline(pipeline_two_sided);
    sg_destroy_pipeline(pipeline_cull_back);
    sg_destroy_shader(shader);
}

void MeshRenderer::draw(const std::vector<PlacedMesh>& meshes,
                        const Camera& cam, float aspect,
                        HMM_Vec3 sun_pos, HMM_Vec3 sun_color,
                        HMM_Vec3 rim_tint, float rim_strength) const {
    if (meshes.empty()) return;

    const HMM_Mat4 vp = HMM_MulM4(cam.projection(aspect), cam.view());

    // Bind each pipeline once and draw all meshes that want it. Saves
    // per-draw pipeline state changes when most placed meshes share
    // the same cull convention (the common case).
    sg_pipeline current_pipe{};
    for (const auto& pm : meshes) {
        const sg_pipeline want = pm.double_sided ? pipeline_two_sided
                                                 : pipeline_cull_back;
        if (want.id != current_pipe.id) {
            sg_apply_pipeline(want);
            current_pipe = want;
        }
        const HMM_Mat4 M = model_matrix(pm.position, pm.euler_deg, pm.scale);

        mesh_vs_params_t vsp{};
        std::memcpy(vsp.view_proj, &vp, sizeof(float) * 16);
        std::memcpy(vsp.model,     &M,  sizeof(float) * 16);

        // Per-mesh overrides fall back to the global values when < 0.
        // Keeps the old ship behaviour unchanged while letting a planet
        // set a lower ambient floor (darker night side) and zero rim
        // (atmosphere halo handles silhouette lift instead).
        const float kGlobalAmbientFloor = 0.45f;
        const float amb = pm.ambient_floor < 0.0f ? kGlobalAmbientFloor
                                                  : pm.ambient_floor;
        const float rim_s = pm.rim_strength  < 0.0f ? rim_strength
                                                    : pm.rim_strength;

        mesh_fs_params_t fsp{};
        fsp.sun_pos[0] = sun_pos.X; fsp.sun_pos[1] = sun_pos.Y; fsp.sun_pos[2] = sun_pos.Z;
        fsp.sun_color[0] = sun_color.X; fsp.sun_color[1] = sun_color.Y; fsp.sun_color[2] = sun_color.Z;
        fsp.sun_color[3] = amb;    // shader reads this as AMBIENT_FLOOR
        fsp.camera_pos[0] = cam.position.X;
        fsp.camera_pos[1] = cam.position.Y;
        fsp.camera_pos[2] = cam.position.Z;
        fsp.rim_tint[0] = rim_tint.X; fsp.rim_tint[1] = rim_tint.Y; fsp.rim_tint[2] = rim_tint.Z;
        fsp.rim_tint[3] = rim_s;
        fsp.body_tint[0] = pm.body_tint.X;
        fsp.body_tint[1] = pm.body_tint.Y;
        fsp.body_tint[2] = pm.body_tint.Z;
        fsp.body_tint[3] = pm.spec_amount;

        // One draw call per submesh. Each submesh binds its own Material
        // (different diffuse/spec/glow per region of the mesh). Missing
        // material slots get substituted with the neutral 1×1 fallbacks
        // so the binding topology stays identical across all draws.
        sg_apply_uniforms(UB_mesh_vs_params, SG_RANGE(vsp));
        sg_apply_uniforms(UB_mesh_fs_params, SG_RANGE(fsp));
        for (const auto& sm : pm.mesh.submeshes) {
            const Material& mat = pm.mesh.materials[sm.material_idx];

            sg_bindings b{};
            b.vertex_buffers[0] = pm.mesh.vbuf;
            b.index_buffer      = pm.mesh.ibuf;
            // Clay mode binds neutral 1×1 fallbacks for ALL channels —
            // bypasses per-mesh textures so AI atlas captures see pure
            // shape + shading without baked-in patterns leaking through.
            const bool clay = pm.clay_mode;
            b.views[0] = (clay || !mat.diffuse.valid) ? white_1x1_view       : mat.diffuse.view;
            b.views[1] = (clay || !mat.spec.valid)    ? gray_1x1_view        : mat.spec.view;
            b.views[2] = (clay || !mat.glow.valid)    ? black_1x1_view       : mat.glow.view;
            b.views[3] = (clay || !mat.normal.valid)  ? flat_normal_1x1_view : mat.normal.view;
            b.samplers[0] = sampler;
            sg_apply_bindings(&b);
            sg_draw((int)sm.index_start, (int)sm.index_count, 1);
        }
    }

    // Atmospheric halos are a second pass handled in a separate TU so
    // its generated shader header doesn't collide with mesh.glsl.h.
    draw_mesh_atmosphere_pass(*this, meshes, vp, sun_pos, cam.position);
}

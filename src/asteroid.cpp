// -----------------------------------------------------------------------------
// asteroid.cpp — procedural asteroid field generation + instanced draw.
// -----------------------------------------------------------------------------

#include "asteroid.h"
#include "camera.h"
#include "render_config.h"

#include "generated/asteroid.glsl.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace {

// ---- tiny RNG (no <random> tax) --------------------------------------------
//
// PCG step — same family we used elsewhere. Deterministic per-seed so a given
// field seed always yields the same rock layout. That matters if we ever
// want to save/restore a universe.
uint32_t pcg_next(uint32_t& s) {
    s = s * 747796405u + 2891336453u;
    uint32_t w = ((s >> ((s >> 28) + 4)) ^ s) * 277803737u;
    return (w >> 22) ^ w;
}
float rand01(uint32_t& s) { return (pcg_next(s) & 0xffffff) / float(0x1000000); }
float randpm(uint32_t& s) { return rand01(s) * 2.0f - 1.0f; }

// ---- 3D value noise (CPU-side, matches the mental model of sun.glsl) -------
float hash3_u32(int x, int y, int z, uint32_t seed) {
    uint32_t h = (uint32_t)x * 0x27d4eb2du
               ^ (uint32_t)y * 0x165667b1u
               ^ (uint32_t)z * 0xd1b54a35u
               ^ seed;
    uint32_t s = h;
    return rand01(s);
}

float vnoise3(HMM_Vec3 p, uint32_t seed) {
    int xi = (int)std::floor(p.X), yi = (int)std::floor(p.Y), zi = (int)std::floor(p.Z);
    float xf = p.X - xi, yf = p.Y - yi, zf = p.Z - zi;
    // Smoothstep each axis so the lattice isn't visible.
    float u = xf * xf * (3.0f - 2.0f * xf);
    float v = yf * yf * (3.0f - 2.0f * yf);
    float w = zf * zf * (3.0f - 2.0f * zf);

    float c000 = hash3_u32(xi,     yi,     zi,     seed);
    float c100 = hash3_u32(xi + 1, yi,     zi,     seed);
    float c010 = hash3_u32(xi,     yi + 1, zi,     seed);
    float c110 = hash3_u32(xi + 1, yi + 1, zi,     seed);
    float c001 = hash3_u32(xi,     yi,     zi + 1, seed);
    float c101 = hash3_u32(xi + 1, yi,     zi + 1, seed);
    float c011 = hash3_u32(xi,     yi + 1, zi + 1, seed);
    float c111 = hash3_u32(xi + 1, yi + 1, zi + 1, seed);

    float x00 = c000 * (1.0f - u) + c100 * u;
    float x10 = c010 * (1.0f - u) + c110 * u;
    float x01 = c001 * (1.0f - u) + c101 * u;
    float x11 = c011 * (1.0f - u) + c111 * u;
    float y0  = x00  * (1.0f - v) + x10  * v;
    float y1  = x01  * (1.0f - v) + x11  * v;
    return y0 * (1.0f - w) + y1 * w;
}

float fbm3(HMM_Vec3 p, uint32_t seed, int octaves = 4) {
    float sum = 0.0f, amp = 0.5f;
    for (int i = 0; i < octaves; ++i) {
        sum += vnoise3(p, seed) * amp;
        p   = HMM_MulV3F(p, 2.13f);
        amp *= 0.5f;
    }
    return sum;
}

// ---- icosahedron base shape ------------------------------------------------
void make_icosahedron(std::vector<HMM_Vec3>& verts, std::vector<uint16_t>& idx) {
    const float t = (1.0f + std::sqrt(5.0f)) / 2.0f;
    verts = {
        {-1,  t, 0}, { 1,  t, 0}, {-1, -t, 0}, { 1, -t, 0},
        { 0, -1, t}, { 0,  1, t}, { 0, -1,-t}, { 0,  1,-t},
        { t,  0,-1}, { t,  0, 1}, {-t,  0,-1}, {-t,  0, 1},
    };
    for (auto& v : verts) v = HMM_NormV3(v);
    idx = {
         0, 11,  5,   0,  5,  1,   0,  1,  7,   0,  7, 10,   0, 10, 11,
         1,  5,  9,   5, 11,  4,  11, 10,  2,  10,  7,  6,   7,  1,  8,
         3,  9,  4,   3,  4,  2,   3,  2,  6,   3,  6,  8,   3,  8,  9,
         4,  9,  5,   2,  4, 11,   6,  2, 10,   8,  6,  7,   9,  8,  1,
    };
}

// Subdivide each triangle into 4, snap new midpoint verts to unit sphere.
void subdivide(std::vector<HMM_Vec3>& verts, std::vector<uint16_t>& idx, int levels) {
    for (int lvl = 0; lvl < levels; ++lvl) {
        std::unordered_map<uint32_t, uint16_t> mid_cache;
        auto midpoint = [&](uint16_t a, uint16_t b) -> uint16_t {
            uint32_t key = ((uint32_t)std::min(a, b) << 16) | std::max(a, b);
            auto it = mid_cache.find(key);
            if (it != mid_cache.end()) return it->second;
            HMM_Vec3 m = HMM_NormV3(HMM_MulV3F(HMM_AddV3(verts[a], verts[b]), 0.5f));
            uint16_t new_idx = (uint16_t)verts.size();
            verts.push_back(m);
            mid_cache[key] = new_idx;
            return new_idx;
        };

        std::vector<uint16_t> new_idx;
        new_idx.reserve(idx.size() * 4);
        for (size_t t = 0; t < idx.size(); t += 3) {
            uint16_t a = idx[t], b = idx[t+1], c = idx[t+2];
            uint16_t ab = midpoint(a, b);
            uint16_t bc = midpoint(b, c);
            uint16_t ca = midpoint(c, a);
            new_idx.insert(new_idx.end(), {
                a,  ab, ca,
                ab, b,  bc,
                ca, bc, c,
                ab, bc, ca,
            });
        }
        idx = std::move(new_idx);
    }
}

// Radially displace each vertex with a LAYERED noise field, turning the
// sphere into a proper rock:
//
//   low-freq fbm      → dictates the overall lumpy silhouette
//   mid-freq RIDGED   → sharp crevice lines (ridged = 1 - |vnoise*2-1|)
//   high-freq vnoise  → fine pitting that flat-shading will pick up as
//                       per-triangle variation
//
// Biased inward by subtracting 0.55 from the low-freq term — real rocky
// surfaces have more concave pits than convex lumps, which reads as
// "impact-cratered" to the eye.
void displace(std::vector<HMM_Vec3>& verts, uint32_t variant_seed) {
    for (auto& v : verts) {
        HMM_Vec3 p = v;
        float n_low   = fbm3(HMM_MulV3F(p, 1.4f), variant_seed, 3);
        float n_mid   = vnoise3(HMM_MulV3F(p, 3.2f), variant_seed + 17u);
        float n_ridge = 1.0f - std::abs(n_mid * 2.0f - 1.0f);    // [0..1], sharp at 0.5
        float n_fine  = vnoise3(HMM_MulV3F(p, 7.5f), variant_seed + 101u);

        float d = (n_low  - 0.55f) * 0.55f
                + (n_ridge - 0.45f) * 0.18f
                + (n_fine  - 0.50f) * 0.05f;
        // Clamp the inward displacement. Pushing a vertex past ~0.6× its
        // original radius can cause adjacent triangles to fold through
        // each other — the winding inverts and back-face culling then
        // drills a hole through to the skybox. Outward displacement
        // doesn't have this risk, so we only clamp the lower bound.
        if (d < -0.35f) d = -0.35f;
        v = HMM_MulV3F(v, 1.0f + d);
    }
}

// Per-vertex normals via area-weighted face-normal accumulation.
std::vector<HMM_Vec3> compute_normals(const std::vector<HMM_Vec3>& verts,
                                      const std::vector<uint16_t>& idx) {
    std::vector<HMM_Vec3> N(verts.size(), {0, 0, 0});
    for (size_t t = 0; t < idx.size(); t += 3) {
        const HMM_Vec3& a = verts[idx[t]];
        const HMM_Vec3& b = verts[idx[t+1]];
        const HMM_Vec3& c = verts[idx[t+2]];
        HMM_Vec3 face = HMM_Cross(HMM_SubV3(b, a), HMM_SubV3(c, a));
        // unnormalised cross = area-weighted, which is exactly what we want
        N[idx[t]]   = HMM_AddV3(N[idx[t]],   face);
        N[idx[t+1]] = HMM_AddV3(N[idx[t+1]], face);
        N[idx[t+2]] = HMM_AddV3(N[idx[t+2]], face);
    }
    for (auto& n : N) n = HMM_NormV3(n);
    return N;
}

// Build a single asteroid mesh variant.
Mesh generate_variant(uint32_t variant_seed) {
    std::vector<HMM_Vec3> positions;
    std::vector<uint16_t> indices;
    make_icosahedron(positions, indices);
    // Level 3: 1280 tris, 642 verts. Worth the memory — layered
    // displacement needs enough vertices to carve the ridges.
    subdivide(positions, indices, /*levels=*/3);
    displace(positions, variant_seed);
    auto normals = compute_normals(positions, indices);

    Mesh m;
    m.vertices.resize(positions.size());
    for (size_t i = 0; i < positions.size(); ++i) {
        m.vertices[i].pos[0]    = positions[i].X;
        m.vertices[i].pos[1]    = positions[i].Y;
        m.vertices[i].pos[2]    = positions[i].Z;
        m.vertices[i].normal[0] = normals[i].X;
        m.vertices[i].normal[1] = normals[i].Y;
        m.vertices[i].normal[2] = normals[i].Z;
    }
    m.indices = std::move(indices);
    return m;
}

// ---- per-instance struct (matches shader layout, tight-packed) -------------
struct AsteroidInstance {
    float pos_scale[4];    // world xyz + uniform scale
    float axis_phase[4];   // unit axis xyz + initial phase
    float speed_tint[4];   // angular velocity + rgb tint
};
static_assert(sizeof(AsteroidInstance) == 48, "instance layout wrong");

} // namespace

bool AsteroidField::init() {
    // ---- generate N distinct mesh variants --------------------------------
    for (int v = 0; v < kVariantCount; ++v) {
        variant_mesh[v] = generate_variant(seed + v * 0x9E3779B9u);
        if (!variant_mesh[v].upload()) return false;
    }

    // ---- scatter instances across the field volume -----------------------
    // Each variant gets ~total_count / kVariantCount instances.
    uint32_t rng = seed;
    std::vector<std::vector<AsteroidInstance>> per_variant(kVariantCount);
    for (int i = 0; i < total_count; ++i) {
        int v_idx = i % kVariantCount;
        AsteroidInstance inst{};

        HMM_Vec3 pos = {
            center.X + randpm(rng) * half_extent.X,
            center.Y + randpm(rng) * half_extent.Y,
            center.Z + randpm(rng) * half_extent.Z,
        };
        float scale = base_radius * (size_min + rand01(rng) * (size_max - size_min));
        inst.pos_scale[0] = pos.X; inst.pos_scale[1] = pos.Y;
        inst.pos_scale[2] = pos.Z; inst.pos_scale[3] = scale;

        HMM_Vec3 axis = HMM_NormV3({ randpm(rng), randpm(rng), randpm(rng) });
        float    phase = rand01(rng) * 6.2831853f;
        inst.axis_phase[0] = axis.X; inst.axis_phase[1] = axis.Y;
        inst.axis_phase[2] = axis.Z; inst.axis_phase[3] = phase;

        // Small rocks tumble faster than big ones (conservation-of-angular-
        // -momentum hand-wave). Sign random so some rotate the other way.
        float ang_vel = (rand01(rng) - 0.5f) * 0.8f * (base_radius / scale);
        // Desaturated palette — real asteroids are basalt/carbonaceous/metal
        // grays, not rainbow candy. Tight variation to keep the field
        // cohesive instead of "bag of marbles".
        float g = 0.38f + (rand01(rng) - 0.5f) * 0.12f;  // base gray
        HMM_Vec3 tint = {
            g + 0.04f,                                    // slight warm bias
            g - 0.01f + (rand01(rng) - 0.5f) * 0.04f,
            g - 0.06f + (rand01(rng) - 0.5f) * 0.03f,
        };
        inst.speed_tint[0] = ang_vel;
        inst.speed_tint[1] = tint.X; inst.speed_tint[2] = tint.Y; inst.speed_tint[3] = tint.Z;

        per_variant[v_idx].push_back(inst);
    }

    for (int v = 0; v < kVariantCount; ++v) {
        sg_buffer_desc bd{};
        bd.usage.vertex_buffer = true;
        bd.data.ptr  = per_variant[v].data();
        bd.data.size = per_variant[v].size() * sizeof(AsteroidInstance);
        instance_buf[v]  = sg_make_buffer(&bd);
        instance_count[v] = (int)per_variant[v].size();
    }

    // ---- shader + pipeline ------------------------------------------------
    shader = sg_make_shader(asteroid_shader_desc(sg_query_backend()));

    sg_pipeline_desc pd{};
    pd.shader = shader;
    pd.layout.buffers[0].stride    = sizeof(MeshVertex);
    pd.layout.buffers[0].step_func = SG_VERTEXSTEP_PER_VERTEX;
    pd.layout.buffers[1].stride    = sizeof(AsteroidInstance);
    pd.layout.buffers[1].step_func = SG_VERTEXSTEP_PER_INSTANCE;

    pd.layout.attrs[ATTR_asteroid_a_pos].format         = SG_VERTEXFORMAT_FLOAT3;
    pd.layout.attrs[ATTR_asteroid_a_pos].buffer_index   = 0;
    pd.layout.attrs[ATTR_asteroid_a_normal].format      = SG_VERTEXFORMAT_FLOAT3;
    pd.layout.attrs[ATTR_asteroid_a_normal].buffer_index = 0;
    pd.layout.attrs[ATTR_asteroid_a_normal].offset      = 12;

    pd.layout.attrs[ATTR_asteroid_i_pos_scale].format   = SG_VERTEXFORMAT_FLOAT4;
    pd.layout.attrs[ATTR_asteroid_i_pos_scale].buffer_index = 1;
    pd.layout.attrs[ATTR_asteroid_i_axis_phase].format  = SG_VERTEXFORMAT_FLOAT4;
    pd.layout.attrs[ATTR_asteroid_i_axis_phase].buffer_index = 1;
    pd.layout.attrs[ATTR_asteroid_i_axis_phase].offset  = 16;
    pd.layout.attrs[ATTR_asteroid_i_speed_tint].format  = SG_VERTEXFORMAT_FLOAT4;
    pd.layout.attrs[ATTR_asteroid_i_speed_tint].buffer_index = 1;
    pd.layout.attrs[ATTR_asteroid_i_speed_tint].offset  = 32;

    pd.index_type    = SG_INDEXTYPE_UINT16;
    // No back-face culling. Belt-and-suspenders with the displacement
    // clamp above: any residual inverted triangles still draw, so the
    // rock reads as solid from every angle. The overdraw cost is trivial
    // (backface tris fail the depth test almost immediately once the
    // front face has written depth).
    pd.cull_mode     = SG_CULLMODE_NONE;
    pd.depth.compare       = SG_COMPAREFUNC_LESS_EQUAL;
    pd.depth.write_enabled = true;
    pd.colors[0].pixel_format = kSceneColorFormat;
    pd.depth.pixel_format     = kSceneDepthFormat;
    pd.sample_count           = kSceneSampleCount;

    pipeline = sg_make_pipeline(&pd);
    if (sg_query_pipeline_state(pipeline) != SG_RESOURCESTATE_VALID) {
        std::fprintf(stderr, "[asteroid] pipeline creation failed\n");
        return false;
    }
    return true;
}

void AsteroidField::destroy() {
    sg_destroy_pipeline(pipeline);
    sg_destroy_shader(shader);
    for (int v = 0; v < kVariantCount; ++v) {
        sg_destroy_buffer(instance_buf[v]);
        variant_mesh[v].destroy();
    }
}

void AsteroidField::draw(const Camera& cam, float aspect, float time_sec,
                         HMM_Vec3 sun_pos, HMM_Vec3 sun_color,
                         HMM_Vec3 rim_tint, float rim_strength) const {
    const HMM_Mat4 vp = HMM_MulM4(cam.projection(aspect), cam.view());

    asteroid_vs_params_t vsp{};
    std::memcpy(vsp.view_proj, &vp, sizeof(float) * 16);
    vsp.sun_pos_time[0] = sun_pos.X;
    vsp.sun_pos_time[1] = sun_pos.Y;
    vsp.sun_pos_time[2] = sun_pos.Z;
    vsp.sun_pos_time[3] = time_sec;

    asteroid_fs_params_t fsp{};
    fsp.sun_color_amb[0] = sun_color.X;
    fsp.sun_color_amb[1] = sun_color.Y;
    fsp.sun_color_amb[2] = sun_color.Z;
    fsp.sun_color_amb[3] = 1.0f;
    fsp.sun_world_pos[0] = sun_pos.X;
    fsp.sun_world_pos[1] = sun_pos.Y;
    fsp.sun_world_pos[2] = sun_pos.Z;
    fsp.camera_pos[0] = cam.position.X;
    fsp.camera_pos[1] = cam.position.Y;
    fsp.camera_pos[2] = cam.position.Z;
    fsp.rim_tint[0] = rim_tint.X;
    fsp.rim_tint[1] = rim_tint.Y;
    fsp.rim_tint[2] = rim_tint.Z;
    fsp.rim_tint[3] = rim_strength;

    sg_apply_pipeline(pipeline);

    for (int v = 0; v < kVariantCount; ++v) {
        if (instance_count[v] == 0) continue;
        sg_bindings b{};
        b.vertex_buffers[0] = variant_mesh[v].vbuf;
        b.vertex_buffers[1] = instance_buf[v];
        b.index_buffer      = variant_mesh[v].ibuf;
        sg_apply_bindings(&b);
        sg_apply_uniforms(UB_asteroid_vs_params, SG_RANGE(vsp));
        sg_apply_uniforms(UB_asteroid_fs_params, SG_RANGE(fsp));
        sg_draw(0, variant_mesh[v].index_count, instance_count[v]);
    }
}

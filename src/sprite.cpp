// -----------------------------------------------------------------------------
// sprite.cpp — SpriteRenderer + SpriteArt loader.
//
// See sprite.h for the architectural "why". This file is the mechanics:
//   * unit-quad geometry (4 verts, 6 indices, shared between all sprites)
//   * one shader, two pipelines (alpha-blend + additive)
//   * billboarding via camera right/up extracted from the view matrix
//   * per-sprite uniforms applied in a tight CPU loop (N is small; proper
//     instancing isn't worth the complexity until we have hundreds of sprites)
// -----------------------------------------------------------------------------

#include "sprite.h"
#include "render_config.h"
#include "stb_image.h"

#include "generated/sprite.glsl.h"
#include "generated/sprite_spot.glsl.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

// Sidecar loader lives in sprite_light_editor.cpp so the editor and the
// asset loader share one parser. Forward-declared here to avoid pulling
// the UI header into core sprite code.
namespace sprite_light_editor {
bool load_lights_sidecar(const std::string& sprite_base_path,
                         std::vector<LightSpot>& out);
}

// ---------------------------------------------------------------------------
// SpriteArt — PNG loading (hull + optional lights sibling)
// ---------------------------------------------------------------------------

// Loads a PNG at `path` into `slot`. Returns (w,h) via out params, or (0,0)
// if the file is missing / can't be decoded. Mirrors load_texture_png in
// material.cpp but records pixel dims so callers can compute aspect.
static bool load_png_slot(const std::string& path, TextureSlot& slot,
                          int& out_w, int& out_h) {
    int w = 0, h = 0, c = 0;
    uint8_t* px = stbi_load(path.c_str(), &w, &h, &c, 4);
    if (!px) {
        out_w = out_h = 0;
        return false;
    }

    sg_image_desc id{};
    id.width  = w;
    id.height = h;
    id.pixel_format = SG_PIXELFORMAT_RGBA8;
    id.data.mip_levels[0] = { px, (size_t)(w * h * 4) };
    slot.image = sg_make_image(&id);

    sg_view_desc vd{};
    vd.texture.image = slot.image;
    slot.view = sg_make_view(&vd);

    stbi_image_free(px);
    slot.valid = (sg_query_image_state(slot.image) == SG_RESOURCESTATE_VALID);
    out_w = w;
    out_h = h;
    return slot.valid;
}

bool load_sprite_art(const std::string& base_path, SpriteArt& art) {
    art.name = base_path;
    const std::string hull_path   = base_path + ".png";
    const std::string lights_path = base_path + "_lights.png";

    int lw = 0, lh = 0;
    if (!load_png_slot(hull_path, art.hull, art.hull_w, art.hull_h)) {
        std::fprintf(stderr, "[sprite] failed to load hull '%s'\n",
                     hull_path.c_str());
        return false;
    }
    // Lights layer is optional. Missing = the sprite has no emissive overlay.
    if (!load_png_slot(lights_path, art.lights, lw, lh)) {
        std::fprintf(stderr, "[sprite]   (no lights layer at '%s' — ok)\n",
                     lights_path.c_str());
    } else if (lw != art.hull_w || lh != art.hull_h) {
        std::fprintf(stderr,
            "[sprite] WARNING: '%s' hull is %dx%d but lights is %dx%d — "
            "they should match for correct overlay alignment\n",
            base_path.c_str(), art.hull_w, art.hull_h, lw, lh);
    }
    // Animated UV-space lights are also loaded here so every consumer of
    // SpriteArt (placed sprites, ship-sprite atlas frames, future systems)
    // gets emissive blinky-blinky for free without each call site
    // remembering to wire the sidecar separately.
    sprite_light_editor::load_lights_sidecar(base_path, art.light_spots);

    std::printf("[sprite] loaded '%s'  hull=%dx%d  lights=%s  spots=%zu\n",
                base_path.c_str(), art.hull_w, art.hull_h,
                art.lights.valid ? "yes" : "no",
                art.light_spots.size());
    return true;
}

void SpriteArt::destroy() {
    auto free_slot = [](TextureSlot& s) {
        if (s.valid) {
            sg_destroy_view(s.view);
            sg_destroy_image(s.image);
            s.valid = false;
        }
    };
    free_slot(hull);
    free_slot(lights);
}

// ---------------------------------------------------------------------------
// SpriteRenderer — shared GPU state + two draw passes
// ---------------------------------------------------------------------------

// Unit quad: two triangles, 4 unique verts. Each vert carries a corner
// position in [-1,+1]^2 (used to expand along camera right/up in the
// vertex shader) and a UV in [0,1].
//
// UV convention: v=0 at the BOTTOM of the quad, v=1 at the TOP. This is
// the opposite of stb_image's top-left origin, and it's deliberate —
// our billboards use camera-up (+Y in world) as the quad's up axis. On
// screen, +camera-up projects to +screen-up (y_ndc = +1 at top). The
// texture sampler gets (u,v) as-is with no flip, so for a PNG loaded
// top-down by stb_image, v=0 must correspond to the visual TOP of the
// image — which means the screen-top corner of the quad must carry v=0
// but our corner layout has the screen-top corner at cy=+1. Hence: the
// mapping (cy=+1 → v=1, cy=-1 → v=0) paired with stb's loading order
// gives "PNG top renders at screen top" after accounting for the way
// sokol uploads image data (row 0 of the data → sampler's v=1 end on
// this pipeline, as verified empirically in Troy / Crimson Veil / Hadrian's
// Gate — before this flip the station rendered upside-down).
//
//   (-1,+1) uv(0,1)  +--------+  (+1,+1) uv(1,1)
//                    |        |
//                    |        |
//   (-1,-1) uv(0,0)  +--------+  (+1,-1) uv(1,0)
//
struct QuadVert { float cx, cy, u, v; };
static const QuadVert kQuad[4] = {
    { -1.0f, -1.0f, 0.0f, 0.0f },   // bottom-left
    {  1.0f, -1.0f, 1.0f, 0.0f },   // bottom-right
    {  1.0f,  1.0f, 1.0f, 1.0f },   // top-right
    { -1.0f,  1.0f, 0.0f, 1.0f },   // top-left
};
static const uint16_t kIndices[6] = { 0, 1, 2,  0, 2, 3 };

bool SpriteRenderer::init() {
    // --- geometry ---------------------------------------------------------
    sg_buffer_desc vbd{};
    vbd.data = SG_RANGE(kQuad);
    vbuf = sg_make_buffer(&vbd);

    sg_buffer_desc ibd{};
    ibd.usage.index_buffer = true;
    ibd.data = SG_RANGE(kIndices);
    ibuf = sg_make_buffer(&ibd);

    // --- shader -----------------------------------------------------------
    shader = sg_make_shader(sprite_shader_desc(sg_query_backend()));

    // --- sampler ----------------------------------------------------------
    // Linear filtering for both mag and min. Pixel-art sprites at 512² that
    // get downscaled for distant views need LINEAR to avoid shimmering. If
    // we ever want the crunchy "every sprite pixel is a block" look we can
    // switch mag_filter to NEAREST here.
    sg_sampler_desc sd{};
    sd.min_filter = SG_FILTER_LINEAR;
    sd.mag_filter = SG_FILTER_LINEAR;
    // We don't generate mipmaps for sprites yet (512² is small enough that
    // the GPU runtime cost is negligible). Use NEAREST mipmap filter as the
    // "no mipmap" signal — sokol has no explicit FILTER_NONE.
    sd.mipmap_filter = SG_FILTER_NEAREST;
    sd.wrap_u = SG_WRAP_CLAMP_TO_EDGE;
    sd.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
    sampler = sg_make_sampler(&sd);

    // --- pipelines --------------------------------------------------------
    // Shared layout + shader; the two pipelines differ ONLY in blend state
    // and depth write, so we build a base and stamp out two variants.
    auto make_pipeline = [&](bool additive) {
        sg_pipeline_desc pd{};
        pd.shader = shader;
        pd.layout.attrs[ATTR_sprite_a_corner].format = SG_VERTEXFORMAT_FLOAT2;
        pd.layout.attrs[ATTR_sprite_a_uv].format     = SG_VERTEXFORMAT_FLOAT2;
        pd.index_type = SG_INDEXTYPE_UINT16;

        pd.colors[0].blend.enabled = true;
        if (additive) {
            // Additive: src*src.a + dst*1. Lights glow without darkening.
            pd.colors[0].blend.src_factor_rgb   = SG_BLENDFACTOR_SRC_ALPHA;
            pd.colors[0].blend.dst_factor_rgb   = SG_BLENDFACTOR_ONE;
            pd.colors[0].blend.src_factor_alpha = SG_BLENDFACTOR_ONE;
            pd.colors[0].blend.dst_factor_alpha = SG_BLENDFACTOR_ONE;
        } else {
            // Standard non-premultiplied alpha blend.
            pd.colors[0].blend.src_factor_rgb   = SG_BLENDFACTOR_SRC_ALPHA;
            pd.colors[0].blend.dst_factor_rgb   = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            pd.colors[0].blend.src_factor_alpha = SG_BLENDFACTOR_ONE;
            pd.colors[0].blend.dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        }

        pd.depth.compare       = SG_COMPAREFUNC_LESS_EQUAL;
        // Hull writes depth so lights layer depth-tests against it correctly,
        // AND so any opaque geometry drawn AFTER sprites is properly occluded
        // by near-camera sprites. Lights pass does NOT write depth — additive
        // overlays shouldn't mask anything behind them.
        pd.depth.write_enabled = !additive;

        pd.colors[0].pixel_format = kSceneColorFormat;
        pd.depth.pixel_format     = kSceneDepthFormat;
        pd.sample_count           = kSceneSampleCount;
        pd.cull_mode              = SG_CULLMODE_NONE;
        return sg_make_pipeline(&pd);
    };

    pipeline_hull   = make_pipeline(/*additive=*/false);
    pipeline_lights = make_pipeline(/*additive=*/true);

    // --- animated-spot pipeline ------------------------------------------
    // Separate shader (sprite_spot.glsl) with its own vertex layout: only
    // the corner attribute — no UV is needed since the fragment shader
    // computes the radial distance procedurally. Reuses our unit quad VBO
    // via the first 2 floats of each vertex; the UV bytes are ignored.
    spot_shader = sg_make_shader(sprite_spot_shader_desc(sg_query_backend()));

    sg_pipeline_desc spd{};
    spd.shader = spot_shader;
    spd.layout.attrs[ATTR_sprite_spot_a_corner].format = SG_VERTEXFORMAT_FLOAT2;
    spd.layout.buffers[0].stride = sizeof(QuadVert);   // skip uv bytes
    spd.index_type = SG_INDEXTYPE_UINT16;

    // Additive, no depth write — same logic as the lights pass: the glow
    // shouldn't darken anything behind it, and overlapping spots should
    // summate commutatively.
    spd.colors[0].blend.enabled         = true;
    spd.colors[0].blend.src_factor_rgb  = SG_BLENDFACTOR_SRC_ALPHA;
    spd.colors[0].blend.dst_factor_rgb  = SG_BLENDFACTOR_ONE;
    spd.colors[0].blend.src_factor_alpha = SG_BLENDFACTOR_ONE;
    spd.colors[0].blend.dst_factor_alpha = SG_BLENDFACTOR_ONE;
    spd.depth.compare          = SG_COMPAREFUNC_LESS_EQUAL;
    spd.depth.write_enabled    = false;
    spd.colors[0].pixel_format = kSceneColorFormat;
    spd.depth.pixel_format     = kSceneDepthFormat;
    spd.sample_count           = kSceneSampleCount;
    spd.cull_mode              = SG_CULLMODE_NONE;
    spot_pipeline = sg_make_pipeline(&spd);

    const bool ok = sg_query_pipeline_state(pipeline_hull)   == SG_RESOURCESTATE_VALID
                 && sg_query_pipeline_state(pipeline_lights) == SG_RESOURCESTATE_VALID
                 && sg_query_pipeline_state(spot_pipeline)   == SG_RESOURCESTATE_VALID;
    if (!ok) {
        std::fprintf(stderr, "[sprite] pipeline creation failed\n");
    }
    return ok;
}

void SpriteRenderer::destroy() {
    sg_destroy_pipeline(spot_pipeline);
    sg_destroy_shader(spot_shader);
    sg_destroy_pipeline(pipeline_hull);
    sg_destroy_pipeline(pipeline_lights);
    sg_destroy_sampler(sampler);
    sg_destroy_shader(shader);
    sg_destroy_buffer(ibuf);
    sg_destroy_buffer(vbuf);
}

// Time-dependent brightness in [0,1] for a light. Pure function of time
// and the light's kind/hz/phase — no hidden state. Intentionally lives in
// the renderer (not the LightSpot struct) because the shape of "animation
// curve" is a rendering concern; the LightSpot is pure data the editor can
// round-trip through JSON without needing to know about time.
static float light_intensity(const LightSpot& s, float t) {
    const float two_pi = 6.28318530718f;
    switch (s.kind) {
    case LightKind::Steady:
        return 1.0f;
    case LightKind::Pulse: {
        // 0.5..1.0 sinusoid — never fully off, feels like a slow "breathing"
        // cabin window. Using a biased cosine so phase=0 starts bright.
        const float a = 0.75f + 0.25f * std::cos(two_pi * (t * s.hz + s.phase));
        return a;
    }
    case LightKind::Strobe: {
        // Short bright flash per cycle, 10% duty. fract() via fmod keeps us
        // in [0,1); any frac < 0.1 is "on". Phase stagger lets multiple
        // strobes on the same base chase each other.
        const float u = std::fmod(t * s.hz + s.phase, 1.0f);
        return (u < 0.1f) ? 1.0f : 0.0f;
    }
    }
    return 1.0f;
}

// Extract world-space right and up vectors from a view matrix. HMM stores
// matrices column-major (OpenGL convention): column i is the transform of
// the i-th basis axis. The VIEW matrix transforms world → camera, so its
// ROWS are the camera's basis vectors expressed in world space. Row 0 is
// camera-right, row 1 is camera-up.
static void camera_basis(const HMM_Mat4& view, HMM_Vec3& right, HMM_Vec3& up) {
    // HMM stores column-major: M.Columns[col].Elements[row]. Row r, col c
    // is view.Columns[c].Elements[r]. To pull the first row:
    right = HMM_V3(view.Elements[0][0], view.Elements[1][0], view.Elements[2][0]);
    up    = HMM_V3(view.Elements[0][1], view.Elements[1][1], view.Elements[2][1]);
}

void SpriteRenderer::draw(const std::vector<SpriteObject>& sprites,
                          const Camera& cam,
                          float aspect,
                          float time_sec) const {
    if (sprites.empty()) return;

    // Shared per-frame state ------------------------------------------------
    const HMM_Mat4 view = cam.view();
    const HMM_Mat4 vp   = HMM_MulM4(cam.projection(aspect), view);

    HMM_Vec3 cam_right, cam_up;
    camera_basis(view, cam_right, cam_up);

    // Back-to-front sort so alpha-blend composites correctly when sprites
    // overlap. We sort by squared distance so we don't pay for sqrt; the
    // ordering is the same.
    std::vector<const SpriteObject*> order;
    order.reserve(sprites.size());
    for (const auto& s : sprites) order.push_back(&s);
    std::sort(order.begin(), order.end(),
              [&](const SpriteObject* a, const SpriteObject* b) {
                  const HMM_Vec3 da = HMM_SubV3(a->position, cam.position);
                  const HMM_Vec3 db = HMM_SubV3(b->position, cam.position);
                  return HMM_DotV3(da, da) > HMM_DotV3(db, db);
              });

    sg_bindings b{};
    b.vertex_buffers[0] = vbuf;
    b.index_buffer      = ibuf;
    b.samplers[SMP_u_smp] = sampler;

    // Inner: apply a pipeline and draw every sprite's chosen layer ---------
    auto draw_layer = [&](sg_pipeline pipe, bool lights_layer) {
        sg_apply_pipeline(pipe);
        for (const SpriteObject* s : order) {
            if (!s->art) continue;
            const TextureSlot& slot = lights_layer ? s->art->lights : s->art->hull;
            if (!slot.valid) continue;

            // Non-square textures: the longest side covers `world_size`,
            // the shorter side is scaled proportionally so pixels stay
            // square. Most of our art is 512x512 so both scales are 1.0.
            const int tw = s->art->hull_w, th = s->art->hull_h;
            const float longest = (float)std::max(tw, th);
            const float sx = (float)tw / longest;
            const float sy = (float)th / longest;

            vs_params_t vsp{};
            std::memcpy(vsp.view_proj, &vp, sizeof(float) * 16);
            vsp.cam_right[0] = cam_right.X; vsp.cam_right[1] = cam_right.Y;
            vsp.cam_right[2] = cam_right.Z; vsp.cam_right[3] = 0.0f;
            vsp.cam_up[0]    = cam_up.X;    vsp.cam_up[1]    = cam_up.Y;
            vsp.cam_up[2]    = cam_up.Z;    vsp.cam_up[3]    = 0.0f;
            vsp.inst_pos[0]  = s->position.X;
            vsp.inst_pos[1]  = s->position.Y;
            vsp.inst_pos[2]  = s->position.Z;
            vsp.inst_pos[3]  = s->world_size * 0.5f;   // half-extent
            vsp.inst_scale[0] = sx;
            vsp.inst_scale[1] = sy;
            vsp.inst_scale[2] = std::cos(s->roll_rad);
            vsp.inst_scale[3] = std::sin(s->roll_rad);

            fs_params_t fsp{};
            fsp.tint[0] = s->tint.X; fsp.tint[1] = s->tint.Y;
            fsp.tint[2] = s->tint.Z; fsp.tint[3] = s->tint.W;

            b.views[VIEW_u_tex] = slot.view;
            sg_apply_bindings(&b);
            sg_apply_uniforms(UB_vs_params, SG_RANGE(vsp));
            sg_apply_uniforms(UB_fs_params, SG_RANGE(fsp));
            sg_draw(0, 6, 1);
        }
    };

    draw_layer(pipeline_hull,   /*lights_layer=*/false);
    draw_layer(pipeline_lights, /*lights_layer=*/true);

    // --- Third pass: animated light spots --------------------------------
    // One small additive quad per LightSpot on each sprite. Positioned by
    // projecting the light's UV (in sprite space, 0..1) into world space
    // using the sprite's billboard basis — same right/up vectors we used
    // to build the hull quad. UV (0,0) is the TOP-LEFT of the sprite, so
    // U maps to +right and V maps to -up (because up is "towards screen top").
    sg_apply_pipeline(spot_pipeline);
    sg_bindings bs{};
    bs.vertex_buffers[0] = vbuf;
    bs.index_buffer      = ibuf;
    sg_apply_bindings(&bs);

    for (const SpriteObject* s : order) {
        if (!s->art || s->lights.empty()) continue;

        // The sprite's half-extent along camera right/up — same math as
        // the hull quad. A light at UV (0.5, 0.5) sits at the sprite's
        // center; (0,0) is top-left; (1,1) is bottom-right.
        const int tw = s->art->hull_w, th = s->art->hull_h;
        const float longest = (float)std::max(tw, th);
        const float half_sz = s->world_size * 0.5f;
        const float half_u  = half_sz * (float)tw / longest;
        const float half_v  = half_sz * (float)th / longest;

        // Z-fighting prevention is handled in the vertex shader via a
        // clip-space depth bias (see sprite_spot.glsl). That's precision-
        // invariant — world-space biases don't work at typical sprite
        // distances because the depth buffer's ULP floor is too coarse.
        for (const LightSpot& ls : s->lights) {
            const float intensity = light_intensity(ls, time_sec);
            if (intensity <= 0.0f) continue;   // skip fully-off strobes

            // UV → world offset from sprite center. U=0.5 / V=0.5 is the
            // center; (0,0) is the sprite's top-LEFT; (1,1) is bottom-RIGHT,
            // matching the F2 editor's storage convention which records
            // `ls.v = mouse_uv.y` from ImGui's top-down click coordinate.
            //
            // V flip: the editor's gizmo at v=0 sits at the TOP of the
            // displayed image (where it intuitively belongs — that's where
            // the user clicked). To make the rendered spot land on the
            // SAME screen pixel the gizmo was drawn on, we must offset in
            // the -cam_up direction when v < 0.5 (i.e. screen-down for the
            // top half of the image). Hence `(0.5 - v)` — the previous
            // `(v - 0.5)` formula painted every light upside-down vs the
            // editor, which only showed up clearly with ship-atlas cells
            // where Mike could easily compare gizmo and rendered position
            // on the same hull feature.
            const float du = (ls.u - 0.5f) * 2.0f * half_u;
            const float dv = (0.5f - ls.v) * 2.0f * half_v;
            // Apply the SAME in-plane roll the hull quad uses (see
            // sprite.glsl:42-46). Without this, light spots stay locked
            // to camera right/up while the hull rotates underneath them,
            // producing the "red dots float off-ship as Tarsus banks"
            // bug. Rotation matrix matches the shader's exactly:
            //   corner = (cx*c - cy*s,  cx*s + cy*c)
            const float roll_c = std::cos(s->roll_rad);
            const float roll_s = std::sin(s->roll_rad);
            const float du_r = du * roll_c - dv * roll_s;
            const float dv_r = du * roll_s + dv * roll_c;
            const HMM_Vec3 world = HMM_AddV3(
                s->position,
                HMM_AddV3(HMM_MulV3F(cam_right, du_r),
                          HMM_MulV3F(cam_up,    dv_r)));

            vs_spot_params_t vsp{};
            std::memcpy(vsp.view_proj, &vp, sizeof(float) * 16);
            vsp.cam_right[0] = cam_right.X; vsp.cam_right[1] = cam_right.Y;
            vsp.cam_right[2] = cam_right.Z; vsp.cam_right[3] = 0.0f;
            vsp.cam_up[0]    = cam_up.X;    vsp.cam_up[1]    = cam_up.Y;
            vsp.cam_up[2]    = cam_up.Z;    vsp.cam_up[3]    = 0.0f;
            vsp.spot_pos[0]  = world.X;
            vsp.spot_pos[1]  = world.Y;
            vsp.spot_pos[2]  = world.Z;
            vsp.spot_pos[3]  = ls.size;   // world-unit radius

            fs_spot_params_t fsp{};
            fsp.color_and_intensity[0] = ls.color.X;
            fsp.color_and_intensity[1] = ls.color.Y;
            fsp.color_and_intensity[2] = ls.color.Z;
            fsp.color_and_intensity[3] = intensity * s->tint.W;  // global sprite alpha fades lights too

            sg_apply_uniforms(UB_vs_spot_params, SG_RANGE(vsp));
            sg_apply_uniforms(UB_fs_spot_params, SG_RANGE(fsp));
            sg_draw(0, 6, 1);
        }
    }
}

// ----------------------------------------------------------------------------
// Tracer rendering — same pipeline + shader as the sprite "spot" pass, just
// without the UV-into-billboard projection. Each tracer is one additive glow
// quad at a free-standing world position. Used for projectiles (firing.cpp,
// projectile.cpp); the spot pipeline already does additive blending and
// vertex-shader depth bias, so the tracers composite over hulls cleanly.
// ----------------------------------------------------------------------------

void SpriteRenderer::draw_tracers(const std::vector<Tracer>& tracers,
                                  const Camera& cam,
                                  float aspect) const {
    if (tracers.empty()) return;

    const HMM_Mat4 view = cam.view();
    const HMM_Mat4 vp   = HMM_MulM4(cam.projection(aspect), view);
    HMM_Vec3 cam_right, cam_up;
    camera_basis(view, cam_right, cam_up);

    sg_apply_pipeline(spot_pipeline);
    sg_bindings bs{};
    bs.vertex_buffers[0] = vbuf;
    bs.index_buffer      = ibuf;
    sg_apply_bindings(&bs);

    for (const Tracer& t : tracers) {
        vs_spot_params_t vsp{};
        std::memcpy(vsp.view_proj, &vp, sizeof(float) * 16);
        vsp.cam_right[0] = cam_right.X; vsp.cam_right[1] = cam_right.Y;
        vsp.cam_right[2] = cam_right.Z; vsp.cam_right[3] = 0.0f;
        vsp.cam_up[0]    = cam_up.X;    vsp.cam_up[1]    = cam_up.Y;
        vsp.cam_up[2]    = cam_up.Z;    vsp.cam_up[3]    = 0.0f;
        vsp.spot_pos[0]  = t.position.X;
        vsp.spot_pos[1]  = t.position.Y;
        vsp.spot_pos[2]  = t.position.Z;
        vsp.spot_pos[3]  = t.size;

        fs_spot_params_t fsp{};
        fsp.color_and_intensity[0] = t.color.X;
        fsp.color_and_intensity[1] = t.color.Y;
        fsp.color_and_intensity[2] = t.color.Z;
        fsp.color_and_intensity[3] = 1.0f;   // tracers always full intensity

        sg_apply_uniforms(UB_vs_spot_params, SG_RANGE(vsp));
        sg_apply_uniforms(UB_fs_spot_params, SG_RANGE(fsp));
        sg_draw(0, 6, 1);
    }
}

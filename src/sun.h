#pragma once
// -----------------------------------------------------------------------------
// sun.h — one self-illuminated star at a world position.
//
// Builds a UV sphere at init, renders it with a fresnel-glow shader, then
// overlays a camera-facing billboard for the corona + god rays. Exposes
// color + direction queries so future lit geometry can treat this as a
// point light source.
//
// Corona implementation lives in sun_corona.cpp so its shader header stays
// in its own TU (avoids sokol-shdc's static-symbol collisions).
// -----------------------------------------------------------------------------

#include "sokol_gfx.h"
#include "HandmadeMath.h"
#include "camera.h"

struct Sun {
    // ---- world placement --------------------------------------------------
    // Scale note: we're using 1 unit ≈ 1 metre. Real stars are huge (Sol
    // ≈ 7e8 m radius) so a faithful sim would be unwieldy; game scale
    // here is "the star looks right from a few tens of km out" rather
    // than physical. 2500 u looks great at the 30 km starting distance.
    HMM_Vec3 position{0.0f, 0.0f, 0.0f};
    float    radius = 2500.0f;

    // ---- surface appearance ----------------------------------------------
    HMM_Vec3 core_color{1.00f, 0.95f, 0.85f};   // near-white hot plasma
    HMM_Vec3 glow_color{1.00f, 0.55f, 0.20f};   // warm limb + corona
    float    rim_tightness = 1.6f;              // lower = fatter limb bloom

    // ---- plasma surface parameters (animated noise granulation) ----------
    float granule_scale   = 4.0f;    // noise cycles across the sphere
    float plasma_flow     = 0.06f;   // noise scroll speed (units / s)
    float plasma_contrast = 0.85f;   // 0..1, how hot/cold the cells get

    // ---- gas shell (wisps hugging the silhouette) ------------------------
    // Drawn AFTER the sphere, additively. Billboard diameter is
    // radius * gas_radius_mult; the density shell peaks at gas_ring_center
    // (fractional radius of the billboard) with half-width gas_ring_width.
    float gas_radius_mult = 2.8f;
    float gas_strength    = 0.75f;
    float gas_noise_scale = 3.0f;
    float gas_ring_center = 0.55f;
    float gas_ring_width  = 0.35f;

    // ---- corona / god-ray parameters -------------------------------------
    // Defaults tuned for Freelancer-style atmospheric glow: a big, soft
    // halo with a blown-out white center and only the faintest hint of
    // diffraction streaks. Crank ray_strength / ray_sharpness for the
    // "anime starburst" look, or set ray_strength = 0 for a pure bloom.
    float corona_radius_mult = 18.0f;   // billboard size = radius * this
    float corona_alpha       = 0.75f;   // overall glow strength
    float ray_count          = 4.0f;    // 4 = subtle cross; 0 = none
    float ray_sharpness      = 4.0f;    // low = fat/soft, high = thin/harsh
    float ray_strength       = 0.12f;   // keep this low for subtlety
    float ray_phase          = 0.0f;    // rotate the ray star if you like

    // ---- solid sphere GPU resources (set by Sun::init) -------------------
    sg_buffer   vbuf{};
    sg_buffer   ibuf{};
    sg_shader   shader{};
    sg_pipeline pipeline{};
    int         index_count = 0;

    // ---- corona billboard GPU resources (set by sun_corona_init) --------
    sg_buffer   corona_vbuf{};
    sg_shader   corona_shader{};
    sg_pipeline corona_pipeline{};

    // ---- gas-shell billboard GPU resources (set by sun_gas_init) --------
    sg_buffer   gas_vbuf{};
    sg_shader   gas_shader{};
    sg_pipeline gas_pipeline{};

    // ---- lifecycle --------------------------------------------------------
    bool init(int lat_segments = 32, int lon_segments = 48);
    // `time_sec` drives the plasma animation — pass stm_sec(stm_now()) or
    // any monotonically-increasing seconds value.
    void draw(const Camera& cam, float aspect, float time_sec) const;
    void destroy();

    // ---- light-source queries for later lit geometry --------------------
    HMM_Vec3 light_dir_to(HMM_Vec3 world_pos) const;   // normalized, point -> sun
    HMM_Vec3 light_color() const { return core_color; }
};

// Corona pass — defined in sun_corona.cpp, called from Sun::draw().
bool sun_corona_init(Sun& s);
void sun_corona_draw(const Sun& s, const Camera& cam, const HMM_Mat4& vp);
void sun_corona_destroy(Sun& s);

// Gas-shell pass — defined in sun_gas.cpp, called from Sun::draw().
bool sun_gas_init(Sun& s);
void sun_gas_draw(const Sun& s, const Camera& cam, const HMM_Mat4& vp, float time_sec);
void sun_gas_destroy(Sun& s);

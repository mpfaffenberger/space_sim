// -----------------------------------------------------------------------------
// planet_texture.cpp — fractal-noise planet generator.
//
// Noise:   3-octave value noise in 3D (sampled on the unit sphere so the
//          texture wraps seamlessly horizontally and has no poles artefact).
// Palette: lookup table keyed off normalised noise value + latitude.
//
// Cheap by construction — a 1024×512 texture is 512k samples, each touching
// at most ~24 hash evaluations. Generates in ~50 ms on a laptop.
// -----------------------------------------------------------------------------

#include "planet_texture.h"
#include "sokol_time.h"    // stm_now / stm_since / stm_ms — for gen timing

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

// ---- tiny deterministic hash-based value noise -------------------------
// 3D → float in [0,1]. Not Perlin, not simplex — just a hashed lattice
// value + smoothstep interpolation. Good enough for a distant planet.
uint32_t hash3(int x, int y, int z) {
    uint32_t h = (uint32_t)x * 0x8da6b343u
               ^ (uint32_t)y * 0xd8163841u
               ^ (uint32_t)z * 0xcb1ab31fu;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}
float lattice(int x, int y, int z) {
    return (hash3(x, y, z) & 0xFFFFFF) / float(0xFFFFFF);   // [0,1]
}
float smooth(float t) { return t * t * (3.0f - 2.0f * t); } // smoothstep

float value_noise_3d(float x, float y, float z) {
    const int xi = (int)std::floor(x), yi = (int)std::floor(y), zi = (int)std::floor(z);
    const float fx = smooth(x - xi), fy = smooth(y - yi), fz = smooth(z - zi);

    // Trilinear lerp over 8 lattice corners.
    auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
    float c000 = lattice(xi,     yi,     zi);
    float c100 = lattice(xi + 1, yi,     zi);
    float c010 = lattice(xi,     yi + 1, zi);
    float c110 = lattice(xi + 1, yi + 1, zi);
    float c001 = lattice(xi,     yi,     zi + 1);
    float c101 = lattice(xi + 1, yi,     zi + 1);
    float c011 = lattice(xi,     yi + 1, zi + 1);
    float c111 = lattice(xi + 1, yi + 1, zi + 1);
    float x00 = lerp(c000, c100, fx);
    float x10 = lerp(c010, c110, fx);
    float x01 = lerp(c001, c101, fx);
    float x11 = lerp(c011, c111, fx);
    float y0  = lerp(x00,  x10,  fy);
    float y1  = lerp(x01,  x11,  fy);
    return lerp(y0, y1, fz);
}

// Fractional Brownian Motion — stack octaves with halving amplitude.
// 10 octaves cover "continents → peninsulas → bays → coastlines → rocks
// → shoreline fuzz → pixel grain → sub-pixel dither". Each octave
// doubles the spatial frequency, so octave N resolves features ~1/(2^N)
// of the planet wide. At 4096×2048 the equator is only 4096 px wide,
// so octaves 8-10 (freq 128-512) are the ones that finally make high-res
// actually LOOK high-res — any fewer and the extra pixels just re-sample
// the same 7-octave function at denser spacing. Each added octave is
// ~15% more FBM work; 10 is the sweet spot for 4K.
float fbm(float x, float y, float z) {
    float sum = 0.0f, amp = 1.0f, freq = 1.0f, norm = 0.0f;
    for (int i = 0; i < 10; ++i) {
        sum  += value_noise_3d(x * freq, y * freq, z * freq) * amp;
        norm += amp;
        amp  *= 0.52f;   // slightly above 0.5 = a touch more fine-detail
        freq *= 2.0f;
    }
    return sum / norm;   // back to [0,1]
}

// ---- Agricultural planet palette ---------------------------------------
// Warm greens for fertile continents, a muted ocean-teal, and icy poles.
// Values hand-picked to look alive without being saturated neon.
struct RGB { uint8_t r, g, b; };
RGB lerp_rgb(RGB a, RGB b, float t) {
    return {
        (uint8_t)(a.r + (b.r - a.r) * t),
        (uint8_t)(a.g + (b.g - a.g) * t),
        (uint8_t)(a.b + (b.b - a.b) * t),
    };
}

RGB agri_palette(float n, float lat_abs) {
    // Polar caps — blend to white near |lat| = 1.0.
    const float polar = std::clamp((lat_abs - 0.82f) / 0.14f, 0.0f, 1.0f);

    const RGB deep_ocean  = {  18,  48,  95 };   // dark blue
    const RGB ocean       = {  35,  82, 140 };   // open sea
    const RGB shallow     = {  65, 135, 175 };   // teal shallows
    const RGB beach       = { 200, 185, 130 };   // sandy coast
    const RGB grass_lt    = { 120, 170,  75 };   // sunlit fields
    const RGB grass       = {  75, 135,  55 };   // fields
    const RGB forest      = {  36,  88,  40 };   // darker vegetation
    const RGB mountain    = {  95,  80,  60 };   // exposed earth/rock
    const RGB ice         = { 238, 242, 248 };   // polar cap

    // Narrower transition bands = crisper coastlines and region
    // boundaries. The extra palette stops (ocean, grass_lt, mountain)
    // add mid-tone texture instead of a flat green/blue wash.
    RGB base;
    if      (n < 0.30f) base = lerp_rgb(deep_ocean, ocean,  n / 0.30f);
    else if (n < 0.44f) base = lerp_rgb(ocean,      shallow,(n - 0.30f) / 0.14f);
    else if (n < 0.46f) base = lerp_rgb(shallow,    beach,  (n - 0.44f) / 0.02f);
    else if (n < 0.52f) base = lerp_rgb(beach,      grass_lt,(n - 0.46f) / 0.06f);
    else if (n < 0.62f) base = lerp_rgb(grass_lt,   grass,  (n - 0.52f) / 0.10f);
    else if (n < 0.78f) base = lerp_rgb(grass,      forest, (n - 0.62f) / 0.16f);
    else                base = lerp_rgb(forest,    mountain, std::min(1.0f, (n - 0.78f) / 0.15f));

    return lerp_rgb(base, ice, polar);
}

} // namespace

void PlanetTexture::destroy() {
    if (view.id)  sg_destroy_view(view);
    if (image.id) sg_destroy_image(image);
    *this = PlanetTexture{};
}

// Mipmap chain builder — 2×2 box-filter downsample, RGBA8 → RGBA8.
// Simple and correct; nobody's going to notice a better kernel on
// noise-generated planet texture. Returns pyramid[0..num_mips-1]
// where [0] is the source and [num_mips-1] is a 1×1 average.
static std::vector<std::vector<uint8_t>>
build_mip_chain(const std::vector<uint8_t>& base, int w, int h) {
    std::vector<std::vector<uint8_t>> chain;
    chain.emplace_back(base);           // level 0 owns its own copy
    int cw = w, ch = h;
    while (cw > 1 || ch > 1) {
        const int nw = std::max(1, cw / 2);
        const int nh = std::max(1, ch / 2);
        std::vector<uint8_t> dst((size_t)nw * nh * 4);
        const auto& src = chain.back();
        for (int y = 0; y < nh; ++y) {
            // Handle odd dimensions — clamp the second tap to edge.
            const int y0 = y * 2;
            const int y1 = std::min(y0 + 1, ch - 1);
            for (int x = 0; x < nw; ++x) {
                const int x0 = x * 2;
                const int x1 = std::min(x0 + 1, cw - 1);
                const uint8_t* p00 = &src[(size_t)(y0 * cw + x0) * 4];
                const uint8_t* p01 = &src[(size_t)(y0 * cw + x1) * 4];
                const uint8_t* p10 = &src[(size_t)(y1 * cw + x0) * 4];
                const uint8_t* p11 = &src[(size_t)(y1 * cw + x1) * 4];
                uint8_t* d = &dst[(size_t)(y * nw + x) * 4];
                for (int c = 0; c < 4; ++c) {
                    d[c] = (uint8_t)((p00[c] + p01[c] + p10[c] + p11[c] + 2) >> 2);
                }
            }
        }
        chain.emplace_back(std::move(dst));
        cw = nw;
        ch = nh;
    }
    return chain;
}

// Default resolution bumped to 4096×2048 (32 MB at mip 0, ~42 MB with
// full mipmap chain). With the sampler already configured for trilinear
// filtering, distant views anti-alias cleanly while close approach
// reveals per-pixel continent edges. This is the first knob to dial
// back if startup time or VRAM ever become problems — the generator
// is O(w·h·octaves) and scales linearly with pixel count.
PlanetTexture make_planet_texture(const std::string& preset, int w, int h) {
    const uint64_t t_start = stm_now();
    PlanetTexture t;
    t.width  = w;
    t.height = h;

    // Sample noise on the unit sphere so the texture is seamless across
    // the ±180° UV seam. (u=0 and u=1 map to the same 3D point.)
    std::vector<uint8_t> px(w * h * 4);
    for (int y = 0; y < h; ++y) {
        const float v   = (y + 0.5f) / (float)h;            // [0,1]
        const float lat = (v - 0.5f) * 3.14159265f;          // [-π/2, π/2]
        const float cy  = std::cos(lat);
        const float sy  = std::sin(lat);
        for (int x = 0; x < w; ++x) {
            const float u   = (x + 0.5f) / (float)w;
            const float lon = (u - 0.5f) * 6.28318530f;      // [-π, π]
            // Sphere-coord radius controls the scale of the largest
            // features. 3.0 gives a handful of continents at 2048 wide;
            // bigger → more, smaller continents (Pangaea vs archipelago).
            const float R = 3.0f;
            const float sx  = std::cos(lon) * cy * R;
            const float sz  = std::sin(lon) * cy * R;
            const float sy3 = sy * R;

            float n = fbm(sx + 11.3f, sy3 + 7.7f, sz + 3.1f);

            // High-frequency detail mixed in at low weight — breaks up
            // the "clean procedural" feel without shifting continents.
            // Sampled at 6× the base frequency with a different offset
            // so it can't interfere constructively with the base FBM.
            float detail = value_noise_3d(sx * 6.0f + 51.2f,
                                          sy3 * 6.0f - 17.8f,
                                          sz * 6.0f +  9.4f);
            n = std::clamp(n + (detail - 0.5f) * 0.08f, 0.0f, 1.0f);

            // Slight latitude bias — cooler high latitudes look more
            // "north-hemisphere-continent" on an earthlike.
            n += (1.0f - std::fabs(sy)) * 0.02f;

            RGB c;
            (void)preset;  // only one preset today, but keep the param
            c = agri_palette(n, std::fabs(sy));

            const size_t k = (size_t)(y * w + x) * 4;
            px[k + 0] = c.r;
            px[k + 1] = c.g;
            px[k + 2] = c.b;
            px[k + 3] = 255;
        }
    }

    // Build the full mip pyramid on the CPU. Sokol doesn't auto-gen;
    // it just takes whatever we hand it. A 4096×2048 source produces
    // 13 mip levels down to 1×1.
    const auto mips = build_mip_chain(px, w, h);

    sg_image_desc id{};
    id.width        = w;
    id.height       = h;
    id.num_mipmaps  = (int)mips.size();
    id.pixel_format = SG_PIXELFORMAT_RGBA8;
    for (size_t i = 0; i < mips.size(); ++i) {
        id.data.mip_levels[i] = { mips[i].data(), mips[i].size() };
    }
    t.image = sg_make_image(&id);

    sg_view_desc vd{};
    vd.texture.image = t.image;
    t.view = sg_make_view(&vd);

    const double gen_ms = stm_ms(stm_since(t_start));
    std::printf("[planet_texture] generated '%s' %dx%d  mips=%d  (%.1f ms)\n",
                preset.c_str(), w, h, (int)mips.size(), gen_ms);
    return t;
}

#pragma once
// -----------------------------------------------------------------------------
// explosion.h — short-lived ship-death FX.
//
// One Explosion per kill. Lives for ~1.2 seconds, drawn each frame as
// TWO additive glow billboards (reusing the sprite spot pipeline that
// already handles tracers + light spots):
//
//   1. Flash   — small (~80 m), bright near-white, intensity decays
//                exponentially from t=0. Captures the \"electrical
//                discharge\" pop the moment armor cracks.
//   2. Shockwave — large (grows 50 -> ~500 m), orange-red, intensity
//                  fades linearly. Reads as the expanding fireball /
//                  pressure ring.
//
// Both layers stack additively in HDR (color values > 1.0 deliberately
// — the bloom post-process picks them up, which is what makes the FX
// pop on screen instead of looking like a flat shaded sphere).
//
// No real shader work: a billboard glow with a smart size + color
// curve over time gives most of the visual punch of a dedicated
// shockwave shader at zero pipeline cost. Upgrade to ring geometry +
// noise textures when v2 calls for it.
// -----------------------------------------------------------------------------

#include <HandmadeMath.h>
#include <vector>

struct Explosion {
    HMM_Vec3 position   { 0.0f, 0.0f, 0.0f };
    float    age_s      = 0.0f;
    float    lifetime_s = 1.2f;
    bool     alive      = true;

    // Disc-shockwave plane vectors. Captured from the camera at spawn
    // time and frozen — so the ring stays where it was relative to
    // world even if the camera rotates afterwards. ring_right and
    // ring_up are unit vectors spanning the disc plane; the renderer
    // places N point-glows around the ring at angle θ via
    //   pos = explosion + cos(θ) * ring_right * r + sin(θ) * ring_up * r
    HMM_Vec3 ring_right { 1.0f, 0.0f, 0.0f };
    HMM_Vec3 ring_up    { 0.0f, 1.0f, 0.0f };
};

namespace explosion {

// Spawn a fresh explosion. `ring_right` / `ring_up` define the disc
// plane — main.cpp passes camera.right() / camera.up() at the moment
// of death so the ring is face-on to the player at t=0 and stays in
// world-space afterwards. Caller passes the dying ship's world
// position (use sprite->position when available so the visual lines
// up with where the ship was last drawn).
void spawn(std::vector<Explosion>& explosions, const HMM_Vec3& pos,
           const HMM_Vec3& ring_right, const HMM_Vec3& ring_up);

// Advance every explosion by dt; prune any that exceeded lifetime.
// Same swap-remove pattern as projectile::tick — no stable indices
// needed because nothing else holds explosion references across frames.
void tick(std::vector<Explosion>& explosions, float dt);

} // namespace explosion

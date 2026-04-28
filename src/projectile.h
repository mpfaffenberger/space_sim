#pragma once
// -----------------------------------------------------------------------------
// projectile.h — gun-fired entities flying through the world.
//
// Each tick of `firing::tick` may spawn one or more Projectiles; each tick
// of `projectile::tick` advances them along their velocity, decrements
// remaining range, and marks any out-of-budget ones dead. The renderer
// draws every alive projectile as an additive billboard glow (reusing the
// sprite spot pipeline; see SpriteRenderer::draw_tracers).
//
// L4.1 scope (this commit): visible projectiles only — they pass through
// ships harmlessly. L4.2 will add hit-tests and damage.
//
// Design notes:
//   * No per-projectile orientation; we render each as a radial glow
//     (additive blob) which reads as a tracer at typical engagement
//     distances. Streak/line rendering would be nicer but is its own
//     pipeline; v1 prioritises minimum diff.
//   * range_remaining (m) decremented by |velocity * dt| each tick;
//     drops alive=false when ≤0. Cleaner than tracking a max-range
//     constant and computing distance from spawn each frame, and
//     handles inherited-from-shooter velocity correctly.
//   * owner_id lets the damage layer skip self-collisions and bill
//     damage to the right ship for kill credit / reputation deltas.
// -----------------------------------------------------------------------------

#include "gun.h"

#include <HandmadeMath.h>
#include <cstdint>
#include <vector>

struct Projectile {
    HMM_Vec3 position { 0, 0, 0 };
    HMM_Vec3 velocity { 0, 0, 0 };       // m/s, world frame
    float    damage_cm        = 0.0f;
    float    range_remaining  = 0.0f;    // m; decremented by speed*dt
    GunType  type             = GunType::Laser;
    uint32_t owner_id         = 0;       // ship id that fired
    bool     alive            = true;
};

struct Ship;

namespace projectile {

// Advance every alive projectile by dt and prune dead ones at the back of
// the vector via swap-remove. Stable indices aren't needed (nothing else
// holds projectile pointers across frames), so swap-remove keeps the
// container compact without touching surviving entries.
void tick(std::vector<Projectile>& projectiles, float dt);

// Collision pass: for each alive projectile, segment-test against every
// alive ship's hit sphere and apply damage on the first hit. Skips
// self-hits (projectile owner_id == ship.id) so a ship's own bullets
// can't kill it. The segment test uses (prev_pos -> current_pos) computed
// from velocity * dt — catches fast bullets that would otherwise pass
// THROUGH a ship in a single frame between two point-tests. dt is the
// same value passed to projectile::tick.
void collide_and_damage(std::vector<Projectile>& projectiles,
                        std::vector<Ship>&        ships,
                        float                     dt);

} // namespace projectile

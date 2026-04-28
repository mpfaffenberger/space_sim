#include "projectile.h"

#include "world_scale.h"

#include <cmath>

void projectile::tick(std::vector<Projectile>& projectiles, float dt) {
    // Forward integrate + range expire in one pass. Swap-remove dead
    // entries at the end so the alive set stays packed at [0, n) for
    // the next tick — keeps cache locality high and renderer iteration
    // simple (no `if (alive)` skips inside the hot draw loop).
    // Same global pacing scale as the camera + ship integrators (see
    // world_scale.h). Range counter decrements by REAL traveled
    // distance so projectiles cover the same total range — they just
    // take 2x as many frames to do it at scale=0.5. Matches the
    // intuition that a 7 km Mass Driver still has 7 km of reach,
    // it just feels like a slower bullet.
    const float scaled_dt = dt * world_scale::k_world_velocity_scale;
    for (Projectile& p : projectiles) {
        if (!p.alive) continue;
        const HMM_Vec3 step = HMM_MulV3F(p.velocity, scaled_dt);
        p.position = HMM_AddV3(p.position, step);
        const float traveled = std::sqrt(HMM_DotV3(step, step));
        p.range_remaining -= traveled;
        if (p.range_remaining <= 0.0f) p.alive = false;
    }
    // Compact: swap dead to the back, then shrink. Stable order isn't
    // needed (projectiles are visually indistinguishable beyond color).
    size_t write = 0;
    for (size_t read = 0; read < projectiles.size(); ++read) {
        if (projectiles[read].alive) {
            if (write != read) projectiles[write] = projectiles[read];
            ++write;
        }
    }
    projectiles.resize(write);
}

#include "ship.h"
#include "ship_sprite.h"   // for sprite->position read

#include <algorithm>

void projectile::collide_and_damage(std::vector<Projectile>& projectiles,
                                    std::vector<Ship>&        ships,
                                    float                     dt) {
    for (Projectile& p : projectiles) {
        if (!p.alive) continue;

        // Reconstruct previous position. Must use the SAME scaled dt
        // the integrator did (see projectile::tick) — otherwise the
        // swept-segment for the hit test extends back past the actual
        // last frame's position and bullets register phantom hits at
        // ranges they never traversed.
        const float scaled_dt = dt * world_scale::k_world_velocity_scale;
        const HMM_Vec3 prev = HMM_SubV3(p.position, HMM_MulV3F(p.velocity, scaled_dt));

        for (Ship& s : ships) {
            if (!s.alive)             continue;
            if (s.id == p.owner_id)   continue;   // can't shoot self

            // Use sprite position (post-integration latest) when ship
            // has one, fall back to ship.position. Same reasoning as
            // the target indicator: sprite->position is the freshest
            // pose at this point in the frame.
            const HMM_Vec3 ship_pos = s.sprite ? s.sprite->position
                                                : s.position;
            const float radius = ship::hit_radius_m(s);
            if (radius <= 0.0f) continue;
            const float r2 = radius * radius;

            // Closest-point-on-segment to ship center. seg = P_curr - P_prev,
            // t = dot(P_ship - P_prev, seg) / |seg|², clamped to [0,1].
            const HMM_Vec3 seg     = HMM_SubV3(p.position, prev);
            const HMM_Vec3 to_ship = HMM_SubV3(ship_pos, prev);
            const float    seg_l2  = HMM_DotV3(seg, seg);
            float t = 0.0f;
            if (seg_l2 > 1e-6f) {
                t = std::clamp(HMM_DotV3(to_ship, seg) / seg_l2, 0.0f, 1.0f);
            }
            const HMM_Vec3 closest = HMM_AddV3(prev, HMM_MulV3F(seg, t));
            const HMM_Vec3 diff    = HMM_SubV3(ship_pos, closest);
            const float    dist2   = HMM_DotV3(diff, diff);

            if (dist2 < r2) {
                // HIT — classify facing, apply damage, kill projectile.
                // One projectile -> at most one hit; break the inner
                // loop so a single bullet can't cascade through ships.
                const HitFacing facing = ship::facing_of_hit(s, closest);
                ship::take_damage(s, p.damage_cm, facing);
                p.alive = false;
                break;
            }
        }
    }
    // Note: dead projectiles are pruned by projectile::tick on the NEXT
    // frame's swap-remove pass. We could compact here too, but the cost
    // is negligible vs. waiting one frame.
}

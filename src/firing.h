#pragma once
// -----------------------------------------------------------------------------
// firing.h — turn `controller.fire_guns` into projectiles.
//
// Sits between ship::tick (which sets fire_guns based on AI / player input)
// and projectile::tick (which advances live projectiles). Each frame:
//
//   1. Decrement every gun's cooldown by dt; clamp at 0.
//   2. Recharge every ship's energy_gj toward klass->energy_recharge.
//   3. For ships whose controller.fire_guns is set: try to fire each
//      mount that's off-cooldown and has enough energy in the pool. A
//      successful fire spawns one Projectile, drains energy_cost_gj from
//      the shared pool, and sets cooldown to refire_delay_s. Misses
//      (energy too low, gun on cooldown) silently skip — Privateer
//      didn't have an "out of ammo" feedback loop and we don't need
//      one yet.
//
// Aim model for v1: every mount fires straight along the SHIP's body
// +Z (forward). No gimbal, no per-mount aim, no lead prediction. The
// AI's Engage state already turns the ship to point at the target; if
// the target is in front of the nose, fire_guns is set. Mounts at off-
// center body offsets still fire forward, just from their offset
// position — produces the "wing guns converge somewhere ahead" look.
//
// Inheritance velocity: projectile starts with the SHIP's forward
// velocity added to the gun's muzzle speed. Realistic-feel — a fast
// ship's bullets fly faster, a fleeing ship's bullets fall short.
// -----------------------------------------------------------------------------

#include <vector>

struct Ship;
struct Projectile;

namespace firing {

// Per-frame tick. Updates cooldowns + energy, spawns projectiles for
// any ship with controller.fire_guns set whose mounts are ready. Player
// ships fire too — fire_guns is populated by main's input code.
void tick(std::vector<Ship>& ships,
          std::vector<Projectile>& projectiles,
          float dt);

} // namespace firing

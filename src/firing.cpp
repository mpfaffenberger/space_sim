#include "firing.h"

#include "gun.h"
#include "projectile.h"
#include "ship.h"
#include "ship_class.h"
#include "ship_sprite.h"   // for sprite->forward_speed read

#include <algorithm>

namespace {

// Aim direction in world frame for projectile spawning. Two paths:
//
//   * NPCs use the SHIP convention (atlas-authored forward = body +Z)
//     and aim along their CURRENT nose direction — orientation*+Z.
//     Lead prediction happens at the AI layer (where to point the nose),
//     not here.
//
//   * Player uses controller.desired_forward as the aim vector. main.cpp
//     sets this each frame from camera-forward + mouse-driven gimbal
//     offset (Freelancer-style: guns track the mouse cursor up to ±15°
//     off the ship's nose). When zero (uninitialised), falls back to the
//     camera-convention forward = orientation * -Z so default behaviour
//     stays "shoot straight ahead".
HMM_Vec3 ship_forward_world(const Ship& s) {
    if (s.is_player) {
        const HMM_Vec3& aim = s.controller.desired_forward;
        const float l2 = HMM_DotV3(aim, aim);
        if (l2 > 1e-6f) return HMM_DivV3F(aim, std::sqrt(l2));
        // Fallback: camera-forward.
        const HMM_Mat4 R = HMM_QToM4(s.orientation);
        const HMM_Vec4 f = HMM_MulM4V4(R, HMM_V4(0, 0, -1, 0));
        return HMM_V3(f.X, f.Y, f.Z);
    }
    const HMM_Mat4 R = HMM_QToM4(s.orientation);
    const HMM_Vec4 f = HMM_MulM4V4(R, HMM_V4(0, 0, 1, 0));
    return HMM_V3(f.X, f.Y, f.Z);
}

// Transform a body-frame offset into world space, given the ship's
// orientation. Used to put the muzzle at the actual mount point on the
// ship — wing-tip guns fire from the wing tips, not the center.
HMM_Vec3 body_to_world(const HMM_Quat& q, HMM_Vec3 v_body) {
    const HMM_Mat4 R = HMM_QToM4(q);
    const HMM_Vec4 v = HMM_MulM4V4(R, HMM_V4(v_body.X, v_body.Y, v_body.Z, 0));
    return HMM_V3(v.X, v.Y, v.Z);
}

} // namespace

void firing::tick(std::vector<Ship>& ships,
                  std::vector<Projectile>& projectiles,
                  float dt) {
    for (Ship& s : ships) {
        if (!s.alive) continue;

        // Cooldowns + energy regen run for EVERY ship every frame —
        // not gated on fire_guns. Otherwise a ship that toggles fire
        // off would freeze its cooldowns mid-cycle.
        for (float& cd : s.gun_cooldowns) {
            cd -= dt;
            if (cd < 0.0f) cd = 0.0f;
        }
        // Energy pool. Player has no klass (until they pick a ship in
        // the upgrade flow); use a generous v1 default so the player
        // can always shoot. NPCs use class numbers.
        constexpr float k_player_energy_max     = 200.0f;
        constexpr float k_player_energy_regen   = 50.0f;
        const float energy_max   = s.klass ? s.klass->energy_max
                                  : (s.is_player ? k_player_energy_max : 0.0f);
        const float energy_regen = s.klass ? s.klass->energy_recharge
                                  : (s.is_player ? k_player_energy_regen : 0.0f);
        s.energy_gj = std::min(s.energy_gj + energy_regen * dt, energy_max);

        if (!s.controller.fire_guns) continue;
        if (s.mounts.empty())        continue;

        const HMM_Vec3 fwd_world = ship_forward_world(s);

        // No gun convergence — every mount fires straight along the
        // ship's aim direction, tracers stay parallel from their muzzle
        // offsets. Parallel is more legible than the toed-in V the
        // earlier convergence math produced; aim accuracy comes from
        // the ITTS reticle telling you where to point.

        // Inherit shooter velocity. ship.world_velocity is populated
        // by sync_from_sprite (NPCs: orientation*+Z*forward_speed) or
        // main.cpp's player block (camera.velocity for the player).
        // Without this, projectiles fly at exactly muzzle speed in the
        // ship's forward direction — fine when the shooter is at
        // rest, but a coasting/strafing player sees tracers drift in
        // their reference frame. Adding the shooter's full 3D motion
        // matches real-world ballistics + every other space sim.
        const HMM_Vec3 ship_v = s.world_velocity;



        for (size_t i = 0; i < s.mounts.size(); ++i) {
            if (s.gun_cooldowns[i] > 0.0f)            continue;
            const GunMount& m  = s.mounts[i];
            if ((int)m.type < 0 || (int)m.type >= kGunTypeCount) continue;
            const GunStats& gs = g_gun_stats[(int)m.type];
            if (!gs.complete)                          continue;   // null-data gun
            if (s.energy_gj < gs.energy_cost_gj)       continue;   // dry

            Projectile p;
            // Muzzle position: ship pos + rotated mount offset.
            p.position = HMM_AddV3(s.position, body_to_world(s.orientation, m.offset_body));
            // Velocity: shooter's full 3D world velocity + muzzle
            // speed along the aim direction. Inheritance lets the
            // tracer fly with the player's frame so coasting / strafing
            // doesn't make bullets visually drift sideways from the
            // crosshair.
            p.velocity = HMM_AddV3(ship_v, HMM_MulV3F(fwd_world, gs.speed_mps));
            p.damage_cm        = gs.damage_cm;
            p.range_remaining  = gs.range_m;
            p.type             = m.type;
            p.owner_id         = s.id;
            p.alive            = true;
            projectiles.push_back(p);

            s.energy_gj         -= gs.energy_cost_gj;
            s.gun_cooldowns[i]   = gs.refire_delay_s;
        }
    }
}

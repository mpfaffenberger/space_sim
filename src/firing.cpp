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

        // Convergence point: where every mount's projectile path should
        // cross. Fixed distance ahead of the ship along the aim vector.
        // Without this, off-center mounts (player's left-low laser at
        // body offset (-10, 10, 0); Talon's wing-tip mass drivers) fire
        // PARALLEL to the aim vector — tracers offset from the cursor
        // by their muzzle offset, never quite hitting where the player
        // points. With convergence, each muzzle aims at the same point
        // and tracer paths visibly cross at that range.
        //
        // 1 km is a typical engagement distance — at the convergence
        // point the tracer hits exactly where the cursor is; closer /
        // farther targets see slight deviation. Real-world fighter
        // guns work the same way (mechanical convergence at a chosen
        // range). Could be made dynamic per-target later.
        constexpr float k_convergence_m = 1000.0f;
        const HMM_Vec3 conv_point =
            HMM_AddV3(s.position, HMM_MulV3F(fwd_world, k_convergence_m));

        // Forward speed lives on the sprite (NPCs) or implicitly 0 for
        // the player (the camera owns player velocity, not the ship).
        // For inheritance we approximate: NPCs use sprite.forward_speed
        // along world forward; player gets 0 (good enough for v1 — the
        // player's projectile speed dwarfs typical camera motion).
        float forward_speed_mps = 0.0f;
        if (s.sprite) forward_speed_mps = s.sprite->forward_speed;

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
            // Fire direction: from THIS muzzle toward the convergence
            // point, NOT straight along ship forward. This is the
            // gun-convergence math — each mount toes-in slightly so
            // all guns' tracer paths cross at conv_point. Falls back
            // to straight forward in the degenerate case where muzzle
            // happens to be at the convergence point (sub-mm distance).
            HMM_Vec3 fire_dir = HMM_SubV3(conv_point, p.position);
            const float fd_l2 = HMM_DotV3(fire_dir, fire_dir);
            fire_dir = (fd_l2 > 1e-6f) ? HMM_DivV3F(fire_dir, std::sqrt(fd_l2))
                                       : fwd_world;

            // Velocity: ship's forward inherited + muzzle speed along
            // the per-mount fire direction.
            const float total_speed = forward_speed_mps + gs.speed_mps;
            p.velocity = HMM_MulV3F(fire_dir, total_speed);
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

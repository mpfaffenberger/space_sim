#include "ship.h"

#include "armor.h"
#include "mobility.h"
#include "ship_class.h"
#include "ship_sprite.h"
#include "shield.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr float k_deg_to_rad = 0.017453293f;

// What direction the ship's nose is pointing, in world space. Identity
// orientation = nose along world +Z (matches the atlas-authoring
// convention; same as ship_sprite.cpp's integrator).
HMM_Vec3 forward_world(const HMM_Quat& q) {
    const HMM_Mat4 R = HMM_QToM4(q);
    const HMM_Vec4 f = HMM_MulM4V4(R, HMM_V4(0.0f, 0.0f, 1.0f, 0.0f));
    return HMM_V3(f.X, f.Y, f.Z);
}

// Rotate a world-space vector into body frame using the inverse of the
// ship's orientation. Used to convert a desired-rotation axis (which the
// controller computes in world frame from current vs desired forward)
// into body frame, since update_ship_sprite_motion expects body-frame
// angular_velocity.
HMM_Vec3 world_to_body(const HMM_Quat& q, HMM_Vec3 v_world) {
    const HMM_Quat q_inv = HMM_InvQ(q);
    const HMM_Mat4 R_inv = HMM_QToM4(q_inv);
    const HMM_Vec4 v4    = HMM_MulM4V4(R_inv, HMM_V4(v_world.X, v_world.Y, v_world.Z, 0.0f));
    return HMM_V3(v4.X, v4.Y, v4.Z);
}

// PursueTarget: aim at target_pos and ramp throttle so we ARRIVE rather
// than overshoot. Naive "throttle to cruise always" produces clover-leaf
// orbits around the target — at 400 m/s with 50°/s max yaw, the U-turn
// radius (~v / max_yaw_rad ≈ 460 m) is comparable to typical engagement
// ranges, so the ship flies past, turns, flies past again, forever. The
// fix is a distance-based throttle ramp: full cruise far out, linear
// decel through `slow_radius`, hard stop inside `stop_radius`. Radii are
// derived from cruise_speed so a faster ship gets a proportionally
// wider arrival cone (Centurion @ 500 m/s starts slowing 5 km out;
// Tarsus @ 300 starts at 3 km).
void behavior_pursue_target(Ship& s) {
    if (!s.sprite || !s.klass) return;
    const HMM_Vec3 to = HMM_SubV3(s.behavior.target_pos, s.sprite->position);
    const float    d2 = HMM_DotV3(to, to);
    if (d2 < 1e-3f) {
        // Sub-mm distance — we ARE the target. Park.
        s.controller.desired_speed = 0.0f;
        return;
    }
    const float d = std::sqrt(d2);
    s.controller.desired_forward = HMM_DivV3F(to, d);

    // Kinematic arrival: at distance `d` (minus a small arrival
    // tolerance), the highest speed we can carry and STILL decelerate
    // to zero exactly at the target is v_brake = sqrt(2·a·d). Clamp to
    // cruise_speed and that's our target throttle. This auto-scales:
    // far away -> full cruise (v_brake exceeds cruise so the clamp
    // wins), close in -> the sqrt curve smoothly ramps speed down,
    // arrival distance -> exactly zero. No tuning constants beyond the
    // arrival tolerance; the ship's own accel + cruise pick the
    // window. A previous draft used arbitrary radii (cruise·10 / cruise·1)
    // which under-shot at short range — Talon at 500m initial distance
    // got a 11 m/s target and looked stationary. The kinematic version
    // gives ~308 m/s at the same distance and a clean smooth approach.
    constexpr float k_arrival_tol_m = 25.0f;
    const float v_max         = s.klass->cruise_speed;
    const float accel         = mobility::accel_mps2(s.klass->acceleration);
    const float dist_braking  = std::max(0.0f, d - k_arrival_tol_m);
    const float v_brake       = std::sqrt(2.0f * accel * dist_braking);
    s.controller.desired_speed = std::min(v_max, v_brake);
}

// Proportional flight controller. Closes the angle from current forward
// to controller.desired_forward by writing a body-frame omega; lerps
// forward_speed toward controller.desired_speed at class accel rate.
//
// Kp = 4.0 produces a snappy-but-not-twitchy turn — at the max-rate
// clamp, a 90° gap closes in roughly π/2 / max_rate seconds (~1.5s for
// a Good-tier ship). Tune later if the AI feels sluggish/jittery.
void flight_controller_step(Ship& s, float dt) {
    if (!s.sprite || !s.klass) return;
    const HMM_Vec3 fwd  = forward_world(s.sprite->orientation);
    const HMM_Vec3 want = s.controller.desired_forward;

    // axis_world = fwd × want; |axis_world| = sin(angle); fwd·want = cos(angle).
    HMM_Vec3 axis_world = HMM_Cross(fwd, want);
    const float sin_mag2 = HMM_DotV3(axis_world, axis_world);
    if (sin_mag2 > 1e-10f) {
        const float sin_mag = std::sqrt(sin_mag2);
        const float cos_a   = std::clamp(HMM_DotV3(fwd, want), -1.0f, 1.0f);
        const float angle   = std::atan2(sin_mag, cos_a);   // 0..π, well-conditioned

        // Convert rotation axis from world to body frame for the integrator.
        HMM_Vec3 axis_body = world_to_body(s.sprite->orientation,
                                            HMM_DivV3F(axis_world, sin_mag));
        // Defensive renormalise — rotation is unitary so axis_body's
        // magnitude should already be ~1, but float drift over time can
        // accumulate if we skip this.
        const float ab_len2 = HMM_DotV3(axis_body, axis_body);
        if (ab_len2 > 1e-10f) axis_body = HMM_DivV3F(axis_body, std::sqrt(ab_len2));

        // Single combined max-rate cap. Privateer ships have similar
        // yaw/pitch tiers, so the simpler model (single rate) reads
        // identical in-flight to a per-axis controller; revisit when a
        // ship class wants asymmetric rates.
        constexpr float Kp = 4.0f;
        const float max_rate_rad = std::min(mobility::yaw_rate_deg(s.klass->max_ypr),
                                            mobility::pitch_rate_deg(s.klass->max_ypr))
                                   * k_deg_to_rad;
        const float omega_mag    = std::min(Kp * angle, max_rate_rad);
        s.sprite->angular_velocity = HMM_MulV3F(axis_body, omega_mag);
    } else {
        // Aligned — kill any residual rotation so the integrator doesn't
        // keep nudging us off-axis. Without this, tiny numerical
        // remainders from previous frames can produce visible jitter.
        s.sprite->angular_velocity = HMM_V3(0.0f, 0.0f, 0.0f);
    }

    // Throttle: lerp forward_speed toward desired at class accel.
    const float ds       = s.controller.desired_speed - s.sprite->forward_speed;
    const float max_step = mobility::accel_mps2(s.klass->acceleration) * dt;
    s.sprite->forward_speed += std::clamp(ds, -max_step, +max_step);
}

} // namespace

Ship ship::spawn(const ShipClass& klass) {
    static uint32_t s_next_id = 1;

    Ship s;
    s.id      = s_next_id++;
    s.klass   = &klass;
    s.faction = klass.default_faction;

    // Health = full max. Base hull armor from class; shields from the
    // fitted shield generator if one is configured.
    s.armor_fore_cm  = klass.armor_fore_cm;
    s.armor_aft_cm   = klass.armor_aft_cm;
    s.armor_side_cm  = klass.armor_side_cm;
    if (klass.default_armor) {
        // Stack fitted-armor cm onto the base hull. Privateer's manual
        // mostly treats armor as "tier replaces base" rather than "tier
        // adds to base", but the additive interpretation is what the
        // fan-data ArmorType numbers were authored for. Either is fine
        // gameplay-wise; pick one and stay consistent.
        s.armor_fore_cm += klass.default_armor->front_cm;
        s.armor_aft_cm  += klass.default_armor->back_cm;
        s.armor_side_cm += klass.default_armor->side_cm;
    }
    if (klass.default_shield) {
        s.shield_fore_cm = klass.default_shield->front_cm;
        s.shield_aft_cm  = klass.default_shield->back_cm;
        s.shield_side_cm = klass.default_shield->side_cm;
    }
    s.energy_gj = klass.energy_max;

    // Controller idle until a behavior fills it in.
    s.controller.desired_forward = HMM_V3(0.0f, 0.0f, 1.0f);
    s.controller.desired_speed   = 0.0f;
    return s;
}

void ship::tick(Ship& s, float dt) {
    if (!s.alive) return;
    switch (s.behavior.kind) {
    case ShipBehavior::None:
        // Controller is idle — leave sprite kinematics alone. This is
        // the path the existing demo runs on (JSON-set angular_velocity
        // + forward_speed, integrator advances them). Adding the Ship
        // struct to a system MUST stay backwards-compatible with this
        // case or every existing scene starts behaving differently.
        return;

    case ShipBehavior::PursueTarget:
        behavior_pursue_target(s);
        flight_controller_step(s, dt);
        return;
    }
}

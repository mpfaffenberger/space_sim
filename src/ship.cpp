#include "ship.h"

#include "armor.h"
#include "mobility.h"
#include "ship_class.h"
#include "ship_sprite.h"
#include "shield.h"
#include "world_scale.h"

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
                                   * s.klass->ypr_rate_multiplier
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

namespace { uint32_t s_next_id = 1; }   // shared monotonic counter

Ship ship::spawn_player() {
    Ship s;
    s.id        = s_next_id++;
    s.is_player = true;
    s.klass     = nullptr;
    s.faction   = Faction::Civilian;   // unaligned; rep is the real currency
    s.alive     = true;
    // No sprite, no controller, no behaviour. Pose is filled in each frame
    // from the camera before perception runs. Health/energy don't apply
    // until the player ship has a class — defer to a follow-up that lets
    // the player pick a Centurion/Tarsus/etc. and inherit its stats.
    return s;
}

void ship::sync_from_sprite(Ship& s) {
    if (!s.sprite) return;
    s.position    = s.sprite->position;
    s.orientation = s.sprite->orientation;
}

Ship ship::spawn(const ShipClass& klass) {
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

    // Mount loadout: copy the class default. gun_cooldowns sized to
    // match, all starting at 0 (fully ready to fire). Per-instance
    // copy lets the player upgrade individual ships without mutating
    // the shared ShipClass.
    s.mounts        = klass.default_guns;
    s.gun_cooldowns.assign(s.mounts.size(), 0.0f);

    // Controller idle until a behavior fills it in.
    s.controller.desired_forward = HMM_V3(0.0f, 0.0f, 1.0f);
    s.controller.desired_speed   = 0.0f;
    return s;
}

void ship::tick(Ship& s, float dt) {
    if (!s.alive)     return;
    if (s.is_player)  return;   // player flies via camera input, not the controller
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

    case ShipBehavior::ChaseTarget: {
        // "Fly toward this point at full cruise, never slow." The dogfight
        // counterpart to PursueTarget: PursueTarget is for *arriving at*
        // a waypoint and stopping; ChaseTarget is for *attacking through*
        // a target — we want overshoot, not docking, so the kinematic-
        // arrival ramp would actively hurt (it'd drop speed below the
        // fleer's cruise and produce the orbital tail-chase pattern).
        //
        // Honors the controller.afterburner flag: AI's BreakOff state
        // sets it true so extensions plow out fast; Engage / Flee leave
        // it false for cruise-speed pursuit / weave. afterburner_speed=0
        // means "no afterburner fitted" (the JSON default for ships
        // missing one); fall back to cruise in that case.
        if (!s.sprite || !s.klass) return;
        const HMM_Vec3 to = HMM_SubV3(s.behavior.target_pos, s.sprite->position);
        const float d2 = HMM_DotV3(to, to);
        if (d2 > 1e-3f) {
            s.controller.desired_forward = HMM_DivV3F(to, std::sqrt(d2));
        }
        const float ab = s.klass->afterburner_speed;
        const float base =
            (s.controller.afterburner && ab > 0.0f) ? ab : s.klass->cruise_speed;
        s.controller.desired_speed = base * s.controller.speed_scale;
        flight_controller_step(s, dt);
        return;
    }
    }
}

// ----------------------------------------------------------------------------
// Damage pipeline — see ship.h for declarations.
// ----------------------------------------------------------------------------

namespace {

// How long shield regen pauses on the hit facing after taking damage.
// Privateer-canonical "shields can't recover under sustained fire" feel —
// continuous fire keeps the timer pegged so shields stay down until the
// shooter lays off. 3 seconds is the original game's number; tune
// per-class later (capships might pause longer, ace shields shorter).
constexpr float k_shield_pause_after_hit = 3.0f;

// Map a HitFacing to the right (shield, armor, pause) triplet on a Ship.
// References-by-pointer because there's no clean way to return references
// to a varying-trio in C++ without a helper struct; the pointers are
// always non-null after the switch.
struct HitTarget { float* shield; float* armor; float* pause;
                   float shield_max; };
HitTarget hit_target(Ship& s, HitFacing f) {
    const float sh_max_fore = s.klass && s.klass->default_shield
                            ? s.klass->default_shield->front_cm : 0.0f;
    const float sh_max_aft  = s.klass && s.klass->default_shield
                            ? s.klass->default_shield->back_cm  : 0.0f;
    const float sh_max_side = s.klass && s.klass->default_shield
                            ? s.klass->default_shield->side_cm  : 0.0f;
    switch (f) {
        case HitFacing::Fore: return { &s.shield_fore_cm, &s.armor_fore_cm,
                                       &s.shield_pause_fore, sh_max_fore };
        case HitFacing::Aft:  return { &s.shield_aft_cm,  &s.armor_aft_cm,
                                       &s.shield_pause_aft,  sh_max_aft  };
        case HitFacing::Side: return { &s.shield_side_cm, &s.armor_side_cm,
                                       &s.shield_pause_side, sh_max_side };
    }
    // Unreachable (enum exhausted), but the compiler wants a return.
    return { &s.shield_fore_cm, &s.armor_fore_cm, &s.shield_pause_fore, sh_max_fore };
}

const char* facing_name(HitFacing f) {
    switch (f) { case HitFacing::Fore: return "fore";
                 case HitFacing::Aft:  return "aft";
                 case HitFacing::Side: return "side"; }
    return "?";
}

} // namespace

void ship::take_damage(Ship& s, float damage_cm, HitFacing facing) {
    if (!s.alive || damage_cm <= 0.0f) return;
    HitTarget t = hit_target(s, facing);

    // Reset regen pause on the affected facing — sustained fire keeps
    // shields suppressed until the shooter lays off.
    *t.pause = k_shield_pause_after_hit;

    // Shield first (with implicit effect_pct = 100% for v1; the per-shield
    // multiplier is loaded but not yet folded in — TODO before L5).
    if (*t.shield > 0.0f) {
        const float absorbed = std::min(*t.shield, damage_cm);
        *t.shield  -= absorbed;
        damage_cm  -= absorbed;
    }
    // Armor (spillover). Goes negative on a kill blow; we clamp at 0
    // for display but flag the kill.
    if (damage_cm > 0.0f) {
        *t.armor -= damage_cm;
        if (*t.armor <= 0.0f) {
            *t.armor    = 0.0f;
            s.alive     = false;
            // Hide the visual instantly. world_size = 0 makes the
            // billboard collapse; explosion FX is a future feature.
            if (s.sprite) s.sprite->world_size = 0.001f;
            std::printf("[ship] killed: id=%u (%s hit)\n",
                        s.id, facing_name(facing));
        }
    }
}

void ship::regen_shields(Ship& s, float dt) {
    if (!s.alive || !s.klass || !s.klass->default_shield) return;
    // Scale regen by its dedicated knob (world_scale.h). Separate from
    // the velocity scale because regen plays a different role: it sets
    // attrition pacing, not movement feel. At 0.0167 a Shield
    // Generator 1's canonical 4 cm/s becomes ~0.067 cm/s — ~15 seconds
    // per cm, so damage actually accumulates between attack runs.
    const float regen_rate = s.klass->default_shield->regen_cm_per_s
                           * world_scale::k_shield_regen_scale;

    auto tick_quad = [&](float& q, float& pause, float max_cm) {
        if (pause > 0.0f) {
            pause -= dt;
            if (pause < 0.0f) pause = 0.0f;
            return;          // suppressed this frame
        }
        if (q < max_cm) {
            q = std::min(q + regen_rate * dt, max_cm);
        }
    };
    tick_quad(s.shield_fore_cm, s.shield_pause_fore, s.klass->default_shield->front_cm);
    tick_quad(s.shield_aft_cm,  s.shield_pause_aft,  s.klass->default_shield->back_cm);
    tick_quad(s.shield_side_cm, s.shield_pause_side, s.klass->default_shield->side_cm);
}

float ship::hit_radius_m(const Ship& s) {
    // NPCs: half the rendered length feels right as a hit sphere — most
    // sprite ships are roughly as wide as they are tall, and capturing a
    // hit anywhere inside that sphere reads correctly with our bullet
    // sizes. Note: ship sprites are spawned with world_size already
    // multiplied by k_ship_size_scale (see main.cpp), so this read
    // automatically picks up the global ship-scale.
    //
    // Player without a visible ship: 30 m typical-fighter default,
    // also scaled by k_ship_size_scale so the player's collision
    // matches the same global knob NPCs respect.
    if (s.sprite)    return s.sprite->world_size * 0.5f;
    if (s.is_player) return 30.0f * world_scale::k_ship_size_scale;
    return 0.0f;
}

HitFacing ship::facing_of_hit(const Ship& s, const HMM_Vec3& hit_pos_world) {
    // Take hit position into body frame and look at which axis dominates.
    // Body +Z forward = Fore, -Z = Aft, |X| or |Y| dominant = Side.
    HMM_Vec3 to_hit = HMM_SubV3(hit_pos_world, s.position);
    const HMM_Mat4 R_inv = HMM_QToM4(HMM_InvQ(s.orientation));
    const HMM_Vec4 v = HMM_MulM4V4(R_inv, HMM_V4(to_hit.X, to_hit.Y, to_hit.Z, 0.0f));

    // Player uses camera convention (forward = -Z), so flip Z axis to
    // match ship convention before classifying the facing.
    float bz = s.is_player ? -v.Z : v.Z;
    const float bx = v.X, by = v.Y;

    // Threshold: pick fore/aft only if the Z-axis component is at least
    // half the magnitude of the lateral axes. Otherwise it's a side hit.
    const float lateral = std::sqrt(bx * bx + by * by);
    if (std::fabs(bz) > lateral) {
        return (bz > 0.0f) ? HitFacing::Fore : HitFacing::Aft;
    }
    return HitFacing::Side;
}

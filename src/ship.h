#pragma once
// -----------------------------------------------------------------------------
// ship.h — per-instance ship state + flight controller + behaviors.
//
// One Ship per actual ship in the world. Pairs with the existing
// ShipSpriteObject for visuals — the sprite owns the kinematics state
// (position / orientation / angular_velocity / forward_speed) and the
// existing per-frame integrator advances them. Ship adds the layers on
// top: which class this is, which faction it belongs to, current health
// in cm-of-durasteel per facing, energy reserve, and a flight controller
// that decides each tick what the ship is *trying* to do.
//
// The flight controller is a proportional controller: a behavior writes
// `desired_forward` (unit vec, world space) + `desired_speed` (m/s) +
// throttle / fire flags, and ship::tick() turns the angular gap into a
// body-frame angular_velocity (clamped by class max yaw/pitch rate),
// and lerps forward_speed toward the desired value at the class's accel
// rate. The downstream integrator (update_ship_sprite_motion) then
// integrates the resulting omega + speed exactly as it does today —
// ZERO change to that path.
//
// Behaviors today are intentionally minimal:
//   None         — controller is idle. The ship keeps whatever
//                  angular_velocity / forward_speed were set externally
//                  (e.g. by the JSON loader). Existing demo flies on
//                  this code path with no behavioural change.
//   PursueTarget — turn toward `target_pos`, throttle to cruise speed.
//                  First taste of "AI is alive" — when wired in JSON,
//                  the ship aims itself at the chosen world point.
//
// Larger AI structure (state machine, sensors, factions vs target
// selection, weapons, damage) is the next layer up; this file just
// provides the substrate.
// -----------------------------------------------------------------------------

#include "faction.h"

#include <HandmadeMath.h>
#include <cstdint>

struct ShipClass;
struct ShipSpriteObject;

// What the controller is aiming for THIS tick. Behaviors write here;
// the flight controller reads here.
struct ShipFlightController {
    HMM_Vec3 desired_forward = { 0.0f, 0.0f, 1.0f };  // unit vec, world frame
    float    desired_speed   = 0.0f;                   // m/s
    bool     afterburner     = false;
    bool     fire_guns       = false;
};

enum class ShipBehavior : uint8_t {
    None = 0,         // controller idle; sprite keeps externally-set kinematics
    PursueTarget,     // turn toward target_pos, throttle to cruise
};

struct ShipBehaviorState {
    ShipBehavior kind = ShipBehavior::None;
    HMM_Vec3     target_pos = { 0, 0, 0 };  // PursueTarget aim point (world)
};

struct Ship {
    // ---- identity ------------------------------------------------------
    uint32_t          id      = 0;          // monotonic; 0 reserved for "none"
    const ShipClass*  klass   = nullptr;
    Faction           faction = Faction::Civilian;

    // ---- visual back-pointer ------------------------------------------
    // Non-owning. The ShipSpriteObject vector lives in main's State and
    // outlives every Ship; we just point into it. This keeps kinematics
    // ownership unchanged from the prior commits — the existing
    // update_ship_sprite_motion path still runs the integration, the
    // controller above just writes its inputs.
    ShipSpriteObject* sprite = nullptr;

    // ---- behaviour + controller ---------------------------------------
    ShipFlightController controller;
    ShipBehaviorState    behavior;

    // ---- health (cm of durasteel; same unit as gun damage) ------------
    float armor_fore_cm  = 0.0f;
    float armor_aft_cm   = 0.0f;
    float armor_side_cm  = 0.0f;
    float shield_fore_cm = 0.0f;
    float shield_aft_cm  = 0.0f;
    float shield_side_cm = 0.0f;
    float energy_gj      = 0.0f;

    bool  alive = true;
};

namespace ship {

// Construct a fresh Ship from a class. Health/shield/energy filled to
// max from klass + klass->default_shield. Caller is expected to assign
// `sprite` (after building the matching ShipSpriteObject) and may
// override `faction` if the spawn context wants something different
// from the class default (e.g. a captured Talon flying for Confed).
Ship spawn(const ShipClass& klass);

// Per-frame controller + behavior step. Call BEFORE
// update_ship_sprite_motion: the behaviour writes the controller's
// desired_*, the flight controller writes sprite->angular_velocity and
// sprite->forward_speed, then the unchanged integrator advances the
// pose using those values.
//
// dt is in seconds. No-op for ships with behavior == None (preserves
// the existing demo's JSON-driven motion).
void tick(Ship& s, float dt);

} // namespace ship

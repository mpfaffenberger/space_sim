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
#include "gun.h"
#include "perception.h"
#include "ship_ai.h"

#include <HandmadeMath.h>
#include <cstdint>
#include <vector>

struct ShipClass;
struct ShipSpriteObject;

// Hit facing classification — top-level so collision code (projectile.cpp)
// and ship code (ship.cpp) can share without nested-namespace gymnastics.
enum class HitFacing : uint8_t { Fore, Aft, Side };

// What the controller is aiming for THIS tick. Behaviors write here;
// the flight controller reads here.
struct ShipFlightController {
    HMM_Vec3 desired_forward = { 0.0f, 0.0f, 1.0f };  // unit vec, world frame
    float    desired_speed   = 0.0f;                   // m/s
    bool     afterburner     = false;
    bool     fire_guns       = false;

    // Multiplier on the behavior-supplied target speed. Engage uses
    // 0.5 to throttle down for combat tracking (full cruise overshoots
    // the target faster than the ship can track its evasion); BreakOff
    // uses 1.0 with afterburner=true for clean separation. Other states
    // default to 1.0. Read by ChaseTarget when picking desired_speed.
    float    speed_scale     = 1.0f;
};

enum class ShipBehavior : uint8_t {
    None = 0,         // controller idle; sprite keeps externally-set kinematics
    PursueTarget,     // turn toward target_pos, kinematic-arrival throttle
    ChaseTarget,      // turn toward target_pos, FULL CRUISE (no slow-on-approach)
};

struct ShipBehaviorState {
    ShipBehavior kind = ShipBehavior::None;
    HMM_Vec3     target_pos = { 0, 0, 0 };  // PursueTarget aim point (world)
};

struct Ship {
    // ---- identity ------------------------------------------------------
    uint32_t          id        = 0;        // monotonic; 0 reserved for "none"
    const ShipClass*  klass     = nullptr;  // nullptr is legal for the player
    Faction           faction   = Faction::Civilian;
    bool              is_player = false;    // toggled on by ship::spawn_player()

    // ---- canonical pose ------------------------------------------------
    // Authoritative for perception + AI logic. Synced once per frame from
    // the source of truth, which differs by ship kind:
    //   * NPC ships     — copied from sprite->position/orientation
    //                     (the integrator owns those, see ship_sprite.cpp).
    //   * Player ship   — copied from the camera, which is the source of
    //                     truth for player input. The reverse direction
    //                     (write back into camera) isn't needed because
    //                     the player ship doesn't run a controller.
    // Doing the sync once per frame means perception + AI read a single
    // unified field (`s.position`) regardless of whether the ship is an
    // NPC or the player — no special-casing in the inner loops.
    HMM_Vec3 position    = { 0.0f, 0.0f, 0.0f };
    HMM_Quat orientation = { 0.0f, 0.0f, 0.0f, 1.0f };

    // World-frame velocity (m/s). Populated per frame from the source
    // of truth: camera.velocity for the player, sprite forward * speed
    // for NPCs (synced in ship::sync_from_sprite). Read by firing.cpp
    // so spawned projectiles inherit the shooter's motion — bullets
    // fired from a moving ship carry that ship's velocity, the way
    // real-world physics works. Without this, lateral coasting (with
    // damping=0) makes tracers visually drift relative to the player.
    HMM_Vec3 world_velocity = { 0.0f, 0.0f, 0.0f };

    // ---- visual back-pointer ------------------------------------------
    // Non-owning. The ShipSpriteObject vector lives in main's State and
    // outlives every Ship; we just point into it. This keeps kinematics
    // ownership unchanged from the prior commits — the existing
    // update_ship_sprite_motion path still runs the integration, the
    // controller above just writes its inputs.
    //
    // nullptr for the player ship (the player is INSIDE their cockpit;
    // the cockpit_hud renderer handles their visuals) and for any AI
    // entity whose visuals are off-screen / abstract.
    ShipSpriteObject* sprite = nullptr;

    // ---- behaviour + controller ---------------------------------------
    ShipFlightController controller;
    ShipBehaviorState    behavior;

    // ---- AI state machine (L3) ----------------------------------------
    // When ai.enabled is true, ship_ai::tick OWNS behavior — it'll be
    // overwritten every frame based on perception + state. When false,
    // the JSON-set or scripted behaviour stands. This split keeps the
    // demo's legacy-motion ships (Tarsus circle) working alongside
    // AI-driven ships (Talon engaging) in the same scene.
    ShipAIState ai;

    // ---- perception ---------------------------------------------------
    // Refreshed by perception::tick once per frame, BEFORE ship::tick.
    // Behaviours may read this to pick desired_forward / desired_speed
    // ("chase nearest hostile", "flee if outnumbered"). The vector
    // inside is reused across ticks — no per-frame allocation in the
    // steady state once perceived counts stop growing.
    ShipPerception perception;

    // ---- health (cm of durasteel; same unit as gun damage) ------------
    // current values per facing — start at max from class+armor+shield,
    // ticked down by ship::take_damage, regenerated for shields by
    // ship::regen_shields after a per-facing pause window.
    float armor_fore_cm  = 0.0f;
    float armor_aft_cm   = 0.0f;
    float armor_side_cm  = 0.0f;
    float shield_fore_cm = 0.0f;
    float shield_aft_cm  = 0.0f;
    float shield_side_cm = 0.0f;
    // Regen-pause timers (seconds remaining) per facing. Set to
    // k_shield_pause_after_hit when that quadrant takes damage; the
    // shield can't refill until this hits zero. Privateer-canonical
    // "shields drop, can't immediately recover" feel.
    float shield_pause_fore = 0.0f;
    float shield_pause_aft  = 0.0f;
    float shield_pause_side = 0.0f;
    float energy_gj         = 0.0f;

    // ---- weapons ------------------------------------------------------
    // Per-instance copy of the fitted gun mounts. Initialised from
    // klass->default_guns at spawn for NPCs; for the player ship
    // (klass=nullptr) we hardcode a default loadout in main.cpp until
    // the upgrade-economy commit lets the player pick one. Per-mount
    // because future code will swap weapons at-rest, and that's where
    // GunMount::type changes — keeping it on Ship rather than ShipClass.
    std::vector<GunMount> mounts;
    // Cooldown timers (seconds remaining) sized to mounts.size(). A
    // gun fires when its cooldown reaches 0; firing::tick decrements
    // every tick. Vector instead of fixed array because Privateer's
    // capships have up to 8 mounts plus turrets.
    std::vector<float>    gun_cooldowns;

    bool  alive = true;
};

namespace ship {

// Construct a fresh Ship from a class. Health/shield/energy filled to
// max from klass + klass->default_shield. Caller is expected to assign
// `sprite` (after building the matching ShipSpriteObject) and may
// override `faction` if the spawn context wants something different
// from the class default (e.g. a captured Talon flying for Confed).
Ship spawn(const ShipClass& klass);

// Construct a player Ship. No class, no sprite, faction defaults to
// Civilian (the player has no faction in Privateer's sense — only
// per-faction reputation). The caller wires position + orientation
// each frame from the camera. Uses the same monotonic ID counter as
// spawn(), so the player gets a real ID (>= 1) and 0 stays reserved
// as the "none" sentinel for nearest_hostile_id / nearest_friend_id.
Ship spawn_player();

// Snapshot the sprite's pose into the Ship's canonical pose fields.
// No-op for ships without a sprite (player + abstract AI). Cheap —
// two field copies — but bundled into a function so the per-frame
// sync code in main.cpp reads cleanly.
void sync_from_sprite(Ship& s);

// Damage-pipeline support — see implementation in ship.cpp for
// per-function notes. All in cm-of-durasteel (same unit as gun damage).
void       take_damage(Ship& s, float damage_cm, ::HitFacing facing);
void       regen_shields(Ship& s, float dt);
float      hit_radius_m(const Ship& s);
::HitFacing  facing_of_hit(const Ship& s, const HMM_Vec3& hit_pos_world);

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

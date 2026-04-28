#pragma once
// -----------------------------------------------------------------------------
// ship_ai.h — high-level AI state machine.
//
// One layer up from the flight controller. Reads perception (L2),
// transitions between discrete behavioural states, and writes the
// concrete behaviour (L1) that the controller will close in on this
// tick. Not a full Privateer-grade brain yet — the v1 set is the
// minimal viable subset that produces visible reactivity:
//
//   Idle    — no perceived hostiles, no patrol assignment, sit still.
//             Behaviour stays None so any externally-set kinematics
//             (e.g. JSON-authored angular_velocity for the Tarsus's
//             demo circle) keep running. This is the back-compat
//             fallthrough that means "ship has no AI today".
//
//   Patrol  — same as Idle today (placeholder). When OrbitAnchor lands,
//             this becomes "circle around patrol_anchor at cruise/2".
//             Tracked separately so future commits can wire it without
//             reshuffling state IDs.
//
//   Engage  — perceived a hostile. Behaviour = PursueTarget pointed at
//             the target ship's CURRENT position (refreshed every tick
//             — this is the dynamic-target moment that distinguishes
//             AI chase from a static fire-and-forget waypoint).
//
//   Flee    — health below threshold AND a hostile is in range. Run
//             the OPPOSITE direction at cruise speed. Won't visibly
//             trigger until L4 (damage pipeline) lets ships actually
//             take hits — the wiring is here so L4 doesn't need an
//             AI revision.
//
// Each ship's state lives on Ship::ai (added in ship.h). ship_ai::tick
// is called once per frame, AFTER perception::tick (it consumes that
// data) and BEFORE ship::tick (which consumes the behaviour we write).
//
// The state machine is a pure function of (current state, perception,
// hp). No timers / cooldowns / hysteresis yet — those are the next
// nuance pass once we see ships flicker between states. Privateer's
// AI used a few seconds of "I just left this state" buffering to stop
// chatter; we'll add it when the demo demands it.
// -----------------------------------------------------------------------------

#include <HandmadeMath.h>
#include <cstdint>
#include <vector>

struct Ship;

enum class AIState : uint8_t {
    Idle = 0,
    Patrol,
    Engage,
    Flee,
    Count
};

struct ShipAIState {
    // True when this ship's behaviour should be driven by the AI state
    // machine (set by the JSON loader when an `ai` block is present).
    // When false, ship_ai::tick is a no-op for this ship — the existing
    // scripted-behaviour or legacy-motion path runs unchanged. This is
    // how Tarsus keeps its demo circle while the Talon goes AI-driven.
    bool      enabled = false;

    AIState   state = AIState::Idle;
    uint32_t  target_id = 0;          // Engage / Flee anchor; 0 = none
    float     state_entered_at = 0.0f;

    // Patrol parameters (placeholder for v1 — Patrol currently identical
    // to Idle). When OrbitAnchor behaviour lands, Patrol will read these.
    HMM_Vec3  patrol_anchor = { 0.0f, 0.0f, 0.0f };
    bool      has_patrol_anchor = false;
};

namespace ship_ai {

// Update one ship's state and write a fresh behaviour for the controller
// to consume this frame. Called once per ship per frame between
// perception::tick (L2) and ship::tick (L1).
//
// `all_ships` is needed for target lookup by ID (Engage / Flee resolve
// nearest_hostile_id from perception into the target's actual position).
// Linear scan; cheap at the demo's N — promote to a hash on the way to
// hundreds of ships.
//
// `t_now` is the engine's monotonic seconds counter; used to stamp
// state_entered_at for future hysteresis logic.
void tick(Ship& s, const std::vector<Ship>& all_ships, float t_now);

// Convert AIState to / from JSON-friendly lowercase strings.
const char* to_name(AIState st);
AIState     from_name(std::string_view s);

} // namespace ship_ai

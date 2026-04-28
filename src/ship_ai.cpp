#include "ship_ai.h"

#include "armor.h"
#include "perception.h"
#include "shield.h"
#include "ship.h"
#include "ship_class.h"

#include <cmath>
#include <string>
#include <string_view>

namespace {

// Linear lookup. N is small (demo: 3 ships); upgrade to an
// unordered_map<id, idx> when ships start spawning by the dozens.
const Ship* find_by_id(const std::vector<Ship>& ships, uint32_t id) {
    if (id == 0) return nullptr;
    for (const Ship& s : ships) if (s.id == id) return &s;
    return nullptr;
}

// Health fraction, 0..1. Sums armor + shield across all three facings
// against the per-class max (base hull + fitted armor + fitted shield).
// Returns 1.0 when no class info is available so the AI defaults to
// "fully healthy" rather than "near-dead-flee" on placeholder ships.
float hp_fraction(const Ship& s) {
    if (!s.klass) return 1.0f;
    const float cur = s.armor_fore_cm  + s.armor_aft_cm  + s.armor_side_cm
                    + s.shield_fore_cm + s.shield_aft_cm + s.shield_side_cm;
    float maxv = s.klass->armor_fore_cm + s.klass->armor_aft_cm + s.klass->armor_side_cm;
    if (s.klass->default_armor) {
        maxv += s.klass->default_armor->front_cm + s.klass->default_armor->back_cm
              + s.klass->default_armor->side_cm;
    }
    if (s.klass->default_shield) {
        maxv += s.klass->default_shield->front_cm + s.klass->default_shield->back_cm
              + s.klass->default_shield->side_cm;
    }
    return (maxv > 0.0f) ? (cur / maxv) : 1.0f;
}

// Threshold where Engage flips to Flee. Privateer-feel: ships fight
// hard until they're badly hurt, then run. 30% leaves enough margin
// that a hit-and-run pursuer can't always catch a fleeing ship; the
// fleer regenerates shields if it escapes. Constant for v1, eventually
// per-class (cowardly merchant captain flees at 60%, ace pilot at 10%).
constexpr float k_flee_threshold = 0.30f;

} // namespace

void ship_ai::tick(Ship& s, const std::vector<Ship>& all_ships, float t_now) {
    if (!s.alive || s.is_player) return;
    if (!s.ai.enabled)            return;

    const ShipPerception& p = s.perception;
    AIState next = s.ai.state;

    // ---- transitions ---------------------------------------------------
    // Single-pass "what should I be doing right now". No hysteresis: a
    // transient blip on perception will instantly flip state. Easy to
    // add a "stay in state for at least N seconds" guard later using
    // state_entered_at — left out of v1 because the demo doesn't have
    // enough ships to produce thrash.
    const bool   has_hostile = (p.nearest_hostile_id != 0);
    const float  hp          = hp_fraction(s);

    if (has_hostile && hp <= k_flee_threshold) {
        next = AIState::Flee;
        s.ai.target_id = p.nearest_hostile_id;
    } else if (has_hostile) {
        next = AIState::Engage;
        s.ai.target_id = p.nearest_hostile_id;
    } else if (s.ai.has_patrol_anchor) {
        next = AIState::Patrol;
        s.ai.target_id = 0;
    } else {
        next = AIState::Idle;
        s.ai.target_id = 0;
    }

    if (next != s.ai.state) {
        s.ai.state            = next;
        s.ai.state_entered_at = t_now;
    }

    // ---- act ----------------------------------------------------------
    // Translate state into a concrete behaviour. The behaviour layer
    // owns the actual rotation / throttle math; we just pick which one
    // and where to point it.
    switch (s.ai.state) {
        case AIState::Idle: {
            // No behaviour. Anything externally driving the kinematics
            // (legacy JSON motion) keeps running. Controller stays idle.
            s.behavior.kind = ShipBehavior::None;
            break;
        }
        case AIState::Patrol: {
            // Placeholder. Until OrbitAnchor lands this is "loiter near
            // anchor" without active control — same fallthrough as Idle.
            // Distinct state ID so future code can recognise patrol
            // ships by intent even before the behaviour is wired.
            s.behavior.kind = ShipBehavior::None;
            break;
        }
        case AIState::Engage: {
            const Ship* t = find_by_id(all_ships, s.ai.target_id);
            if (!t) { s.behavior.kind = ShipBehavior::None; break; }
            // Dynamic target: refresh target_pos every tick to track the
            // hostile's current position. This is the "the AI chases me
            // when I move" moment — distinct from a static PursueTarget
            // that points at a fixed waypoint.
            s.behavior.kind       = ShipBehavior::PursueTarget;
            s.behavior.target_pos = t->position;
            break;
        }
        case AIState::Flee: {
            const Ship* threat = find_by_id(all_ships, s.ai.target_id);
            if (!threat) { s.behavior.kind = ShipBehavior::None; break; }
            // Aim at a far-away point in the direction OPPOSITE the
            // threat. 50 km is well past any current radar range, so
            // PursueTarget's kinematic-arrival logic stays in
            // "full cruise" mode — the fleer doesn't decelerate as it
            // pulls away. If the threat keeps closing, target updates
            // each tick (re-projected from current relative geometry)
            // and the fleer keeps running.
            HMM_Vec3 away = HMM_SubV3(s.position, threat->position);
            const float len2 = HMM_DotV3(away, away);
            HMM_Vec3 unit = (len2 > 1e-6f)
                ? HMM_DivV3F(away, std::sqrt(len2))
                : HMM_V3(0.0f, 0.0f, 1.0f);
            s.behavior.kind       = ShipBehavior::PursueTarget;
            s.behavior.target_pos = HMM_AddV3(s.position, HMM_MulV3F(unit, 50000.0f));
            break;
        }
        case AIState::Count: break;  // unreachable
    }
}

const char* ship_ai::to_name(AIState st) {
    switch (st) {
        case AIState::Idle:   return "idle";
        case AIState::Patrol: return "patrol";
        case AIState::Engage: return "engage";
        case AIState::Flee:   return "flee";
        default:              return "?";
    }
}

AIState ship_ai::from_name(std::string_view s) {
    if (s == "idle")   return AIState::Idle;
    if (s == "patrol") return AIState::Patrol;
    if (s == "engage") return AIState::Engage;
    if (s == "flee")   return AIState::Flee;
    return AIState::Count;
}

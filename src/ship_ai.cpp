#include "ship_ai.h"

#include "armor.h"
#include "perception.h"
#include "shield.h"
#include "ship.h"
#include "ship_class.h"
#include "ship_sprite.h"   // for sprite->forward_speed read in lead prediction

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
    const bool  has_hostile = (p.nearest_hostile_id != 0);
    const float hp          = hp_fraction(s);

    // Personality gate. Cowards (merchants, civilians) prefer to flee
    // — but ONLY when something's actively hunting them. When no
    // hostile is locked-on, even a Coward will fight back rather than
    // run pre-emptively. Standard ships always engage until hurt.
    const bool is_coward =
        s.klass && s.klass->personality == AIPersonality::Coward;

    // "Is anyone Engage-locked on me right now?" Walks the ship list
    // looking for hostiles whose ai.state == Engage with target_id ==
    // s.id. BreakOff explicitly does NOT count — when a pursuer is
    // extending we treat that as "not currently being hunted", which
    // gives the prey a window to flip Coward->Engage and pile damage
    // on the disengaging hostile. That role-reversal is exactly the
    // dynamic Mike asked for: Tarsus runs while Talon presses,
    // counter-attacks while Talon extends.
    bool being_targeted = false;
    for (const Ship& other : all_ships) {
        if (&other == &s || !other.alive) continue;
        if (other.ai.target_id == s.id && other.ai.state == AIState::Engage) {
            being_targeted = true;
            break;
        }
    }

    // BreakOff parameters. Distance is constant; duration is randomised
    // per entry (5-10s) so the pursuer can't predict the exact moment
    // a fight resumes. Per-class tuning later (an ace pilot might
    // break off at 800m, a rookie at 400m).
    // 150m (was 600m) — only break when uncomfortably close. With the
    // wider trigger every brush past target distance was kicking the
    // AI into a 3-8s afterburner extension, which read as "every time
    // I get them near, they bail". Tighter threshold lets the AI
    // press attacks at typical engagement distances and only break
    // when an actual collision risk is imminent.
    constexpr float k_break_distance_m   = 150.0f;
    constexpr float k_break_duration_min = 3.0f;
    constexpr float k_break_duration_max = 8.0f;

    // Firing cap: max continuous-FIRING time before a forced break.
    // Randomised per Engage entry (3-6s) so the prey can't time a
    // counter-maneuver to the exact moment of the pursuer's break.
    // Trigger is bound to fire_guns held true (see firing_started_at
    // tracking below), NOT to time-in-Engage — a ship still closing
    // distance shouldn't burn its window.
    constexpr float k_firing_max_min = 3.0f;
    constexpr float k_firing_max_max = 6.0f;

    // Continuous-firing tracker. Reads controller.fire_guns from the
    // PREVIOUS frame's act phase: true means we shot last tick, false
    // means we held fire. Tracking belongs in transition phase so the
    // BreakOff decision can use it inline below.
    if (s.ai.state == AIState::Engage && s.controller.fire_guns) {
        if (s.ai.firing_started_at < 0.0f) {
            s.ai.firing_started_at = t_now;
        }
    } else {
        s.ai.firing_started_at = -1.0f;   // not Engaged or not firing
    }
    const bool firing_too_long = (s.ai.firing_started_at >= 0.0f)
        && (t_now - s.ai.firing_started_at) > s.ai.firing_max_duration_s;

    // Sticky-BreakOff: once committed, stay in until the (per-entry)
    // timer expires regardless of perception. Otherwise the very
    // condition that triggered BreakOff (target uncomfortably close)
    // would flip us right back to Engage on the NEXT tick and we'd
    // never escape the orbital tail-chase. After the timer the
    // normal cascade resumes — usually back to Engage with the same
    // hostile, but a fresh perception read can drop a fled target.
    const bool in_break = (s.ai.state == AIState::BreakOff)
                       && (t_now - s.ai.state_entered_at) < s.ai.break_duration_s;

    if (in_break) {
        next = AIState::BreakOff;   // hold the maneuver; target_id preserved
    } else if (has_hostile && hp <= k_flee_threshold) {
        // Critical-HP always flees, regardless of personality.
        next = AIState::Flee;
        s.ai.target_id = p.nearest_hostile_id;
    } else if (has_hostile && is_coward && being_targeted) {
        // Hunted Coward flees with evasion. Drops back to Engage on the
        // next tick after the pursuer enters BreakOff (being_targeted
        // flips false), then back to Flee when pursuer re-Engages.
        next = AIState::Flee;
        s.ai.target_id = p.nearest_hostile_id;
    } else if (has_hostile) {
        // Default to Engage; check the break triggers next.
        next = AIState::Engage;
        s.ai.target_id = p.nearest_hostile_id;

        // Engage -> BreakOff trigger 1: TOO CLOSE. Inside this radius
        // overshoot dynamics dominate (we plow through the target's
        // position, U-turn, plow back, repeat — the scissors).
        // Breaking out perpendicular gains separation for a clean
        // attack run from a fresh angle.
        if (const Ship* t = find_by_id(all_ships, s.ai.target_id); t) {
            const HMM_Vec3 to_t = HMM_SubV3(t->position, s.position);
            if (HMM_DotV3(to_t, to_t) < k_break_distance_m * k_break_distance_m) {
                next = AIState::BreakOff;
            }
        }

        // Engage -> BreakOff trigger 2: SUSTAINED FIRE. No ship
        // presses a fire button for more than its (per-entry random)
        // firing cap before extending. Bound to actual blasting so a
        // long approach with no firing solution doesn't burn the window.
        if (firing_too_long) {
            next = AIState::BreakOff;
        }
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

        // ---- per-state entry actions -------------------------------
        if (next == AIState::Engage) {
            // Randomise this Engage's firing cap in [k_firing_max_min,
            // k_firing_max_max]. Pseudo-random off (t_now, ship.id) so
            // multiple ships and back-to-back engagements don't all
            // expire on the same beat — keeps the rhythm desynced.
            const float r = std::fmod(t_now * 11.3f + (float)s.id * 5.17f, 1.0f);
            s.ai.firing_max_duration_s = k_firing_max_min
                                       + r * (k_firing_max_max - k_firing_max_min);
            s.ai.firing_started_at     = -1.0f;   // fresh burst
        }
        if (next == AIState::BreakOff) {
            // Randomise this break's duration in [k_break_duration_min,
            // k_break_duration_max]. Pseudo-random off (t_now, ship.id)
            // so different ships and different break occurrences land
            // on different durations. The pursuer can't predict the
            // exact moment of re-engagement, which keeps timing-based
            // counters from feeling cheap.
            const float r = std::fmod(t_now * 13.7f + (float)s.id * 7.31f, 1.0f);
            s.ai.break_duration_s = k_break_duration_min
                                  + r * (k_break_duration_max - k_break_duration_min);
            // Pick a body-frame break direction at entry, cache it for
            // the duration. Picking fresh every tick would just
            // reproduce the orbital chase we're escaping.
            //
            // Four cardinal axes (up / down / left / right) chosen
            // pseudo-randomly off ship.id + t_now so different ships
            // and different break occurrences pick different axes —
            // keeps repeated breaks from looking scripted. ±Z (forward/
            // back) intentionally excluded: forward break is just
            // "keep flying", backward doesn't map onto our forward-
            // only thrust model.
            const int choice =
                ((int)std::floor(t_now * 7.13f) + (int)s.id * 3) & 3;
            HMM_Vec3 body_dir;
            switch (choice) {
                case 0:  body_dir = HMM_V3( 0.0f,  1.0f, 0.0f); break;  // pitch up
                case 1:  body_dir = HMM_V3( 0.0f, -1.0f, 0.0f); break;  // pitch down
                case 2:  body_dir = HMM_V3(-1.0f,  0.0f, 0.0f); break;  // break left
                default: body_dir = HMM_V3( 1.0f,  0.0f, 0.0f); break;  // break right
            }
            const HMM_Mat4 R = HMM_QToM4(s.orientation);
            const HMM_Vec4 v = HMM_MulM4V4(R, HMM_V4(body_dir.X, body_dir.Y, body_dir.Z, 0));
            const HMM_Vec3 lateral_world = HMM_V3(v.X, v.Y, v.Z);

            // Blend with away-from-target so we always gain separation,
            // even if the chosen lateral happens to point at the target
            // (e.g. target directly above + we picked "up"). 50/50 mix
            // of perpendicular + away gives a satisfying diagonal
            // escape; renormalise to keep unit length.
            HMM_Vec3 away_world = HMM_V3(0, 0, 1);
            if (const Ship* t = find_by_id(all_ships, s.ai.target_id); t) {
                HMM_Vec3 to_t = HMM_SubV3(t->position, s.position);
                const float l2 = HMM_DotV3(to_t, to_t);
                if (l2 > 1e-6f) {
                    away_world = HMM_DivV3F(HMM_MulV3F(to_t, -1.0f),
                                            std::sqrt(l2));
                }
            }
            const HMM_Vec3 mix = HMM_AddV3(lateral_world, away_world);
            const float ml2 = HMM_DotV3(mix, mix);
            s.ai.break_dir_world = (ml2 > 1e-6f)
                ? HMM_DivV3F(mix, std::sqrt(ml2))
                : lateral_world;
        }
    }

    // ---- act ----------------------------------------------------------
    // Translate state into a concrete behaviour. The behaviour layer
    // owns the actual rotation / throttle math; we just pick which one
    // and where to point it.
    switch (s.ai.state) {
        case AIState::Idle: {
            s.behavior.kind          = ShipBehavior::None;
            s.controller.fire_guns   = false;
            s.controller.afterburner = false;
            s.controller.speed_scale = 1.0f;
            break;
        }
        case AIState::Patrol: {
            // Placeholder. Until OrbitAnchor lands this is "loiter near
            // anchor" without active control — same fallthrough as Idle.
            s.behavior.kind          = ShipBehavior::None;
            s.controller.fire_guns   = false;
            s.controller.afterburner = false;
            s.controller.speed_scale = 1.0f;
            break;
        }
        case AIState::BreakOff: {
            // Aim 20 km out along the cached break direction. ChaseTarget
            // (not PursueTarget) so the kinematic-arrival ramp doesn't
            // engage. Afterburner flag is set so the controller picks
            // afterburner_speed (Talon: 1000 m/s, 2.5x cruise) — that's
            // the visible "plow out fast" Mike asked for. fire_guns
            // off because we're explicitly disengaging this maneuver.
            //
            // Distance is sized for k_break_duration_s × afterburner_speed
            // with margin: 10 s at 1000 m/s = 10 km traveled, so a 20 km
            // waypoint stays unreached for the whole burn. Reaching the
            // waypoint mid-break would trigger a U-turn back to it and
            // ruin the extension feel.
            const HMM_Vec3 break_target = HMM_AddV3(s.position,
                HMM_MulV3F(s.ai.break_dir_world, 20000.0f));
            s.behavior.kind          = ShipBehavior::ChaseTarget;
            s.behavior.target_pos    = break_target;
            s.controller.fire_guns   = false;
            s.controller.afterburner = true;
            s.controller.speed_scale = 1.0f;   // full afterburner — extension
            break;
        }
        case AIState::Engage: {
            const Ship* t = find_by_id(all_ships, s.ai.target_id);
            if (!t || !t->alive) {
                // Target died or was never resolvable. Drop it so the
                // next perception tick can pick a fresh hostile.
                s.ai.target_id = 0;
                s.behavior.kind = ShipBehavior::None;
                s.controller.fire_guns = false;
                break;
            }

            // ---- lead prediction ---------------------------------------
            // Aim at where the target WILL BE by the time projectiles
            // arrive, not where it is right now. This is the basic
            // intercept math every fighter AI needs: without it the
            // pursuer perpetually lags the target's lateral motion and
            // settles into a tail-chase orbit (you can never catch what
            // you're always chasing the trail of).
            //
            // Mental projectile speed = average of this ship's mounts'
            // muzzle velocities. Cheap; uses the average rather than
            // best-gun speed because the AI's firing is bursty across
            // mixed mounts and a tighter solution feels right when most
            // mounts agree. Falls back to 1100 m/s (Mass Driver) if
            // the ship has no usable mounts (placeholder ships).
            float avg_proj_speed = 0.0f;
            int   n_complete = 0;
            for (const auto& m : s.mounts) {
                if ((int)m.type < 0 || (int)m.type >= kGunTypeCount) continue;
                const GunStats& gs = g_gun_stats[(int)m.type];
                if (!gs.complete) continue;
                avg_proj_speed += gs.speed_mps;
                ++n_complete;
            }
            avg_proj_speed = (n_complete > 0) ? (avg_proj_speed / n_complete) : 1100.0f;

            // Target velocity in world frame. NPCs: orientation*+Z * speed.
            // Player approximated as stationary (camera doesn't expose a
            // velocity field; close enough for v1 — typical encounter
            // distances dwarf typical player flight speed).
            HMM_Vec3 t_vel = HMM_V3(0.0f, 0.0f, 0.0f);
            if (t->sprite) {
                const HMM_Mat4 tR  = HMM_QToM4(t->orientation);
                const HMM_Vec4 tf4 = HMM_MulM4V4(tR, HMM_V4(0, 0, 1, 0));
                t_vel = HMM_MulV3F(HMM_V3(tf4.X, tf4.Y, tf4.Z), t->sprite->forward_speed);
            }

            // Iterate-once lead solution: t_int = current_distance /
            // proj_speed. A two-iter refinement (use lead position to
            // recompute distance) tightens the answer for fast-crossing
            // targets, but at our engagement geometry the single-pass
            // version is within a few metres — not worth the extra math.
            const HMM_Vec3 to_target = HMM_SubV3(t->position, s.position);
            const float    dist      = std::sqrt(HMM_DotV3(to_target, to_target));
            const float    t_int     = (avg_proj_speed > 1.0f) ? dist / avg_proj_speed : 0.0f;
            const HMM_Vec3 lead_pos  = HMM_AddV3(t->position, HMM_MulV3F(t_vel, t_int));

            // ---- behaviour: chase the LEAD point at full cruise --------
            // ChaseTarget (not PursueTarget) so the controller doesn't
            // decelerate as we approach — that's what produced the
            // "two ships orbit each other forever" pattern in the v1
            // Engage. With ChaseTarget the Talon plows straight at the
            // intercept point and overshoots when it can't quite match
            // the Tarsus's jink, which is the desired dogfight feel.
            s.behavior.kind       = ShipBehavior::ChaseTarget;
            s.behavior.target_pos = lead_pos;

            // ---- firing: lead-corrected cone test ----------------------
            // 15° cone around our nose, but tested against the LEAD
            // direction (where we should be aiming) rather than the
            // raw target position. Otherwise we'd never fire at a
            // crossing target — the cone would chase the wrong point.
            constexpr float k_engage_cos_threshold = 0.966f;  // cos(15°)
            const HMM_Vec3 to_lead   = HMM_SubV3(lead_pos, s.position);
            const float    lead_d2   = HMM_DotV3(to_lead, to_lead);
            const float    weapons_r = s.klass ? s.klass->weapons_range : 0.0f;
            const float    weapons_r2 = weapons_r * weapons_r;

            const HMM_Mat4 R    = HMM_QToM4(s.orientation);
            const HMM_Vec4 fwd4 = HMM_MulM4V4(R, HMM_V4(0, 0, 1, 0));
            const HMM_Vec3 fwd  = HMM_V3(fwd4.X, fwd4.Y, fwd4.Z);

            bool in_arc = false;
            if (lead_d2 > 1e-6f && lead_d2 < weapons_r2) {
                const HMM_Vec3 lead_unit = HMM_DivV3F(to_lead, std::sqrt(lead_d2));
                in_arc = HMM_DotV3(fwd, lead_unit) >= k_engage_cos_threshold;
            }
            s.controller.fire_guns   = in_arc;
            s.controller.afterburner = false;
            // Throttle DOWN during the attack run. Full cruise produces
            // the overshoot/orbit pattern (covered ~7 m/frame at 60 Hz)
            // — the AI's max yaw can't track jinking targets at that
            // speed. Half-throttle gives time on target during firing
            // bursts; the BreakOff state restores full speed when it's
            // time to extend.
            s.controller.speed_scale = 0.5f;
            break;
        }
        case AIState::Flee: {
            const Ship* threat = find_by_id(all_ships, s.ai.target_id);
            if (!threat || !threat->alive) {
                s.ai.target_id  = 0;
                s.behavior.kind = ShipBehavior::None;
                break;
            }
            s.controller.fire_guns   = false;   // running, not shooting
            s.controller.afterburner = false;   // cruise weave, not all-out burn
            s.controller.speed_scale = 1.0f;    // full cruise to escape

            // Direction directly away from the threat — the escape
            // baseline before evasion math.
            HMM_Vec3 away = HMM_SubV3(s.position, threat->position);
            const float len2 = HMM_DotV3(away, away);
            HMM_Vec3 unit = (len2 > 1e-6f)
                ? HMM_DivV3F(away, std::sqrt(len2))
                : HMM_V3(0.0f, 0.0f, 1.0f);

            // Home tether: blend escape direction with a pull back
            // toward patrol_anchor, weighted by how far from home the
            // ship has wandered. At home the blend is 0 (pure flee);
            // farther out, an increasing fraction of the desired
            // direction is "toward home" so the ship orbits-and-returns
            // instead of running to infinity. Capped at 0.7 so SOME
            // flee instinct always survives — at full home-pull a
            // merchant would happily fly back through its attacker.
            //
            // Tether radius (5 km) is the "trying to stay home"
            // engagement bubble. Adjust per-class via patrol_anchor
            // setup if some ships should roam farther.
            if (s.ai.has_patrol_anchor) {
                const HMM_Vec3 to_home = HMM_SubV3(s.ai.patrol_anchor, s.position);
                const float    dh2     = HMM_DotV3(to_home, to_home);
                if (dh2 > 1.0f) {
                    const float    dh       = std::sqrt(dh2);
                    const HMM_Vec3 home_dir = HMM_DivV3F(to_home, dh);
                    constexpr float k_tether_radius_m = 5000.0f;
                    constexpr float k_max_pull        = 0.7f;
                    const float blend = std::clamp(
                        dh / k_tether_radius_m, 0.0f, k_max_pull);
                    HMM_Vec3 mixed = HMM_AddV3(
                        HMM_MulV3F(unit,     1.0f - blend),
                        HMM_MulV3F(home_dir, blend));
                    const float mlen2 = HMM_DotV3(mixed, mixed);
                    if (mlen2 > 1e-6f) unit = HMM_DivV3F(mixed, std::sqrt(mlen2));
                }
            }

            // Evasion: jink perpendicular to the escape vector on a
            // sinusoid. Without this the fleer flies a clean line and
            // is trivial to track + shoot. With ~25° amplitude on a
            // ~3-second period, the fleer weaves enough that a pursuer
            // with the AI's 15° firing cone has to chase the wobble
            // (and a player with twitchier aim has a better fight).
            //
            // Phase + period jitter via ship.id so multiple fleers
            // don't all jink in unison. Perpendicular axis is escape ×
            // world-up; degenerate (escape vertical) falls back to
            // escape × world-right.
            HMM_Vec3 perp = HMM_Cross(unit, HMM_V3(0.0f, 1.0f, 0.0f));
            float perp_len2 = HMM_DotV3(perp, perp);
            if (perp_len2 < 1e-6f) {
                perp = HMM_Cross(unit, HMM_V3(1.0f, 0.0f, 0.0f));
                perp_len2 = HMM_DotV3(perp, perp);
            }
            perp = HMM_DivV3F(perp, std::sqrt(std::max(perp_len2, 1e-12f)));

            constexpr float kPi = 3.14159265358979323846f;
            const float period_s = 2.5f + (float)(s.id % 4) * 0.4f;   // 2.5..3.7s
            const float phase    = t_now * (2.0f * kPi / period_s)
                                   + (float)s.id * 1.3f;
            const float jink     = std::sin(phase) * 0.45f;            // ~25° lateral

            // Compose escape + lateral jink, renormalise. Adding a
            // perpendicular component shrinks the unit vector slightly
            // and PursueTarget's projection math wants unit length.
            HMM_Vec3 evade = HMM_AddV3(unit, HMM_MulV3F(perp, jink));
            const float evade_len2 = HMM_DotV3(evade, evade);
            evade = (evade_len2 > 1e-6f)
                ? HMM_DivV3F(evade, std::sqrt(evade_len2))
                : unit;

            // 50 km out — well past any radar range; PursueTarget's
            // kinematic-arrival math stays in "full cruise" mode
            // (always far, never decelerates). Recomputed every tick
            // so the fleer wobbles dynamically as the phase advances.
            s.behavior.kind       = ShipBehavior::PursueTarget;
            s.behavior.target_pos = HMM_AddV3(s.position, HMM_MulV3F(evade, 50000.0f));
            break;
        }
        case AIState::Count: break;  // unreachable
    }
}

const char* ship_ai::to_name(AIState st) {
    switch (st) {
        case AIState::Idle:     return "idle";
        case AIState::Patrol:   return "patrol";
        case AIState::Engage:   return "engage";
        case AIState::BreakOff: return "breakoff";
        case AIState::Flee:     return "flee";
        default:                return "?";
    }
}

AIState ship_ai::from_name(std::string_view s) {
    if (s == "idle")     return AIState::Idle;
    if (s == "patrol")   return AIState::Patrol;
    if (s == "engage")   return AIState::Engage;
    if (s == "breakoff") return AIState::BreakOff;
    if (s == "flee")     return AIState::Flee;
    return AIState::Count;
}

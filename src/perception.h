#pragma once
// -----------------------------------------------------------------------------
// perception.h — what each ship can see this tick.
//
// The AI's "what's around me?" query, refreshed once per perception tick.
// For every alive ship we walk every other alive ship, distance-cull
// against the observer's class-defined radar_range, and bucket the
// survivors into Allied / Neutral / Hostile via the faction matrix
// (faction.h). The result lands in Ship::perception so behaviours +
// state machines can consult it without re-walking the world.
//
// Design notes:
//
//   * Refresh frequency. Privateer ran AI at ~10 Hz; we run perception
//     every render frame for now (60 Hz at the Mac's display rate).
//     Costs O(N²) over the world's ship count — fine for the tens of
//     ships an encounter holds, will need bucketing or a kd-tree if we
//     ever push past a few hundred. Easy hook to add: a fixed-rate
//     accumulator in main.cpp that calls perception::tick at 30 Hz
//     instead of every render frame.
//
//   * Symmetry. NPC-vs-NPC stance is symmetric so each pair gets its
//     stance value computed twice today — once from each observer's
//     POV. Cheap (one matrix lookup), trivially parallelisable, not
//     worth optimising until the ship count gets serious.
//
//   * Player handling. The player isn't yet a Ship in g.ships, so AI
//     can't "see" the player via this pipeline. That's a deliberate
//     scope hold — the player-as-ship refactor is its own commit and
//     unlocks player perception cleanly. For now perception is purely
//     NPC-vs-NPC, which is enough to validate the architecture.
//
//   * Quick-access fields. nearest_hostile_id / nearest_friend_id are
//     the two queries every later layer wants ("who do I shoot at?",
//     "who's my wing-leader?"). Computing them inline is O(N) free
//     piggy-backed on the same scan, way cheaper than a separate sort
//     pass over the visible list. Same idea for the count buckets
//     (n_hostile / n_allied / n_neutral) — used by Flee transitions
//     ("am I outnumbered?") and HUD displays.
// -----------------------------------------------------------------------------

#include "faction.h"

#include <HandmadeMath.h>
#include <cstdint>
#include <vector>

struct Ship;

struct PerceivedContact {
    uint32_t ship_id    = 0;
    Stance   stance     = Stance::Neutral;
    float    distance_m = 0.0f;
    HMM_Vec3 to_unit    = { 0.0f, 0.0f, 1.0f };  // observer -> contact, unit
};

struct ShipPerception {
    std::vector<PerceivedContact> visible;   // every ship in radar range

    // Bucket counts — answers "am I outnumbered?" without re-scanning.
    int n_hostile = 0;
    int n_allied  = 0;
    int n_neutral = 0;

    // Quick-access targets for the two queries every AI layer wants.
    // ID 0 is reserved as "none" by ship::spawn (real IDs start at 1).
    uint32_t nearest_hostile_id   = 0;
    float    nearest_hostile_dist = 1e30f;
    uint32_t nearest_friend_id    = 0;
    float    nearest_friend_dist  = 1e30f;
};

namespace perception {

// Refresh every alive ship's `perception` field. Call once per frame
// (or 30 Hz tick when we wire that), BEFORE ship::tick — behaviours
// may consult perception when picking desired_forward / desired_speed.
void tick(std::vector<Ship>& ships);

} // namespace perception

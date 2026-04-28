#include "perception.h"

#include "ship.h"
#include "ship_class.h"
#include "ship_sprite.h"

#include <cmath>

// Stance lookup that handles all four (player, NPC) × (player, NPC)
// cases. NPC-vs-NPC consults the symmetric matrix; anything involving
// the player folds the relevant faction's baseline + rep through the
// -25 / +25 thresholds. The player-self case is impossible (only one
// player ship) — perception::tick already skips self-observation.
static Stance classify_pair(const Ship& observer, const Ship& other,
                             const PlayerReputation& rep) {
    if (other.is_player) {
        // "How does this NPC observer feel about the player?"
        return faction::stance_npc_vs_player(observer.faction, rep);
    }
    if (observer.is_player) {
        // "What stance is this NPC contact taking toward me?" — same
        // lookup, just inverted, so the player's perception list
        // colours hostiles/allies the way the NPC's AI sees the player.
        return faction::stance_npc_vs_player(other.faction, rep);
    }
    return faction::stance_npc_vs_npc(observer.faction, other.faction);
}

void perception::tick(std::vector<Ship>& ships, const PlayerReputation& player_rep) {
    // O(N²) double-loop. Each observer's perception is rebuilt from
    // scratch — no incremental updates today (fast enough at N ≤ 50).
    // Inside the inner loop we early-out on dead/sprite-less ships and
    // distance-cull against the OBSERVER's radar range; the contact's
    // own range doesn't matter here (we don't need them to see us).
    for (Ship& observer : ships) {
        ShipPerception& p = observer.perception;

        // Reset every tick — a contact that drifted out of radar range
        // since last tick must NOT linger as a stale entry.
        p.visible.clear();
        p.n_hostile = p.n_allied = p.n_neutral = 0;
        p.nearest_hostile_id   = p.nearest_friend_id  = 0;
        p.nearest_hostile_dist = p.nearest_friend_dist = 1e30f;

        if (!observer.alive) continue;

        // Player has no class -> use a generous default radar range so
        // the player's HUD shows nearby contacts. Bumped to 35 km so
        // the targeting cycle (T key) reaches well past most engagement
        // distances — chasing ships through their afterburner extensions
        // routinely opens the gap to 10+ km and you don't want to lose
        // your target lock just because you can't see far enough. NPCs
        // still use their class radar (Privateer-canonical 25 km), so
        // the player has a small "my HUD is enhanced" advantage.
        constexpr float k_player_radar_m = 35000.0f;
        const float radar_r = observer.is_player ? k_player_radar_m
                            : (observer.klass ? observer.klass->radar_range : 0.0f);
        if (radar_r <= 0.0f) continue;
        const float    r2     = radar_r * radar_r;
        const HMM_Vec3 my_pos = observer.position;

        for (Ship& other : ships) {
            if (&other == &observer) continue;   // never perceive self
            if (!other.alive)        continue;
            // Position must have been synced this frame — for NPCs by
            // ship::sync_from_sprite, for the player by main's camera
            // copy. A ship with neither still has a default-zero
            // position; that's a bug in the caller, but harmless here
            // (it just shows up as a contact at origin).

            const HMM_Vec3 to = HMM_SubV3(other.position, my_pos);
            const float    d2 = HMM_DotV3(to, to);
            if (d2 > r2) continue;                // out of radar range

            const float    d  = std::sqrt(d2);
            // Guard against the sub-mm case (two ships overlapping) so
            // we don't divide by zero. The to_unit fallback (+Z) is
            // arbitrary but harmless — at this distance the AI's
            // pursuit math doesn't meaningfully care about direction.
            const HMM_Vec3 to_unit = (d > 1e-3f) ? HMM_DivV3F(to, d)
                                                  : HMM_V3(0.0f, 0.0f, 1.0f);

            PerceivedContact c;
            c.ship_id    = other.id;
            c.distance_m = d;
            c.to_unit    = to_unit;
            c.stance     = classify_pair(observer, other, player_rep);
            p.visible.push_back(c);

            switch (c.stance) {
                case Stance::Hostile:
                    ++p.n_hostile;
                    if (d < p.nearest_hostile_dist) {
                        p.nearest_hostile_dist = d;
                        p.nearest_hostile_id   = other.id;
                    }
                    break;
                case Stance::Allied:
                    ++p.n_allied;
                    if (d < p.nearest_friend_dist) {
                        p.nearest_friend_dist = d;
                        p.nearest_friend_id   = other.id;
                    }
                    break;
                case Stance::Neutral:
                    ++p.n_neutral;
                    break;
            }
        }
    }
}

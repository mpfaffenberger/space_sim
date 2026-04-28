#include "perception.h"

#include "ship.h"
#include "ship_class.h"
#include "ship_sprite.h"

#include <cmath>

void perception::tick(std::vector<Ship>& ships) {
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

        if (!observer.alive || !observer.sprite || !observer.klass) continue;

        const float    radar_r = observer.klass->radar_range;
        const float    r2      = radar_r * radar_r;
        const HMM_Vec3 my_pos  = observer.sprite->position;

        for (Ship& other : ships) {
            if (&other == &observer) continue;   // never perceive self
            if (!other.alive || !other.sprite)   continue;

            const HMM_Vec3 to = HMM_SubV3(other.sprite->position, my_pos);
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
            c.stance     = faction::stance_npc_vs_npc(observer.faction, other.faction);
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

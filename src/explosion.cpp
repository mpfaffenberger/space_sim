#include "explosion.h"

void explosion::spawn(std::vector<Explosion>& explosions, const HMM_Vec3& pos,
                      const HMM_Vec3& ring_right, const HMM_Vec3& ring_up) {
    Explosion e;
    e.position   = pos;
    e.age_s      = 0.0f;
    e.lifetime_s = 1.2f;
    e.alive      = true;
    e.ring_right = ring_right;
    e.ring_up    = ring_up;
    explosions.push_back(e);
}

void explosion::tick(std::vector<Explosion>& explosions, float dt) {
    for (Explosion& e : explosions) {
        if (!e.alive) continue;
        e.age_s += dt;
        if (e.age_s >= e.lifetime_s) e.alive = false;
    }
    // Compact alive entries to the front.
    size_t w = 0;
    for (size_t r = 0; r < explosions.size(); ++r) {
        if (explosions[r].alive) {
            if (w != r) explosions[w] = explosions[r];
            ++w;
        }
    }
    explosions.resize(w);
}

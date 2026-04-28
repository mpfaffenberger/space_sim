#include "faction.h"

#include <algorithm>
#include <cstdio>

Stance g_faction_stance[kFactionCount][kFactionCount] = {};
int8_t g_faction_baseline_to_player[kFactionCount] = {};

namespace {

// Symmetric stance defaults — the matrix Mike approved in design review.
// Reading order: H = hostile, N = neutral, A = allied. Diagonal is A
// (faction always allied to itself; wingmen rely on this).
//
//             Civ  Mer  Cnf  Mil  Hnt  Pir  Ret  Kil
constexpr char k_stance_grid[kFactionCount][kFactionCount] = {
    /* Civ */ { 'A', 'N', 'N', 'N', 'N', 'H', 'H', 'H' },
    /* Mer */ { 'N', 'A', 'A', 'A', 'N', 'H', 'H', 'H' },
    /* Cnf */ { 'N', 'A', 'A', 'A', 'N', 'H', 'H', 'H' },
    /* Mil */ { 'N', 'A', 'A', 'A', 'N', 'H', 'H', 'H' },
    /* Hnt */ { 'N', 'N', 'N', 'N', 'A', 'H', 'H', 'H' },
    /* Pir */ { 'H', 'H', 'H', 'H', 'H', 'A', 'N', 'H' },
    /* Ret */ { 'H', 'H', 'H', 'H', 'H', 'N', 'A', 'H' },
    /* Kil */ { 'H', 'H', 'H', 'H', 'H', 'H', 'H', 'A' },
};

constexpr Stance char_to_stance(char c) {
    return (c == 'A') ? Stance::Allied
         : (c == 'H') ? Stance::Hostile
         :              Stance::Neutral;
}

// Faction baseline opinion of a stranger (no rep accumulated yet). Confed
// stay professional (0). Pirates assume you're prey (-30). Kilrathi treat
// any non-Kilrathi as the enemy (-100); even max rep won't make them allies.
constexpr int8_t k_baseline[kFactionCount] = {
    /* Civ      */ +10,
    /* Merchant */ +10,
    /* Confed   */   0,
    /* Militia  */   0,
    /* Hunter   */   0,
    /* Pirate   */ -30,
    /* Retro    */ -50,
    /* Kilrathi */ -100,
};

constexpr const char* k_names[kFactionCount] = {
    "civilian", "merchant", "confed", "militia",
    "hunter", "pirate", "retro", "kilrathi",
};

bool g_initialised = false;

} // namespace

void faction::init() {
    if (g_initialised) return;
    g_initialised = true;

    int hostile_pairs = 0;
    int allied_pairs  = 0;
    for (int a = 0; a < kFactionCount; ++a) {
        for (int b = 0; b < kFactionCount; ++b) {
            const Stance st = char_to_stance(k_stance_grid[a][b]);
            g_faction_stance[a][b] = st;
            // Count each unordered pair once (a < b) for the log line.
            if (a < b) {
                if (st == Stance::Hostile) ++hostile_pairs;
                if (st == Stance::Allied)  ++allied_pairs;
            }
        }
        g_faction_baseline_to_player[a] = k_baseline[a];
    }

    std::printf("[faction] %d factions, %d hostile pairs, %d allied pairs\n",
                kFactionCount, hostile_pairs, allied_pairs);
}

Faction faction::from_name(std::string_view s) {
    for (int i = 0; i < kFactionCount; ++i) {
        if (s == k_names[i]) return (Faction)i;
    }
    return Faction::Count;  // sentinel "unknown"
}

const char* faction::to_name(Faction f) {
    if ((int)f < 0 || (int)f >= kFactionCount) return "?";
    return k_names[(int)f];
}

Stance faction::stance_npc_vs_npc(Faction a, Faction b) {
    return g_faction_stance[(int)a][(int)b];
}

Stance faction::stance_npc_vs_player(Faction npc, const PlayerReputation& r) {
    const int eff = std::clamp(
        (int)g_faction_baseline_to_player[(int)npc] + (int)r.rep[(int)npc],
        -100, +100);
    if (eff <= -25) return Stance::Hostile;
    if (eff >= +25) return Stance::Allied;
    return Stance::Neutral;
}

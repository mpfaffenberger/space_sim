#pragma once
// -----------------------------------------------------------------------------
// faction.h — who hates whom.
//
// Privateer's universe has eight factions. Most NPC behaviour is gated on
// "is this other ship hostile to me?" which is a 2-axis lookup:
//
//   * NPC vs NPC: a static 8x8 stance matrix. A Pirate sees a Confed and
//     attacks; a Merchant sees another Merchant and ignores it.
//   * NPC vs PLAYER: the matrix doesn't apply because the player has no
//     faction. Instead, every faction tracks a *reputation* score with the
//     player (-100..+100), combined with that faction's *baseline* feeling
//     about strangers. Stance is the thresholded sum of the two.
//
// This split matches the original game: you can't BECOME Confed by killing
// pirates, but Confed can come to like (or hate) you individually.
//
// Stance is symmetric for v1. Privateer was actually asymmetric in places
// (Retros hated everyone but not everyone hated Retros equally) — fine to
// add when we need it. The matrix is mutable at runtime so future event
// scripts ("you destroyed the Steltek artefact, all Retros now Hostile to
// you") can poke it without recompile.
// -----------------------------------------------------------------------------

#include <cstdint>
#include <string>
#include <string_view>

enum class Faction : uint8_t {
    Civilian = 0,
    Merchant,
    Confed,
    Militia,
    Hunter,
    Pirate,
    Retro,
    Kilrathi,
    Count
};
constexpr int kFactionCount = (int)Faction::Count;

enum class Stance : uint8_t { Allied, Neutral, Hostile };

// 8x8 symmetric matrix, populated by faction::init(). Indexed
// [faction_a][faction_b]. Mutable by design — runtime events may flip
// individual cells (e.g. mission scripts).
extern Stance g_faction_stance[kFactionCount][kFactionCount];

// Per-player rep score with each faction. Range is [-100, +100]; defaults
// to 0 on a fresh save. Combined with `g_faction_baseline_to_player[]` to
// derive the runtime stance.
struct PlayerReputation {
    int8_t rep[kFactionCount] = {};   // zeroed
};

// Faction-specific opinion of a stranger. Confed treat newcomers neutrally
// (0), Pirates dislike them (-30), Kilrathi treat any non-Kilrathi as prey
// (-100, no rep can save you).
extern int8_t g_faction_baseline_to_player[kFactionCount];

namespace faction {

// Populate the static tables (stance matrix + player baselines). Idempotent.
// Logs one line: `[faction] 8 factions, X hostile pairs, Y allied pairs`.
void init();

// String <-> enum. Names are lowercase canonical: "civilian", "merchant",
// "confed", "militia", "hunter", "pirate", "retro", "kilrathi". Returns
// Faction::Count on unknown input (call sites should treat that as error).
Faction     from_name(std::string_view s);
const char* to_name(Faction f);

// The two queries the AI layer cares about. NPC-vs-NPC consults the static
// matrix; NPC-vs-PLAYER folds rep + baseline into thresholds.
//   eff = clamp(baseline + rep, -100, +100)
//   eff <= -25  -> Hostile
//   eff >= +25  -> Allied
//   else        -> Neutral
Stance stance_npc_vs_npc(Faction a, Faction b);
Stance stance_npc_vs_player(Faction npc, const PlayerReputation& rep);

} // namespace faction

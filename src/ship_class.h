#pragma once
// -----------------------------------------------------------------------------
// ship_class.h — per-ship-type design data ("the Talon weighs X").
//
// One ShipClass per kind of ship (talon, tarsus, centurion, ...). Loaded
// once at startup from assets/ships/<name>/ship.json next to the existing
// atlas_manifest.json — they're the visual side and stat side of the
// same logical ship. Pointers into g_ship_classes are stable for the
// program's lifetime; per-instance Ship structs (next commit) will hold
// `const ShipClass*` rather than copying.
//
// Mobility numbers come straight from the docs/privateer_ship_data.json
// canonical table: top speed in m/s (treating "kps" as flavour),
// acceleration + YPR as descriptive tiers (mobility.h supplies the
// multipliers). Armor in cm is per-facing (Fore/Aft/Side, sides
// symmetric L=R).
//
// Out of scope for v1:
//   * upgrade economy. ShipClass declares max engine/shield levels but
//     nothing reads them yet — the per-class default loadout is
//     authoritative for combat balance.
//   * turrets. Centurion etc. have rear/top/bottom mounts; we ignore
//     them and treat their fixed forward guns as the full loadout.
//   * cargo. The Galaxy carries 150 units, the Tarsus 100 — fields are
//     loaded so the trading layer can read them later, but combat
//     doesn't care today.
// -----------------------------------------------------------------------------

#include "faction.h"
#include "gun.h"
#include "mobility.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct ShieldType;
struct ArmorType;

struct ShipClass {
    // ---- identity ------------------------------------------------------
    std::string name;             // lowercase: "talon", "tarsus" — used as registry key
    std::string display_name;     // "Talon", "Tarsus"
    std::string class_label;      // "Light Fighter (used by militia, ...)"
    std::string atlas_manifest;   // path to existing sprite atlas, e.g.
                                  // "ships/talon/atlas_manifest.json"
    Faction default_faction = Faction::Civilian;

    // ---- hull (cm of durasteel) ----------------------------------------
    // Base armor that's always present. Add ArmorType::xxx_cm for the
    // total per-facing protection of an instance.
    float armor_fore_cm = 10.0f;
    float armor_aft_cm  = 10.0f;
    float armor_side_cm =  8.0f;

    // ---- mobility ------------------------------------------------------
    float        cruise_speed       = 300.0f;   // m/s top speed without afterburner
    float        afterburner_speed  = 0.0f;     // 0 = no afterburner fitted
    MobilityTier acceleration       = MobilityTier::Average;
    MobilityTier max_ypr            = MobilityTier::Average;

    // ---- sensing / engagement -----------------------------------------
    // Privateer-canonical radar reach. weapons_range is a soft AI hint —
    // "engage within this distance". Each gun has its own true range_m.
    float radar_range   = 25000.0f;   // m
    float weapons_range =  3000.0f;   // m

    // ---- slot caps for the upgrade economy (informational v1) ---------
    uint8_t max_engine_level = 1;
    uint8_t max_shield_level = 1;

    // ---- default fitted loadout ---------------------------------------
    // Each instance starts with these unless the spawner overrides. The
    // string fields are looked up against the shield/armor tables at
    // ShipClass load time and resolved to pointers below; if the lookup
    // fails the pointer is null and the loader logs a warning.
    std::string default_shield_name;
    std::string default_armor_name;
    const ShieldType* default_shield = nullptr;
    const ArmorType*  default_armor  = nullptr;

    std::vector<GunMount> default_guns;

    // ---- energy --------------------------------------------------------
    float energy_max      = 200.0f;   // GJ
    float energy_recharge =  30.0f;   // GJ/s

    // ---- cargo (trading layer; combat ignores) ------------------------
    int cargo_units      = 0;
    int cargo_units_max  = 0;
};

namespace ship_class {

// Scan `assets/ships/*/ship.json`, build the registry. Logs one line per
// ship loaded plus a final summary. Safe to call before or after the
// gun/shield/armor table loaders, but they should all be loaded before
// any ship instance is spawned. Returns the count of successfully
// loaded ship classes (0 means nothing in assets/ships had a ship.json).
int load_all(const std::string& ships_dir);

// Look up by registry key (e.g. "talon"). Returns nullptr on miss.
// Pointers are stable for the program's lifetime.
const ShipClass*               find(std::string_view name);
const std::vector<ShipClass>&  all();

} // namespace ship_class

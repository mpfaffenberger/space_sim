#pragma once
// -----------------------------------------------------------------------------
// shield.h — installable shield generators.
//
// Unlike guns (a fixed enum because code references specific types),
// shields are open-ended fitted equipment — there are 13 variants in the
// source data (Generators 1..7, Light/Medium/Heavy, Capital, Weak Light,
// Steltek Drone) and a future expansion could add more without a recompile.
// So shields are stored as a runtime vector keyed by name, and a Ship
// instance carries a `const ShieldType*` into that vector.
//
// All measurements in CM OF DURASTEEL — same unit as armor and gun damage.
// effect_pct scales incoming damage (>100 = better than nominal).
// -----------------------------------------------------------------------------

#include <string>
#include <string_view>
#include <vector>

struct ShieldType {
    std::string name;
    uint8_t level         = 1;     // 1-3 — must be <= ShipClass::max_shield_level
    float power_gw        = 0.0f;  // passive draw (informational v1)
    float regen_cm_per_s  = 0.0f;
    float front_cm        = 0.0f;
    float back_cm         = 0.0f;
    float side_cm         = 0.0f;
    float effect_pct      = 100.0f;  // 87..111 in the source data
};

namespace shield {

// Parse the "shields" array from docs/privateer_ship_data.json. Logs
// `[shield] loaded N types`. Returns true on success.
bool load_table(const std::string& json_path);

// Look up by canonical name from the source data ("Shield Generator 2",
// "Heavy Shields", etc.). Returns nullptr on miss. Pointers are stable
// for the program's lifetime — the table isn't reallocated after init.
const ShieldType* find(std::string_view name);

// Read-only view for tooling (debug panel, save/load).
const std::vector<ShieldType>& all();

} // namespace shield

#pragma once
// -----------------------------------------------------------------------------
// armor.h — installable hull armor types.
//
// Mirror of shield.h: a small open-ended catalogue of fitted equipment,
// loaded from docs/privateer_ship_data.json at startup. Source data has
// three (Plasteel, Tungsten, Isometal). A Ship instance holds a
// const ArmorType* alongside its base hull cm; total cm = base + armor.
//
// Source data also lists per-facing values, but Privateer armor is
// uniform (front_cm == back_cm == side_cm in every entry). We keep the
// per-facing fields anyway in case future entries break that pattern,
// and to keep the data model identical to shields.
// -----------------------------------------------------------------------------

#include <string>
#include <string_view>
#include <vector>

struct ArmorType {
    std::string name;
    float front_cm = 0.0f;
    float back_cm  = 0.0f;
    float side_cm  = 0.0f;
};

namespace armor {

bool load_table(const std::string& json_path);
const ArmorType*               find(std::string_view name);
const std::vector<ArmorType>&  all();

} // namespace armor

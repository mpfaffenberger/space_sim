#include "gun.h"

#include "json.h"

#include <cctype>
#include <cstdio>
#include <string>
#include <unordered_map>

GunStats g_gun_stats[kGunTypeCount] = {};

namespace {

// Map the human-readable names in docs/privateer_ship_data.json to our
// enum values. The canonical name comes straight from the JSON so the
// docs file remains source-of-truth; the lowercase_underscore alias is
// what ship.json files use to reference the type.
struct NameMap {
    GunType     type;
    const char* json_name;        // exact match for the source data
    const char* short_name;       // lowercase_underscore for ship.json
    HMM_Vec3    tracer_color;     // canonical-feeling colour per gun
};

constexpr NameMap k_name_map[] = {
    { GunType::Laser,            "Laser",              "laser",              {1.00f, 0.20f, 0.20f} },
    { GunType::MassDriver,       "Mass Driver",        "mass_driver",        {1.00f, 0.55f, 0.10f} },
    { GunType::MesonBlaster,     "Meson Blaster",      "meson_blaster",      {0.30f, 1.00f, 0.40f} },
    { GunType::NeutronGun,       "Neutron Gun",        "neutron_gun",        {0.40f, 0.95f, 1.00f} },
    { GunType::ParticleCannon,   "Particle Cannon",    "particle_cannon",    {1.00f, 0.95f, 0.30f} },
    { GunType::TachyonCannon,    "Tachyon Cannon",     "tachyon_cannon",     {1.00f, 0.30f, 1.00f} },
    { GunType::IonicPulseCannon, "Ionic Pulse Cannon", "ionic_pulse_cannon", {0.30f, 0.40f, 1.00f} },
    { GunType::PlasmaGun,        "Plasma Gun",         "plasma_gun",         {1.00f, 0.85f, 0.55f} },
    { GunType::SteltekGun,       "Steltek Gun",        "steltek_gun",        {0.30f, 1.00f, 0.50f} },
};
static_assert(sizeof(k_name_map)/sizeof(k_name_map[0]) == kGunTypeCount,
              "k_name_map must list every GunType");

// JSON helper: read an optional numeric field. Returns 0 (and reports
// "missing" via *was_null) when the source JSON has `null` (which is
// how the docs flag partial-data rows like Plasma Gun RF / Steltek RF).
float read_num_or_null(const json::Value* v, bool& was_null) {
    if (!v || v->is_null()) { was_null = true; return 0.0f; }
    return v->as_float();
}

} // namespace

bool gun::load_table(const std::string& json_path) {
    json::Value root = json::parse_file(json_path);
    if (!root.is_object()) {
        std::fprintf(stderr, "[gun] could not parse '%s'\n", json_path.c_str());
        return false;
    }
    const json::Value* arr = root.find("guns");
    if (!arr || !arr->is_array()) {
        std::fprintf(stderr, "[gun] '%s': missing 'guns' array\n", json_path.c_str());
        return false;
    }

    // Index entries in the JSON by canonical name so we can look up each
    // GunType regardless of source-array ordering.
    std::unordered_map<std::string, const json::Value*> by_name;
    for (const auto& v : arr->as_array()) {
        if (!v.is_object()) continue;
        if (auto* n = v.find("Name"); n && n->is_string()) {
            by_name[n->as_string()] = &v;
        }
    }

    int n_loaded = 0, n_complete = 0;
    for (const auto& nm : k_name_map) {
        auto it = by_name.find(nm.json_name);
        if (it == by_name.end()) {
            std::fprintf(stderr, "[gun] '%s' missing from '%s'\n",
                         nm.json_name, json_path.c_str());
            continue;
        }
        const json::Value& v = *it->second;
        GunStats& g = g_gun_stats[(int)nm.type];
        g.name           = nm.json_name;
        g.tracer_color   = nm.tracer_color;
        g.damage_cm      = v.find("Damage (cm)")    ? v["Damage (cm)"].as_float() : 0.0f;
        g.range_m        = v.find("Range (m)")      ? v["Range (m)"].as_float()   : 0.0f;
        // Source data lists projectile speed in kps; we treat it as m/s so
        // the same numbers feel right at the engine's coordinate scale.
        // The "k" is flavour, not literal — matches the speed-table
        // discussion in the design pass.
        g.speed_mps      = v.find("Speed (kps)")    ? v["Speed (kps)"].as_float() : 0.0f;
        bool refire_null = false, energy_null = false;
        g.refire_delay_s = read_num_or_null(v.find("Refire Delay (s)"), refire_null);
        g.energy_cost_gj = read_num_or_null(v.find("Energy Use (GJ)"),  energy_null);
        g.complete       = !refire_null && !energy_null;
        ++n_loaded;
        if (g.complete) ++n_complete;
    }

    std::printf("[gun] loaded %d gun types (%d complete) from %s\n",
                n_loaded, n_complete, json_path.c_str());
    return true;
}

GunType gun::from_name(std::string_view s) {
    // Case- and underscore-tolerant — accept "Mass Driver", "mass_driver",
    // "MASS_DRIVER" etc. uniformly. Cheap because the table is tiny.
    std::string norm;
    norm.reserve(s.size());
    for (char c : s) {
        if (c == ' ') norm.push_back('_');
        else norm.push_back((char)std::tolower((unsigned char)c));
    }
    for (const auto& nm : k_name_map) {
        if (norm == nm.short_name) return nm.type;
    }
    return GunType::Count;
}

const char* gun::to_name(GunType t) {
    if ((int)t < 0 || (int)t >= kGunTypeCount) return "?";
    return k_name_map[(int)t].short_name;
}

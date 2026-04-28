#include "shield.h"

#include "json.h"

#include <cstdio>

namespace {

// Reserved up-front so pointers we hand out via shield::find() stay
// stable for the program's lifetime. 32 is plenty — source data has 13.
std::vector<ShieldType> g_shields;

float opt_num(const json::Value& v, const char* key, float fallback) {
    const json::Value* p = v.find(key);
    return (p && p->is_number()) ? p->as_float() : fallback;
}

} // namespace

bool shield::load_table(const std::string& json_path) {
    json::Value root = json::parse_file(json_path);
    if (!root.is_object()) {
        std::fprintf(stderr, "[shield] could not parse '%s'\n", json_path.c_str());
        return false;
    }
    const json::Value* arr = root.find("shields");
    if (!arr || !arr->is_array()) {
        std::fprintf(stderr, "[shield] '%s': missing 'shields' array\n", json_path.c_str());
        return false;
    }

    g_shields.clear();
    g_shields.reserve(32);
    for (const auto& v : arr->as_array()) {
        if (!v.is_object()) continue;
        ShieldType s;
        if (auto* n = v.find("Name"); n && n->is_string()) s.name = n->as_string();
        if (s.name.empty()) continue;   // skip malformed rows silently
        s.level          = (uint8_t)opt_num(v, "Levels", 1.0f);
        s.power_gw       = opt_num(v, "Power (GW)", 0.0f);
        s.regen_cm_per_s = opt_num(v, "Regen", 0.0f);
        s.front_cm       = opt_num(v, "Front (cm)", 0.0f);
        s.back_cm        = opt_num(v, "Back (cm)",  0.0f);
        s.side_cm        = opt_num(v, "Sides (cm)", 0.0f);
        s.effect_pct     = opt_num(v, "Effect %", 100.0f);
        g_shields.push_back(std::move(s));
    }

    std::printf("[shield] loaded %zu types from %s\n",
                g_shields.size(), json_path.c_str());
    return true;
}

const ShieldType* shield::find(std::string_view name) {
    for (const auto& s : g_shields) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

const std::vector<ShieldType>& shield::all() { return g_shields; }

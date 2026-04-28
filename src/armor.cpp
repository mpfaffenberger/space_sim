#include "armor.h"

#include "json.h"

#include <cstdio>

namespace {

std::vector<ArmorType> g_armors;

float opt_num(const json::Value& v, const char* key, float fallback) {
    const json::Value* p = v.find(key);
    return (p && p->is_number()) ? p->as_float() : fallback;
}

} // namespace

bool armor::load_table(const std::string& json_path) {
    json::Value root = json::parse_file(json_path);
    if (!root.is_object()) {
        std::fprintf(stderr, "[armor] could not parse '%s'\n", json_path.c_str());
        return false;
    }
    const json::Value* arr = root.find("armor");
    if (!arr || !arr->is_array()) {
        std::fprintf(stderr, "[armor] '%s': missing 'armor' array\n", json_path.c_str());
        return false;
    }

    g_armors.clear();
    g_armors.reserve(16);
    for (const auto& v : arr->as_array()) {
        if (!v.is_object()) continue;
        ArmorType a;
        if (auto* n = v.find("Name"); n && n->is_string()) a.name = n->as_string();
        if (a.name.empty()) continue;
        a.front_cm = opt_num(v, "Front (cm)", 0.0f);
        a.back_cm  = opt_num(v, "Back (cm)",  0.0f);
        a.side_cm  = opt_num(v, "Sides (cm)", 0.0f);
        g_armors.push_back(std::move(a));
    }

    std::printf("[armor] loaded %zu types from %s\n",
                g_armors.size(), json_path.c_str());
    return true;
}

const ArmorType* armor::find(std::string_view name) {
    for (const auto& a : g_armors) {
        if (a.name == name) return &a;
    }
    return nullptr;
}

const std::vector<ArmorType>& armor::all() { return g_armors; }

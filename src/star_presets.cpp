#include "star_presets.h"
#include "sun.h"

#include <algorithm>
#include <cctype>

void apply_star_preset(Sun& sun, const StarPreset& p) {
    sun.core_color         = p.core_color;
    sun.glow_color         = p.glow_color;
    sun.rim_tightness      = p.rim_tightness;
    sun.plasma_contrast    = p.plasma_contrast;
    sun.plasma_flow        = p.plasma_flow;
    sun.corona_radius_mult = p.corona_radius_mult;
    sun.gas_strength       = p.gas_strength;
    sun.ray_strength       = p.ray_strength;
}

const StarPreset* find_star_preset(const std::string& name) {
    // Normalize to lowercase once — tiny string, not worth a separate util.
    std::string key = name;
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    for (int i = 0; i < kStarPresetCount; ++i) {
        if (key == kStarPresets[i].name) return &kStarPresets[i];
    }
    return nullptr;
}

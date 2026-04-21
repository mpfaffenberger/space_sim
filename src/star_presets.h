#pragma once
// -----------------------------------------------------------------------------
// star_presets.h — named stellar classifications.
//
// Each preset bundles a color pair + the plasma + corona parameters that
// feel right for that "type" of star. Yellow is Sol-like; blue/red/orange
// are loosely tuned to real stellar class colors; green and purple are
// unapologetically sci-fi (pulsars, dyson-swarm stars, alien space opera
// stars, whatever you want them to be in lore).
// -----------------------------------------------------------------------------

#include "HandmadeMath.h"
#include <string>

struct Sun;

struct StarPreset {
    const char* name;
    HMM_Vec3 core_color;
    HMM_Vec3 glow_color;
    float    rim_tightness;      // sphere shader limb exponent
    float    plasma_contrast;    // 0..1; how violently the cells contrast
    float    plasma_flow;        // noise scroll speed (units / s)
    float    corona_radius_mult; // outer bloom billboard size factor
    float    gas_strength;       // noisy haze ring intensity
    float    ray_strength;       // lens-flare diffraction strength at source
};

// The catalog. Ordered so keys 1..6 map to a sensible tour.
constexpr StarPreset kStarPresets[] = {
    // Yellow (Sol-like G-type main sequence). Our baseline default.
    {
        "yellow",
        /*core*/  { 1.00f, 0.95f, 0.85f },
        /*glow*/  { 1.00f, 0.55f, 0.20f },
        /*rim*/    1.6f,
        /*contr*/  0.85f,
        /*flow*/   0.06f,
        /*coronaR*/18.0f,
        /*gas*/    0.75f,
        /*rays*/   0.12f,
    },
    // Blue (hot O/B-type supergiant). Bigger, angrier, whiter core.
    {
        "blue",
        /*core*/  { 0.82f, 0.92f, 1.00f },
        /*glow*/  { 0.35f, 0.60f, 1.00f },
        /*rim*/    1.4f,
        /*contr*/  1.10f,  // more violent granulation
        /*flow*/   0.10f,  // faster boil
        /*coronaR*/22.0f,  // bigger halo
        /*gas*/    0.85f,
        /*rays*/   0.18f,
    },
    // Red (dwarf or dying giant). Dim, slow, moody.
    {
        "red",
        /*core*/  { 1.00f, 0.42f, 0.20f },
        /*glow*/  { 0.90f, 0.18f, 0.06f },
        /*rim*/    1.8f,
        /*contr*/  0.60f,  // calm surface
        /*flow*/   0.035f, // slow boil
        /*coronaR*/14.0f,  // smaller halo
        /*gas*/    0.60f,
        /*rays*/   0.08f,
    },
    // Green (sci-fi — real blackbodies don't peak here, but we're a space
    // opera, not an astrophysics paper). Alien, eerie, slightly toxic.
    {
        "green",
        /*core*/  { 0.75f, 1.00f, 0.65f },
        /*glow*/  { 0.25f, 0.95f, 0.40f },
        /*rim*/    1.5f,
        /*contr*/  0.95f,
        /*flow*/   0.08f,
        /*coronaR*/20.0f,
        /*gas*/    0.90f,  // extra toxic-looking haze
        /*rays*/   0.14f,
    },
    // Orange (K-type dwarf). Warmer than yellow, dimmer than red.
    {
        "orange",
        /*core*/  { 1.00f, 0.68f, 0.35f },
        /*glow*/  { 0.95f, 0.35f, 0.10f },
        /*rim*/    1.6f,
        /*contr*/  0.80f,
        /*flow*/   0.05f,
        /*coronaR*/16.0f,
        /*gas*/    0.70f,
        /*rays*/   0.11f,
    },
    // Purple (exotic — pulsar, dying supergiant, whatever). Cool and tense.
    {
        "purple",
        /*core*/  { 0.90f, 0.75f, 1.00f },
        /*glow*/  { 0.65f, 0.20f, 1.00f },
        /*rim*/    1.5f,
        /*contr*/  1.00f,
        /*flow*/   0.07f,
        /*coronaR*/20.0f,
        /*gas*/    0.80f,
        /*rays*/   0.16f,
    },
};

constexpr int kStarPresetCount = (int)(sizeof(kStarPresets) / sizeof(kStarPresets[0]));

// Apply a preset to a Sun in place. Leaves geometry + resources untouched;
// only mutates the tunable fields.
void apply_star_preset(Sun& sun, const StarPreset& p);

// Look up by name (case-insensitive). Returns nullptr if not found.
const StarPreset* find_star_preset(const std::string& name);

#pragma once
// -----------------------------------------------------------------------------
// system_def.h — star-system-as-data.
//
// Until now the world was hardcoded in main.cpp: one sun at origin, one
// asteroid field at a baked-in position, skybox seed via CLI. That model
// breaks the second you want two systems to feel different — or the
// second you want content designers (future-you, at 11pm, in a trance)
// to be able to make systems without recompiling.
//
// So: a StarSystem is a value-typed struct populated from a JSON file.
// Loader returns nullopt on failure (and logs the reason). main.cpp reads
// whichever system the user named on the CLI (`--system troy`) and
// configures the scene from it.
//
// Schema (see assets/systems/*.json for examples):
//
//   {
//     "name":         "Troy",
//     "description":  "...",
//     "skybox_seed":  "troy",
//     "star":         { "preset": "yellow" },
//     "asteroid_fields": [
//        { "center": [x,y,z], "half_extent": [x,y,z],
//          "count": N, "base_radius": R, "seed": uint }
//     ],
//     "player_start": { "position": [x,y,z] }
//   }
//
// Any field can be omitted to fall back to defaults. Unknown fields are
// ignored (forward-compat).
// -----------------------------------------------------------------------------

#include "HandmadeMath.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct AsteroidFieldDef {
    HMM_Vec3  center       = { 0.0f, 0.0f, 0.0f };
    HMM_Vec3  half_extent  = { 3000.0f, 500.0f, 3000.0f };
    int       count        = 200;
    float     base_radius  = 45.0f;
    float     size_min     = 0.4f;
    float     size_max     = 2.6f;
    uint32_t  seed         = 0xA57E01DU;
};

// A static mesh dropped into the world at a specific position/orientation.
// "ships" right now, "stations/debris/planets/everything" down the line.
// A placed sprite billboard (np-0kv.4). The `sprite` field is a path
// stem relative to `assets/` (e.g. "sprites/mining_base") — the loader
// resolves it to <stem>.png for the hull and <stem>_lights.png for the
// optional static-emissive overlay, plus <stem>.lights.json for animated
// LightSpots (authored by the in-engine light editor, np-0kv.5). Scale
// is specified in length_meters like placed_meshes so authors can think
// in "how big is this station in world units" rather than pixel units.
struct PlacedSpriteDef {
    std::string sprite;                               // stem, e.g. "sprites/mining_base"
    HMM_Vec3    position      = { 0, 0, 0 };
    float       length_meters = 1000.0f;
};

// A multi-view ship sprite atlas instance. `atlas` is a JSON manifest stem
// relative to assets/, e.g. "ships/tarsus/atlas_manifest". Unlike generic
// placed_sprites, this selects one of many authored view angles per frame.
//
// Optional motion: NPC ships fly aircraft-style — nose tracks motion at
// `forward_speed` along body +Z, while the ship spins at `angular_velocity`
// around its own (body) axes. Free strafe / inertial drift are intentionally
// player-only (camera.cpp). For a horizontal turning circle, set
// `angular_velocity_deg = (0, yaw_rate, 0)` and `forward_speed = v`; the
// path radius is implicit (= v / |omega|). Default = static.
//
// Optional behavior block: when present, drives the ship via the
// flight controller (src/ship.h) instead of the static angular_velocity
// + forward_speed. The two paths are mutually exclusive in practice —
// behavior=None (the default) honours the legacy motion fields, any
// other behavior overwrites them every tick.
struct PlacedShipSpriteDef {
    std::string atlas;
    HMM_Vec3    position       = { 0, 0, 0 };
    float       length_meters  = 18.0f;
    bool        lights_enabled = true;  // debug scenes can disable glow spam

    HMM_Vec3    angular_velocity_deg = { 0.0f, 0.0f, 0.0f };  // body frame, deg/s
    float       forward_speed        = 0.0f;                  // body +Z, m/s

    // Optional explicit class name (e.g. "talon"). When empty, main.cpp
    // derives the class from the atlas path ("ships/talon/..." -> "talon").
    std::string ship_class;

    // Optional behavior. "" / "none" = no controller (legacy motion).
    // "pursue_target" = turn toward `behavior_target_pos` and throttle
    // to cruise. More behaviors land as the AI layer grows.
    std::string behavior_kind;
    HMM_Vec3    behavior_target_pos = { 0, 0, 0 };

    // Optional AI state-machine driver. When `ai_enabled` is true the
    // ship_ai layer owns `behavior` every frame (overrides whatever
    // `behavior_kind` set above — they're mutually exclusive in
    // practice). The initial state seeds the machine; transitions are
    // pure functions of perception + hp from there.
    bool        ai_enabled            = false;
    std::string ai_initial_state;     // "idle" / "patrol" / "engage" / "flee"
    HMM_Vec3    ai_patrol_anchor      = { 0, 0, 0 };
    bool        ai_has_patrol_anchor  = false;
};

struct PlacedMeshDef {
    std::string obj_path;    // relative to assets/ (e.g. "meshes/ships/tarsus.obj")
    HMM_Vec3    position  = { 0, 0, 0 };
    HMM_Vec3    euler_deg = { 0, 0, 0 };   // pitch, yaw, roll in degrees
    HMM_Vec3    tint      = { 1.0f, 1.0f, 1.0f };
    float       spec      = 0.35f;

    // Size control. Exactly one of these is meant to be set per ship.
    //
    //   length_meters > 0  → size the mesh so its longest AABB extent
    //                        equals this value (units = scene meters).
    //                        Self-documenting, independent of whatever
    //                        random units the source OBJ was authored in.
    //
    //   scale              → raw linear multiplier on the mesh's native
    //                        coordinates (legacy path for non-ship props
    //                        where canonical length isn't meaningful).
    //
    // If both are provided, `length_meters` wins. If neither, `scale = 1`.
    float       length_meters = 0.0f;
    float       scale         = 0.0f;

    // If set, replaces the on-disk <mesh>.png diffuse with a runtime-
    // generated procedural texture. Currently supported presets:
    //   "agricultural"  — green planet with oceans + polar caps
    // Leave empty to use the normal file-based texture loading.
    std::string texture_preset;

    // Winding policy. `true` → render both sides of every triangle
    // (safe default for imported meshes that may have inconsistent
    // winding). `false` → backface-cull; correct for our own clean
    // geometry like the icosphere planet, and a mild perf win on
    // closed convex shapes.
    bool        double_sided = true;

    // Lighting overrides — negative = "inherit global default". Lets
    // planets run a darker night side and skip the rim term.
    float       ambient_floor = -1.0f;
    float       rim_strength  = -1.0f;

    // Optional atmospheric halo. Zero thickness = no atmosphere pass
    // at all. Anything > 0 triggers a second draw with a fresnel shell
    // scaled up by this fraction of the mesh size.
    float       atm_thickness = 0.0f;
    HMM_Vec3    atm_color     = { 0.55f, 0.75f, 1.0f };
    float       atm_strength  = 1.0f;
};

// A navigation waypoint — a named point in space the player can target
// via the in-game targeting system (press N to cycle). Lives separately
// from `placed_sprites` / `placed_meshes` because some nav points are
// abstract (jump points have no current visual representation) and some
// renderable objects shouldn't be cycled through (e.g. background props).
// We keep the schemas decoupled so authors get clean control over both.
//
// `kind` is a free-form string used for HUD prefixing ("STATION", "JUMP",
// "PLANET") — kept as a string rather than an enum so adding a new kind
// ("WRECK", "ANOMALY") doesn't require code changes.
struct NavPointDef {
    std::string name;                       // human-readable, shown on HUD
    std::string kind     = "nav";           // "station" | "jump" | "planet" | ...
    HMM_Vec3    position = { 0.0f, 0.0f, 0.0f };
};

struct StarSystem {
    std::string name         = "Unnamed";
    std::string description;
    std::string skybox_seed  = "troy";
    std::string star_preset  = "yellow";

    // Studio-lighting flag for debug/inspection scenes. When true, main.cpp
    // dims the sun and parks it off-axis (same effect as --capture-clean's
    // sun setup) so bloom/lens-flare don't whitewash the scene. Unlike
    // --capture-clean, the in-game HUD/cockpit overlay still draws — this
    // is the live-debug case, not the screenshot case. Default false so
    // existing systems are unaffected.
    bool        studio_lighting = false;

    std::vector<AsteroidFieldDef> asteroid_fields;
    std::vector<PlacedMeshDef>       placed_meshes;
    std::vector<PlacedSpriteDef>     placed_sprites;
    std::vector<PlacedShipSpriteDef> placed_ship_sprites;
    std::vector<NavPointDef>         nav_points;

    HMM_Vec3    player_start = { 0.0f, 0.0f, 30000.0f };

    // Optional aim point at spawn. When `player_look_at_set` is true,
    // main.cpp builds a camera orientation that points the default
    // forward (-Z) at this world position. Useful for showing the
    // demo's interesting bits without a turn-around-and-find-it step.
    HMM_Vec3    player_look_at      = { 0.0f, 0.0f, 0.0f };
    bool        player_look_at_set  = false;
};

// Load a system from `assets/systems/<name>.json` — the common case. Pass
// a full path to load from anywhere. Returns nullopt on failure.
std::optional<StarSystem> load_system(const std::string& name_or_path);

// -----------------------------------------------------------------------------
// new_privateer — entry point.
//
// Stage 3 of N: flight + star + dust.
//
// Controls:
//   mouse          — look (pointer captured; right-click to release)
//   W / S          — throttle forward / reverse
//   A / D          — strafe left / right
//   Space / LShift — thrust up / down
//   Tab (hold)     — cruise engine: windup → high speed, windown on release
//   X              — full brake (zero velocity)
//   Escape (×2)    — quit (double-tap within 1s so accidental taps are safe)
// -----------------------------------------------------------------------------

#include "sokol_log.h"
#include "sokol_gfx.h"
#include "sokol_app.h"
#include "sokol_glue.h"
#include "sokol_time.h"
#include "sokol_debugtext.h"

#include "armor.h"
#include "asteroid.h"
#include "atlas_grid_viewer.h"
#include "camera.h"
#include "cockpit_hud.h"
#include "debug_panel.h"
#include "dev_remote.h"
#include "dust.h"
#include "faction.h"
#include "gun.h"
#include "mesh_render.h"
#include "ship.h"
#include "ship_class.h"
#include "shield.h"
#include "sprite.h"
#include "ship_sprite.h"
#include "sprite_light_editor.h"

#include <unordered_map>
#include "obj_loader.h"
#include "planet_texture.h"
#include "postprocess.h"
#include "render_config.h"
#include "rendertargets.h"
#include "skybox.h"
#include "star_presets.h"
#include "sun.h"
#include "system_def.h"


#include "imgui.h"   // ImGui::GetIO() for WantCaptureMouse handoff

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct AppState {
    sg_pass_action scene_pass_action{};
    Camera         camera{};
    Skybox         skybox{};
    Sun            sun{};
    DustField      dust{};
    RenderTargets  rt{};
    PostProcess    post{};

    // Zero-or-more asteroid fields, one per entry in the system's JSON.
    std::vector<AsteroidField> asteroid_fields;

    // Statically-placed meshes (ships, stations, debris). Loaded from the
    // system's `placed_meshes` list at startup.
    MeshRenderer            mesh_render{};
    std::vector<PlacedMesh> placed_meshes;

    // Sprite-with-lights renderer (np-0kv epic). Sprites are placed via
    // the system JSON's `placed_sprites` array (np-0kv.4). `sprite_art`
    // caches SpriteArt by stem path so the same PNG pair loads exactly
    // once even when multiple instances reference it.
    SpriteRenderer                                       sprite_render{};
    std::unordered_map<std::string, SpriteArt>           sprite_art;
    std::vector<SpriteObject>                            placed_sprites;
    std::unordered_map<std::string, ShipSpriteAtlas>      ship_sprite_atlases;
    std::vector<ShipSpriteObject>                        placed_ship_sprites;
    // Per-instance Ship objects, indexed in lockstep with placed_ship_sprites:
    // ships[i].sprite == &placed_ship_sprites[i] for every i. Holding the
    // back-pointer means we only have to keep the index correspondence at
    // construction time — once both vectors stop being resized, the pointer
    // stays valid and lookups are O(1). The cap-at-construction is OK
    // because no current code path appends ships after startup; if/when
    // dynamic spawning lands, this needs to be a slot-map (handle ->
    // index) rather than vectors of two.
    std::vector<Ship>                                    ships;
    std::vector<SpriteObject>                            frame_sprites;

    // Held-key flags, indexed by sapp key code. Flight physics reads these
    // every frame so we don't rely on key-repeat timing.
    std::array<bool, SAPP_MAX_KEYCODES> keys_down{};

    // Fly-by-wire aim mode. true = mouse cursor is hidden and its
    // screen position drives ship yaw/pitch (Freelancer feel). false =
    // cursor is visible and the ship freezes its turn input — used
    // when the player wants to click ImGui panels. SPACE toggles.
    bool     fly_by_wire = false;

    // Latest mouse position in *logical* (HiDPI-corrected) pixels,
    // populated on SAPP_EVENTTYPE_MOUSE_MOVE. We store it once and
    // reuse it from frame_cb (for ship aim) and cockpit_hud (for
    // drawing the on-screen aim cursor).
    float    mouse_x = 0.0f;
    float    mouse_y = 0.0f;

    uint64_t last_frame_ticks = 0;
    uint64_t last_fps_ticks   = 0;
    uint32_t frames_since     = 0;

    // Anti-Boodler escape-hatch: one tap arms it, a second tap within 1s
    // actually quits. Single strays just flash a reminder in the terminal.
    uint64_t escape_armed_ticks = 0;

    std::string system_name = "troy";   // assets/systems/<name>.json
    bool        capture_clean = false;  // hide HUD/cockpit overlay for atlas screenshots
    // Ship-sprite frame HUD: prints camera az/el and picked atlas cell az/el
    // for every placed_ship_sprites entry, every frame. F3 toggles. Hidden by
    // --capture-clean so screenshots stay HUD-free without extra flags.
    bool        show_ship_frame_hud = true;
    StarSystem  system{};

    // Targeting / nav-cycling. -1 = no target. Press N to advance through
    // g.system.nav_points. Persists for the lifetime of the loaded system
    // (we don't currently swap systems at runtime; if/when we do, reset
    // this when the new system loads).
    int selected_nav = -1;
};

AppState g;

// ---- input → camera mapping -------------------------------------------------
//
// Accumulates a local-space thrust vector from the currently-held keys. Keys
// are queried *once per axis* so pressing opposing keys cleanly cancels
// instead of flickering.
HMM_Vec3 thrust_from_keys() {
    HMM_Vec3 t{0, 0, 0};
    if (g.keys_down[SAPP_KEYCODE_W])            t.Z -= 1.0f;  // forward (-Z view)
    if (g.keys_down[SAPP_KEYCODE_S])            t.Z += 1.0f;
    if (g.keys_down[SAPP_KEYCODE_A])            t.X -= 1.0f;
    if (g.keys_down[SAPP_KEYCODE_D])            t.X += 1.0f;
    // SPACE used to be 'thrust up' but is now the fly-by-wire mode
    // toggle (handled in event_cb). R/F take over up/down strafe.
    if (g.keys_down[SAPP_KEYCODE_R])            t.Y += 1.0f;
    if (g.keys_down[SAPP_KEYCODE_F])            t.Y -= 1.0f;
    return t;
}

// ---- sokol callbacks --------------------------------------------------------

void init_cb() {
    sg_desc desc{};
    desc.environment = sglue_environment();
    desc.logger.func = slog_func;
    // Sokol pool sizes. Defaults (128 images / 128 views / 128 buffers) blew
    // up the moment we shipped a second view-sphere atlas: each ship cell
    // burns *both* an image and a view slot (80 + 80 per ship), so two ships
    // alone need 160+160 before we count standalone sprites, skybox, offscreen
    // render targets, ImGui's font atlas, etc. Pool overflow is silent —
    // sg_make_image returns SG_INVALID_ID which downstream code reads as
    // 'failed to load', not a hard crash, so the HUD just disappears.
    //
    // 8k slots is generous headroom for the current per-cell-image scheme
    // (~50 ships' worth) and the slot tracking structs themselves are small
    // (the real GPU cost is the textures behind them, not the slots).
    //
    // SCALING NOTE: this number can't grow forever — see
    // docs/ARCHITECTURE_TEXTURES.md. Once we approach ~50 ships' worth of
    // assets we should switch from one-image-per-cell to atlas-page-per-ship
    // (single 8192² sg_image holding all 80 cells as UV rects), which cuts
    // image-pool pressure 80x and is the precondition for LRU residency.
    desc.image_pool_size  = 8192;
    desc.view_pool_size   = 8192;
    desc.buffer_pool_size = 1024;
    sg_setup(&desc);
    stm_setup();

    // --- on-screen text HUD -----------------------------------------------
    // sokol_debugtext gives us several built-in bitmap fonts. We enable a
    // couple so we can style the HUD blocks differently later if we want.
    sdtx_desc_t sdt_desc{};
    sdt_desc.fonts[0]   = sdtx_font_kc854();    // chunky C64 vibe, readable
    sdt_desc.fonts[1]   = sdtx_font_oric();     // thinner for secondary lines
    // Pin the debugtext internal pipeline to the swapchain format — HUD is
    // drawn in the swapchain pass, not the offscreen scene pass.
    sdt_desc.context.color_format = kSwapchainColorFormat;
    sdt_desc.context.depth_format = SG_PIXELFORMAT_NONE;
    sdt_desc.context.sample_count = kSceneSampleCount;
    sdt_desc.logger.func = slog_func;
    sdtx_setup(&sdt_desc);

    // Clear color only shows if everything else fails to draw.
    g.scene_pass_action.colors[0].load_action = SG_LOADACTION_CLEAR;
    g.scene_pass_action.colors[0].clear_value = { 0.02f, 0.02f, 0.06f, 1.0f };
    g.scene_pass_action.depth.load_action     = SG_LOADACTION_CLEAR;
    g.scene_pass_action.depth.clear_value     = 1.0f;

    // Offscreen scene target + bloom ping-pong. Framebuffer size comes from
    // sokol_app, which knows the HiDPI-scaled physical resolution.
    if (!g.rt.init(sapp_width(), sapp_height())) {
        std::fprintf(stderr, "[main] render targets init failed\n");
        std::exit(1);
    }
    if (!g.post.init()) {
        std::fprintf(stderr, "[main] post pipeline init failed\n");
        std::exit(1);
    }

    // ---- load the star system definition ---------------------------------
    // Everything past this point is configured by the JSON: which skybox to
    // paint on the walls, which star preset to tune the sun, where the
    // player spawns, and any number of asteroid fields. Hot-reload-friendly
    // because main.cpp no longer bakes in ANY sector-specific values.
    if (auto sys = load_system(g.system_name)) {
        g.system = std::move(*sys);
    } else {
        std::fprintf(stderr, "[main] could not load system '%s' — quitting\n",
                     g.system_name.c_str());
        std::exit(1);
    }
    g.camera.position = g.system.player_start;

    // Optional spawn aim. The camera's default forward is -Z (identity
    // orientation). To point it at an arbitrary world position, build
    // the shortest-arc rotation from -Z to the desired direction. World
    // +Y is the implicit up reference; if the look_at is exactly above
    // or below the spawn the resulting orientation has an arbitrary
    // roll, which the player can correct with mouse input — fine for a
    // first-frame nudge, not worth a full "keep up vector vertical"
    // pipeline yet.
    if (g.system.player_look_at_set) {
        const HMM_Vec3 to_target = HMM_SubV3(g.system.player_look_at, g.camera.position);
        const float    dist2     = HMM_DotV3(to_target, to_target);
        if (dist2 > 1e-6f) {
            const HMM_Vec3 dir = HMM_DivV3F(to_target, std::sqrt(dist2));
            const HMM_Vec3 def_fwd = HMM_V3(0.0f, 0.0f, -1.0f);
            const HMM_Vec3 axis = HMM_Cross(def_fwd, dir);
            const float sin2 = HMM_DotV3(axis, axis);
            if (sin2 > 1e-10f) {
                const float sin_a = std::sqrt(sin2);
                const float cos_a = std::clamp(HMM_DotV3(def_fwd, dir), -1.0f, 1.0f);
                const float angle = std::atan2(sin_a, cos_a);
                const HMM_Vec3 unit = HMM_DivV3F(axis, sin_a);
                g.camera.orientation = HMM_QFromAxisAngle_RH(unit, angle);
            }
            // else: dir parallel to default forward — identity is already correct.
        }
    }

    // ---- load the design-data tables (factions, guns, shields, armor, ----
    // ship classes). Order matters: ship_class::load_all resolves
    // default_shield/default_armor strings against the shield/armor
    // tables, so those must be populated first. None of these touch the
    // GPU; they're pure data loaders, safe to run after system_def and
    // before any rendering init.
    faction::init();
    {
        const std::string ship_data = "docs/privateer_ship_data.json";
        gun::load_table(ship_data);
        shield::load_table(ship_data);
        armor::load_table(ship_data);
    }
    ship_class::load_all("assets/ships");

    const std::string dir = "assets/skybox/" + g.system.skybox_seed;
    if (!g.skybox.init(dir, g.system.skybox_seed)) {
        std::fprintf(stderr, "[main] skybox init failed (run tools/gen_skybox.sh %s)\n",
                     g.system.skybox_seed.c_str());
        std::exit(1);
    }

    if (!g.sun.init())  { std::fprintf(stderr, "[main] sun init failed\n");  std::exit(1); }
    if (const StarPreset* sp = find_star_preset(g.system.star_preset)) {
        apply_star_preset(g.sun, *sp);
    } else {
        std::fprintf(stderr, "[main] unknown star preset '%s' — using defaults\n",
                     g.system.star_preset.c_str());
    }

    // Park the sun at the *centroid* of all nav points. This makes the
    // star sit in the middle of its system geographically — jump points
    // and stations end up arrayed around it like a real planetary disc,
    // and the lens flare / lighting cues all radiate outward from the
    // hub the player is meant to think of as 'the centre of Troy'
    // rather than the arbitrary world origin (0,0,0).
    //
    // Skipped when nav_points is empty (e.g. Crimson Veil, Hadrian's
    // Gate at time of writing) — those systems were authored against
    // the sun-at-origin convention and their hand-tuned spawn framings
    // would break if we silently moved the star.
    // A tiny dim sun parked far off-axis so meshes get readable directional
    // light without the bloom/lens-flare whitewashing the scene. Two things
    // opt in: --capture-clean (screenshot/atlas mode) and the per-system
    // "studio_lighting": true flag (debug/inspection scenes that still want
    // a live HUD). Both call this same helper so the lighting setup never
    // drifts between the two paths.
    auto apply_studio_sun = [](Sun& sun) {
        sun.position     = HMM_V3(-200000.0f, 150000.0f, 200000.0f);
        sun.radius       = 100.0f;
        sun.core_color   = HMM_V3(0.70f, 0.70f, 0.70f);
        sun.glow_color   = HMM_V3(0.06f, 0.06f, 0.06f);
        sun.corona_alpha = 0.0f;
        sun.gas_strength = 0.0f;
        sun.ray_strength = 0.0f;
    };

    if (g.capture_clean) {
        apply_studio_sun(g.sun);
        std::printf("[main] capture-clean studio sun enabled\n");
    } else if (g.system.studio_lighting) {
        apply_studio_sun(g.sun);
        std::printf("[main] system studio_lighting=true: dim sun enabled\n");
    } else if (!g.system.nav_points.empty()) {
        HMM_Vec3 sum{0.0f, 0.0f, 0.0f};
        for (const auto& nav : g.system.nav_points) {
            sum = HMM_AddV3(sum, nav.position);
        }
        const float n = (float)g.system.nav_points.size();
        g.sun.position = HMM_V3(sum.X / n, sum.Y / n, sum.Z / n);
        std::printf("[main] sun parked at nav-centroid (%.0f, %.0f, %.0f) from %d nav points\n",
                    g.sun.position.X, g.sun.position.Y, g.sun.position.Z,
                    (int)g.system.nav_points.size());
    }

    // Spin up one AsteroidField per entry in the system JSON. Each uses its
    // own seed so placement/sizes are deterministic per-sector.
    g.asteroid_fields.reserve(g.system.asteroid_fields.size());
    for (const auto& def : g.system.asteroid_fields) {
        AsteroidField f;
        f.center      = def.center;
        f.half_extent = def.half_extent;
        f.total_count = def.count;
        f.base_radius = def.base_radius;
        f.size_min    = def.size_min;
        f.size_max    = def.size_max;
        f.seed        = def.seed;
        if (!f.init()) {
            std::fprintf(stderr, "[main] asteroid field init failed\n");
            std::exit(1);
        }
        g.asteroid_fields.push_back(std::move(f));
    }

    if (!g.dust.init()) { std::fprintf(stderr, "[main] dust init failed\n"); std::exit(1); }

    // Mesh renderer + placed mesh instances. Load OBJs from disk now; any
    // file that fails to parse is skipped with a warning so one bad entry
    // doesn't take the whole system down.
    if (!g.mesh_render.init()) {
        std::fprintf(stderr, "[main] mesh renderer init failed\n");
        std::exit(1);
    }

    // Dear ImGui debug overlay. Must come after sg_setup() so the sokol
    // backend has a valid device/context to build its pipeline against.
    debug_panel::init();
    sprite_light_editor::init();
    atlas_grid_viewer::init();

    // Dev remote: HTTP control channel on 127.0.0.1. Lets external
    // tools (code puppy, curl, shell scripts) teleport the camera,
    // grab screenshots, and read state. Non-fatal if it can't bind.
    // Port 47001 picked to avoid collisions with common local dev
    // servers (3000, 5000, 8080, 8765, …).
    dev_remote::start(47001);
    dev_remote::publish_system_name(g.system.name.c_str());
    for (const auto& pm_def : g.system.placed_meshes) {
        PlacedMesh pm;
        pm.name        = pm_def.obj_path;
        pm.position    = pm_def.position;
        pm.euler_deg   = pm_def.euler_deg;
        pm.body_tint     = pm_def.tint;
        pm.spec_amount   = pm_def.spec;
        pm.double_sided  = pm_def.double_sided;
        pm.ambient_floor = pm_def.ambient_floor;
        pm.rim_strength  = pm_def.rim_strength;
        pm.atm_thickness = pm_def.atm_thickness;
        pm.atm_color     = pm_def.atm_color;
        pm.atm_strength  = pm_def.atm_strength;
        const std::string full = "assets/" + pm_def.obj_path;
        const uint64_t     t0 = stm_now();
        if (!load_obj_file(full, pm.mesh) || !pm.mesh.upload()) {
            std::fprintf(stderr, "[main] skipping placed mesh '%s'\n", full.c_str());
            continue;
        }
        const double load_ms = stm_ms(stm_since(t0));

        // Resolve scale. `length_meters` is the preferred path: it uses
        // the mesh's measured bounding box so that 1 scene unit = 1 metre
        // regardless of whatever units the source tool exported. Raw
        // `scale` is still accepted for legacy / non-ship placements.
        const float ext = pm.mesh.longest_extent();
        if (pm_def.length_meters > 0.0f && ext > 1e-6f) {
            pm.scale = pm_def.length_meters / ext;
        } else if (pm_def.scale > 0.0f) {
            pm.scale = pm_def.scale;
        } else {
            pm.scale = 1.0f;
        }

        // Textures now live on `pm.mesh.materials`, populated by the OBJ
        // loader from the `<stem>.materials.json` sidecar. The only
        // exception is procedurally-generated planet textures, which are
        // injected into the mesh's first (usually only) material's
        // diffuse slot so the rest of the pipeline treats it like any
        // other material.
        if (!pm_def.texture_preset.empty() && !pm.mesh.materials.empty()) {
            PlanetTexture pt = make_planet_texture(pm_def.texture_preset);
            Material& m0 = pm.mesh.materials[0];
            if (m0.diffuse.valid) {   // replace any prior-loaded slot
                sg_destroy_view(m0.diffuse.view);
                sg_destroy_image(m0.diffuse.image);
            }
            m0.diffuse.image = pt.image;
            m0.diffuse.view  = pt.view;
            m0.diffuse.valid = true;
        }

        // Count material slots actually populated.
        int md = 0, ms = 0, mg = 0, mn = 0;
        for (const auto& mat : pm.mesh.materials) {
            md += mat.diffuse.valid ? 1 : 0;
            ms += mat.spec.valid    ? 1 : 0;
            mg += mat.glow.valid    ? 1 : 0;
            mn += mat.normal.valid  ? 1 : 0;
        }
        std::printf("[main]   mesh '%s' loaded in %.1f ms: %d tris  "
                    "submeshes=%zu  mats=%zu (D%d/S%d/G%d/N%d)\n",
                    full.c_str(), load_ms, pm.mesh.index_count / 3,
                    pm.mesh.submeshes.size(),
                    pm.mesh.materials.size(),
                    md, ms, mg, mn);
        g.placed_meshes.push_back(std::move(pm));
    }

    // --- sprite renderer (np-0kv) ----------------------------------------
    // One-time init, then iterate the system JSON's `placed_sprites`
    // array. Each entry resolves to an `assets/<stem>.png` hull file plus
    // optional `<stem>_lights.png` static overlay and `<stem>.lights.json`
    // animated-light sidecar (authored by the light editor, np-0kv.5).
    // SpriteArt is cached by stem so repeated references share one GPU
    // texture upload.
    if (!g.sprite_render.init()) {
        std::fprintf(stderr, "[main] sprite renderer init failed\n");
        std::exit(1);
    }
    for (const auto& sd : g.system.placed_sprites) {
        const std::string stem_full = "assets/" + sd.sprite;  // e.g. assets/sprites/mining_base

        // Cache lookup — load once per unique stem even if referenced
        // multiple times. emplace returns {iter, inserted}.
        auto [it, inserted] = g.sprite_art.try_emplace(sd.sprite, SpriteArt{});
        if (inserted) {
            if (!load_sprite_art(stem_full, it->second)) {
                std::fprintf(stderr, "[main] skipping placed sprite '%s'\n",
                             sd.sprite.c_str());
                g.sprite_art.erase(it);
                continue;
            }
        }

        SpriteObject s{};
        s.art        = &it->second;
        s.position   = sd.position;
        s.world_size = sd.length_meters;
        s.tint       = HMM_V4(1.0f, 1.0f, 1.0f, 1.0f);
        // Lights are loaded once into SpriteArt by load_sprite_art(); copy
        // them onto the instance so the F2 editor can mutate per-instance
        // lists without touching the shared art (and so future per-instance
        // overrides — e.g. a damaged ship missing a nav light — drop in
        // without restructuring storage).
        s.lights = it->second.light_spots;
        g.placed_sprites.push_back(s);
    }

    for (const auto& sd : g.system.placed_ship_sprites) {
        auto [it, inserted] = g.ship_sprite_atlases.try_emplace(sd.atlas, ShipSpriteAtlas{});
        if (inserted && !load_ship_sprite_atlas(sd.atlas, it->second, g.sprite_art)) {
            std::fprintf(stderr, "[main] skipping ship sprite atlas '%s'\n", sd.atlas.c_str());
            g.ship_sprite_atlases.erase(it);
            continue;
        }

        ShipSpriteObject s{};
        s.atlas          = &it->second;
        s.position       = sd.position;
        s.world_size     = sd.length_meters;
        s.lights_enabled = sd.lights_enabled;
        s.tint           = HMM_V4(1.0f, 1.0f, 1.0f, 1.0f);

        // Motion: convert deg/s to rad/s once at scene-load time so the
        // hot path doesn't redo the multiplication every frame. orientation
        // defaults to identity (nose along world +Z) — adding an authored
        // initial yaw is a one-quaternion-multiply away if a future scene
        // needs it.
        constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;
        s.angular_velocity = HMM_MulV3F(sd.angular_velocity_deg, kDegToRad);
        s.forward_speed    = sd.forward_speed;

        g.placed_ship_sprites.push_back(s);
    }

    // ---- build the Ship array (one per placed sprite) -------------------
    // Each ShipSpriteObject above is paired with a Ship that owns class /
    // faction / health / behaviour. Class lookup: explicit `ship_class`
    // field on the JSON entry first, otherwise derive from the atlas path
    // by extracting the second slash-segment ("ships/talon/atlas_manifest"
    // -> "talon") — every existing scene happens to follow that
    // convention, so we get class binding for free.
    g.ships.reserve(g.placed_ship_sprites.size());
    int n_with_behavior = 0;
    for (size_t i = 0; i < g.system.placed_ship_sprites.size(); ++i) {
        const auto& sd = g.system.placed_ship_sprites[i];
        if (i >= g.placed_ship_sprites.size()) break;   // atlas-load failure earlier

        std::string class_name = sd.ship_class;
        if (class_name.empty()) {
            // sd.atlas looks like "ships/<class>/atlas_manifest" (or with
            // a .json suffix); pull out the <class> segment.
            const std::string& a = sd.atlas;
            auto slash1 = a.find('/');
            if (slash1 != std::string::npos) {
                auto slash2 = a.find('/', slash1 + 1);
                if (slash2 != std::string::npos) {
                    class_name = a.substr(slash1 + 1, slash2 - slash1 - 1);
                }
            }
        }
        const ShipClass* klass = ship_class::find(class_name);
        if (!klass) {
            std::fprintf(stderr, "[main] no ship_class for atlas '%s' (derived '%s'); "
                                 "sprite will run on legacy motion only\n",
                         sd.atlas.c_str(), class_name.c_str());
            // Push a placeholder Ship anyway so the indices stay aligned.
            // Behavior is None, so it does nothing — the existing motion
            // path drives the sprite as today.
            Ship placeholder{};
            placeholder.sprite = &g.placed_ship_sprites[i];
            g.ships.push_back(placeholder);
            continue;
        }

        Ship inst = ship::spawn(*klass);
        inst.sprite = &g.placed_ship_sprites[i];

        // Translate the JSON behaviour string into the Ship enum.
        if (sd.behavior_kind == "pursue_target") {
            inst.behavior.kind       = ShipBehavior::PursueTarget;
            inst.behavior.target_pos = sd.behavior_target_pos;
            ++n_with_behavior;
        } else if (sd.behavior_kind.empty() || sd.behavior_kind == "none") {
            inst.behavior.kind = ShipBehavior::None;
        } else {
            std::fprintf(stderr, "[main] unknown ship behavior '%s' on '%s' "
                                 "— treating as none\n",
                         sd.behavior_kind.c_str(), class_name.c_str());
        }

        g.ships.push_back(inst);
    }
    std::printf("[ship] %zu instances spawned (%d with active behavior)\n",
                g.ships.size(), n_with_behavior);

    // Fly-by-wire defaults OFF. Player toggles it with SPACE. This is much
    // friendlier for tools/capture scripts and prevents the camera from
    // drifting because the OS cursor happened to be off-centre. Amazing how
    // not fighting the tooling makes the tooling less cursed.
    sapp_show_mouse(true);
    g.fly_by_wire = false;

    g.last_frame_ticks = stm_now();
    g.last_fps_ticks   = g.last_frame_ticks;
    std::printf("[new_privateer] backend=%d, '%s' loaded\n",
                (int)sg_query_backend(), g.system.name.c_str());
}

void frame_cb() {
    // --- timestep -----------------------------------------------------------
    const uint64_t now = stm_now();
    const float    dt  = (float)stm_sec(stm_diff(now, g.last_frame_ticks));
    g.last_frame_ticks = now;

    // --- fly-by-wire aim ----------------------------------------------------
    // Drive yaw/pitch from the absolute mouse position. We skip when the
    // player has popped into free-cursor mode (SPACE) and ALSO when
    // ImGui wants the mouse (e.g. they're hovering the debug panel) —
    // otherwise the ship would lurch every time the user reaches for a
    // slider in CTRL+M.
    if (g.fly_by_wire && !ImGui::GetIO().WantCaptureMouse) {
        const float dpi = sapp_dpi_scale();
        const float w   = (float)sapp_width()  / dpi;
        const float h   = (float)sapp_height() / dpi;
        const float cx  = w * 0.5f;
        const float cy  = h * 0.5f;
        const float off_x = (g.mouse_x - cx) / cx;
        const float off_y = (g.mouse_y - cy) / cy;
        g.camera.apply_mouse_aim(off_x, off_y, dt);
    }

    // Drain any queued dev_remote commands into game state BEFORE
    // physics / rendering — a /camera/set that arrived this frame
    // should be visible in the frame we're about to produce.
    dev_remote::drain_commands(g.camera);

    // --- physics ------------------------------------------------------------
    // Hold-Tab cruise: drive target to 1 while held, 0 otherwise. Camera
    // smooths the lerp so it feels like winding up and winding down.
    g.camera.cruise_target = g.keys_down[SAPP_KEYCODE_TAB] ? 1.0f : 0.0f;
    if (g.keys_down[SAPP_KEYCODE_X]) g.camera.brake();
    // Roll input — Q/E rotate around the view axis. Opposing keys cancel
    // (the same once-per-axis read pattern as thrust_from_keys()).
    float roll_input = 0.0f;
    if (g.keys_down[SAPP_KEYCODE_Q]) roll_input -= 1.0f;
    if (g.keys_down[SAPP_KEYCODE_E]) roll_input += 1.0f;
    if (roll_input != 0.0f) g.camera.apply_roll(roll_input, dt);
    g.camera.apply_thrust(thrust_from_keys(), dt);
    g.camera.integrate(dt);

    // Ship behaviour + flight controller. Runs BEFORE the integrator so
    // any ship with an active behaviour writes fresh angular_velocity /
    // forward_speed onto its sprite this frame. Ships with behavior=None
    // skip the controller entirely and the integrator below sees the
    // legacy JSON-set motion unchanged — the existing demo flies on this
    // back-compat path.
    for (Ship& s : g.ships) ship::tick(s, dt);

    // NPC ship motion. Free-strafe lives on the camera (player only); ships
    // get aircraft-style integration — orientation rotates by body-frame
    // angular velocity, position advances along body +Z at forward_speed.
    // No-op for the static-ship case so this is safe to call unconditionally.
    update_ship_sprite_motion(g.placed_ship_sprites, dt);

    // --- render -------------------------------------------------------------
    // --- build the on-screen HUD for this frame -----------------------------
    const float fb_w = (float)sapp_width();
    const float fb_h = (float)sapp_height();
    sdtx_canvas(fb_w * 0.5f, fb_h * 0.5f);

    const HMM_Vec3 p = g.camera.position;
    const float speed = HMM_LenV3(g.camera.velocity);
    const float dist  = HMM_LenV3(HMM_SubV3(g.sun.position, p));
    const char* mode  = (g.camera.cruise_level > 0.5f)  ? "CRUISE"
                      : (g.camera.cruise_level > 0.05f) ? "SPOOL "
                      :                                   "NORMAL";

    if (!g.capture_clean) {
        sdtx_font(0);
        sdtx_color3f(0.7f, 1.0f, 0.9f);
        sdtx_pos(1.0f, 1.0f);
        sdtx_printf("SPEED  %7.0f u/s\n", speed);
        sdtx_printf("MODE   %s\n",        mode);
        sdtx_printf("D(SUN) %7.0f u\n",   dist);
        sdtx_printf("POS    %5.0f %5.0f %5.0f\n", p.X, p.Y, p.Z);

        // Ship-sprite frame HUD. Prints, per placed ship sprite, the raw
        // camera-relative az/el AND the authored atlas frame the engine
        // actually picked. Toggled with F3. Hidden when there are no ship
        // sprites in the scene (no signal, just clutter).
        if (g.show_ship_frame_hud && !g.placed_ship_sprites.empty()) {
            sdtx_color3f(1.0f, 0.85f, 0.4f);
            sdtx_puts("\n");                       // 1 blank line gap
            sdtx_puts("SHIP SPRITE FRAMES (F3)\n");
            for (size_t i = 0; i < g.placed_ship_sprites.size(); ++i) {
                const ShipSpriteObject& s = g.placed_ship_sprites[i];
                const char* key = (s.atlas ? s.atlas->key.c_str() : "<no-atlas>");
                // Trim a long atlas key like "ships/talon/atlas_manifest"
                // down to the ship name segment so the HUD stays narrow.
                const char* slash1 = std::strchr(key, '/');
                const char* slash2 = slash1 ? std::strchr(slash1 + 1, '/') : nullptr;
                const char* short_key = slash1 ? slash1 + 1 : key;
                const size_t short_len = slash2 ? (size_t)(slash2 - short_key)
                                                : std::strlen(short_key);
                char short_buf[24];
                const size_t copy_len = short_len < sizeof(short_buf) - 1
                                        ? short_len : sizeof(short_buf) - 1;
                std::memcpy(short_buf, short_key, copy_len);
                short_buf[copy_len] = '\0';
                const char* tag = s.manual_frame_enabled ? " [MANUAL]" : "";
                sdtx_printf(" %zu %-10s cam(az %+4.0f el %+4.0f) -> cell(az %+4.0f el %+4.0f)%s\n",
                            i, short_buf,
                            s.debug_cam_az_deg, s.debug_cam_el_deg,
                            s.debug_last_az_deg, s.debug_last_el_deg,
                            tag);
            }
            sdtx_color3f(0.7f, 1.0f, 0.9f);   // restore default for any later block
        }

        // Bottom-left: controls reminder in the second, thinner font.
        sdtx_font(1);
        sdtx_color3f(0.5f, 0.6f, 0.7f);
        sdtx_pos(1.0f, fb_h * 0.5f / 8.0f - 4.0f);   // 4 lines up from bottom
        sdtx_puts("W/S throttle   A/D strafe   R/F up/down\n");
        sdtx_puts("mouse aim      SPACE toggle cursor   TAB cruise\n");
        sdtx_puts("X brake        N cycle nav target\n");
        sdtx_puts("CTRL+M debug   F2 lights   F3 ship-frame HUD   ESC x2 quit\n");
    }

    // --- draw ---------------------------------------------------------------
    const float aspect   = fb_w / fb_h;
    const float time_sec = (float)stm_sec(stm_now());

    // Pass 1: scene → offscreen. Same draw order as before, just a
    // different attachment. HUD is NOT drawn here — it goes over the
    // composited swapchain so post-process doesn't blur/bloom the text.
    {
        sg_pass pass{};
        pass.action = g.scene_pass_action;
        pass.attachments.colors[0]  = g.rt.scene_color_att;
        pass.attachments.depth_stencil = g.rt.scene_depth_att;
        sg_begin_pass(&pass);
        // Draw order rationale:
        //   1. skybox   — no depth write, paints the background
        //   2. dust     — additive particulate in "empty space"; drawn BEFORE
        //                 opaque geometry so rocks/sun paint over it cleanly.
        //                 (If drawn later, dust's depth test lets individual
        //                 near-camera specks sparkle on top of rock surfaces,
        //                 which reads as "holes" to the eye.)
        //   3. asteroids — opaque, writes depth; covers dust where rocks are.
        //   4. sun sphere— opaque, writes depth.
        //   5. sun gas + corona — additive halos, depth test but no write.
        g.skybox.draw(g.camera, aspect);
        g.dust.draw(g.camera, aspect);
        for (const auto& f : g.asteroid_fields) {
            f.draw(g.camera, aspect, time_sec, g.sun.position, g.sun.core_color);
        }
        g.mesh_render.draw(g.placed_meshes, g.camera, aspect,
                           g.sun.position, g.sun.core_color);
        // Sprites go AFTER opaque meshes and BEFORE the sun so the sun's
        // additive corona still paints on top of everything. Sprites use
        // alpha blending, which needs opaque depth already in the buffer
        // so translucent edges composite correctly.
        g.frame_sprites = g.placed_sprites;
        append_ship_sprites_for_camera(g.placed_ship_sprites, g.camera, g.frame_sprites);
        g.sprite_render.draw(g.frame_sprites, g.camera, aspect, time_sec);
        g.sun.draw(g.camera, aspect, time_sec);
        sg_end_pass();
    }

    // Pass 2+3: bright-pass + separable gaussian into bloom_b.
    g.post.apply_bloom(g.rt);

    // Build the ImGui frame OUTSIDE any pass. This is only widget state;
    // no draw calls are issued yet. Slider mutations feed back into the
    // live PlacedMesh list so changes take effect on the *next* frame.
    debug_panel::build(g.placed_meshes, g.placed_ship_sprites);

    // Surface every ship-sprite atlas cell as an extra editable target so
    // F2 can author lights on individual frames (engine glow, nav strobes,
    // etc.). Built fresh each frame because cell pointers are stable but
    // membership in the dropdown should reflect any future hot-reload.
    std::vector<sprite_light_editor::EditableArt> ship_cell_targets;
    ship_cell_targets.reserve(g.ship_sprite_atlases.size() * 16);
    for (auto& [atlas_key, atlas] : g.ship_sprite_atlases) {
        for (auto& frame : atlas.frames) {
            // Cells inside an atlas live in the shared `sprite_art` cache,
            // but we don't want them shown twice if also placed standalone.
            // Filter by name: a placed sprite's stem won't end with `_cell`.
            if (!frame.art) continue;
            sprite_light_editor::EditableArt e;
            e.name = frame.art->name;
            e.art  = const_cast<SpriteArt*>(frame.art);
            ship_cell_targets.push_back(std::move(e));
        }
    }
    sprite_light_editor::build(g.placed_sprites, ship_cell_targets);
    // F4 — atlas grid viewer. Mutates ShipSpriteFrame fields directly,
    // so changes flow into the next render frame with no apply step.
    atlas_grid_viewer::build(g.ship_sprite_atlases);
    if (!g.capture_clean) {
        cockpit_hud::build(g.camera, g.system, g.selected_nav,
                           g.mouse_x, g.mouse_y, g.fly_by_wire);
    }

    // Pass 4: composite to swapchain with lens flare + HUD + ImGui
    // overlay — all in ONE swapchain pass (Metal only tolerates one
    // drawable acquisition per frame; a second pass was flickering).
    const HMM_Mat4 vp = HMM_MulM4(g.camera.projection(aspect), g.camera.view());
    const HMM_Vec3 flare_tint = g.sun.glow_color;
    g.post.composite_to_swapchain(g.rt, g.sun.position, vp, flare_tint,
                                  sapp_width(), sapp_height(),
                                  [] { debug_panel::render(); });

    sg_commit();

    // End-of-frame hook for the dev remote. If a /screenshot is pending
    // this takes it now — after sg_commit the final composited frame is
    // on the macOS window, so the PNG captures what the user sees.
    dev_remote::maybe_capture_screenshot();

    // --- HUD in the terminal ------------------------------------------------
    g.frames_since++;
    const double elapsed = stm_sec(stm_diff(now, g.last_fps_ticks));
    if (elapsed >= 1.0) {
        const HMM_Vec3 p = g.camera.position;
        const float    speed = HMM_LenV3(g.camera.velocity);
        const float    dist_to_sun = HMM_LenV3(HMM_SubV3(g.sun.position, p));
        const char*    mode = (g.camera.cruise_level > 0.5f) ? "CRUISE"
                            : (g.camera.cruise_level > 0.05f) ? "spool"
                            : "normal";
        const int fps = (int)(g.frames_since / elapsed);
        if (!g.placed_ship_sprites.empty()) {
            const ShipSpriteObject& ship = g.placed_ship_sprites.front();
            std::printf("[new_privateer] %4d fps  %6s  v=%7.1f u/s  d(sun)=%.0f  pos=(%.0f,%.0f,%.0f)  ship_frame=(az %.0f el %.0f)\n",
                        fps, mode, speed, dist_to_sun,
                        p.X, p.Y, p.Z,
                        ship.debug_last_az_deg, ship.debug_last_el_deg);
        } else {
            std::printf("[new_privateer] %4d fps  %6s  v=%7.1f u/s  d(sun)=%.0f  pos=(%.0f,%.0f,%.0f)\n",
                        fps, mode, speed, dist_to_sun,
                        p.X, p.Y, p.Z);
        }
        dev_remote::publish_fps(fps);
        g.last_fps_ticks = now;
        g.frames_since   = 0;
    }
}

void cleanup_cb() {
    dev_remote::stop();
    sdtx_shutdown();
    g.post.destroy();
    g.rt.destroy();
    debug_panel::shutdown();
    atlas_grid_viewer::shutdown();
    for (auto& pm : g.placed_meshes) {
        // Mesh::destroy also frees every Material's textures — no more
        // per-placement teardown now that textures live on the mesh.
        pm.mesh.destroy();
    }
    g.mesh_render.destroy();
    for (auto& [_, art] : g.sprite_art) art.destroy();
    g.sprite_render.destroy();
    for (auto& f : g.asteroid_fields) f.destroy();
    g.dust.destroy();
    g.sun.destroy();
    g.skybox.destroy();
    sg_shutdown();
}

void event_cb(const sapp_event* ev) {
    // Give ImGui first crack at the event. If the panel is focused or the
    // mouse is over a widget it'll swallow the input; we only forward to
    // the camera / keymap when it doesn't.
    // Light editor sees events first so its F2 toggle beats any widget
    // focus or ImGui input capture in debug_panel. Same reason for
    // putting the F4 atlas grid viewer ahead of debug_panel.
    if (sprite_light_editor::handle_event(ev)) return;
    if (atlas_grid_viewer::handle_event(ev)) return;
    if (debug_panel::handle_event(ev)) return;

    switch (ev->type) {
    case SAPP_EVENTTYPE_KEY_DOWN:
        if (ev->key_code == SAPP_KEYCODE_ESCAPE) {
            const uint64_t now  = stm_now();
            const double   gap  = g.escape_armed_ticks
                                ? stm_sec(stm_diff(now, g.escape_armed_ticks))
                                : 999.0;
            if (gap < 1.0) {
                sapp_request_quit();
            } else {
                g.escape_armed_ticks = now;
                std::printf("[new_privateer] escape armed — tap again within 1s to quit\n");
            }
        }
        // N — cycle target through nav_points. KEY_DOWN (not keys_down
        // polled per frame) so a single press advances exactly one slot,
        // not however-many frames the key was physically held.
        if (ev->key_code == SAPP_KEYCODE_N && !g.system.nav_points.empty()) {
            const int n = (int)g.system.nav_points.size();
            g.selected_nav = (g.selected_nav + 1) % n;
            std::printf("[nav] target → %s\n",
                        g.system.nav_points[g.selected_nav].name.c_str());
        }
        // F3 — toggle the ship-sprite frame HUD. Useful while flying around a
        // sprite ship: lets you see exactly which atlas cell the engine picks
        // for your current camera angle, and how close the snapped cell is
        // to the raw camera direction.
        if (ev->key_code == SAPP_KEYCODE_F3) {
            g.show_ship_frame_hud = !g.show_ship_frame_hud;
            std::printf("[hud] ship-frame HUD %s\n",
                        g.show_ship_frame_hud ? "on" : "off");
        }
        // SPACE — toggle fly-by-wire vs free-cursor mode. Hides/shows
        // the OS cursor in lockstep so the visual matches the input
        // semantics without needing an extra polling check elsewhere.
        if (ev->key_code == SAPP_KEYCODE_SPACE) {
            g.fly_by_wire = !g.fly_by_wire;
            sapp_show_mouse(!g.fly_by_wire);
            std::printf("[input] fly-by-wire %s\n",
                        g.fly_by_wire ? "ENGAGED" : "PAUSED (cursor free)");
        }
        if ((size_t)ev->key_code < g.keys_down.size()) g.keys_down[ev->key_code] = true;
        break;
    case SAPP_EVENTTYPE_KEY_UP:
        if ((size_t)ev->key_code < g.keys_down.size()) g.keys_down[ev->key_code] = false;
        break;
    case SAPP_EVENTTYPE_MOUSE_MOVE: {
        // Convert framebuffer (HiDPI) px to logical px for everything
        // downstream — same convention as ImGui drawlist coords and
        // sapp_width()/dpi-corrected screen size used in cockpit_hud.
        const float dpi = sapp_dpi_scale();
        g.mouse_x = ev->mouse_x / dpi;
        g.mouse_y = ev->mouse_y / dpi;
        break;
    }
    default:
        break;
    }
}

} // namespace

sapp_desc sokol_main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--system") == 0 && i + 1 < argc) {
            g.system_name = argv[i + 1];
            ++i;
        } else if (std::strcmp(argv[i], "--capture-clean") == 0) {
            g.capture_clean = true;
        }
    }

    sapp_desc desc{};
    desc.init_cb      = init_cb;
    desc.frame_cb     = frame_cb;
    desc.cleanup_cb   = cleanup_cb;
    desc.event_cb     = event_cb;
    desc.width        = 1280;
    desc.height       = 800;
    static std::string title = "new_privateer — " + g.system_name;
    desc.window_title = title.c_str();
    desc.high_dpi     = true;
    desc.sample_count = kSceneSampleCount;   // MSAA off → matches offscreen
    desc.logger.func  = slog_func;
    return desc;
}

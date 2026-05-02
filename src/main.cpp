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
#include "explosion.h"
#include "firing.h"
#include "perception.h"
#include "world_scale.h"
#include "projectile.h"
#include "ship.h"
#include "ship_ai.h"
#include "ship_class.h"
#include "shield.h"
#include "sprite.h"
#include "ship_sprite.h"
#include "sprite_light_editor.h"
#include "sprite_generation_tool.h"

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
    // Per-instance Ship objects. ships[0] is reserved for the player
    // (is_player=true, no sprite, pose synced from camera each frame).
    // ships[1..] are NPCs in lockstep with placed_ship_sprites:
    // ships[1+i].sprite == &placed_ship_sprites[i] for every i. The
    // +1 offset keeps player-vs-NPC the only special case — every
    // perception / behaviour / damage code path otherwise treats
    // ships uniformly. The cap-at-construction is OK because no current
    // code path appends ships after startup; if/when dynamic spawning
    // lands, this needs to be a slot-map (handle -> index).
    std::vector<Ship>                                    ships;

    // Player's per-faction reputation. Default zero ("unknown stranger")
    // so faction baselines determine starting stance: Pirates jump you
    // (-30 baseline), Confeds tolerate you (0), Kilrathi attack on sight
    // (-100). Future commits will mutate this on kills / quests.
    PlayerReputation                                     player_rep;

    // Live projectiles. Spawned by firing::tick (when controller.fire_guns
    // is set on a ship with off-cooldown mounts), advanced by
    // projectile::tick, drawn by SpriteRenderer::draw_tracers. Empty in
    // the steady state until someone pulls a trigger.
    std::vector<Projectile>                              projectiles;

    // Active explosion FX. One Explosion per ship death, lifetime ~1.2s.
    // Drawn additively via the same spot pipeline as tracers, just with
    // a per-explosion size+color curve (see explosion.h for the
    // flash + shockwave layering rationale).
    std::vector<Explosion>                               explosions;

    // Shield-impact flashes. One per ship-facing that took shield damage
    // this frame; brief cyan bubble around the ship — reads as shield
    // lighting up under fire. Spawned in the damage-detection pass via
    // a before/after shield-value comparison.
    struct ShieldFlash {
        HMM_Vec3 position;
        float    radius;
        float    age_s;
        float    lifetime_s;
    };
    std::vector<ShieldFlash>                             shield_flashes;

    // Armor-impact flashes. Same structure as ShieldFlash but rendered
    // smaller (no shield bubble — hits land ON the hull) and orange-red
    // (sparking metal). Triggered when armor cm drops, NOT shields —
    // signals "shields down, hull taking damage" without needing a
    // separate UI cue.
    struct ArmorFlash {
        HMM_Vec3 position;
        float    radius;
        float    age_s;
        float    lifetime_s;
    };
    std::vector<ArmorFlash>                              armor_flashes;

    // Player damage indicator — screen-edge red vignette intensity. Set
    // to 1.0 whenever the player ship takes any damage (shield or armor),
    // decays exponentially each frame. Renders as a red gradient on the
    // screen border, fading toward center. The classic FPS "you're being
    // hit" cue without needing a dedicated full-screen shader.
    float                                                player_hit_intensity = 0.0f;
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
    bool        show_ship_frame_hud = false;
    StarSystem  system{};

    // Targeting / nav-cycling. -1 = no target. Press N to advance through
    // g.system.nav_points. Persists for the lifetime of the loaded system
    // (we don't currently swap systems at runtime; if/when we do, reset
    // this when the new system loads).
    int selected_nav = -1;

    // Player ship target. Cycled by the T key through every contact in
    // perception.visible[] sorted by distance ascending. 0 = no target.
    // Stored as a Ship::id rather than an index so it stays valid across
    // any future ship-array reshuffles. Resolved to a pointer each frame
    // when we need to display it.
    uint32_t player_target_id = 0;

    // Navmap overlay (Alt+N to toggle, Alt+N or ESC to close). Big
    // top-down view of the system's nav points + ship contacts; clicking
    // a nav selects it (matches the N-key cycle's effect).
    bool show_navmap = false;
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
    g.scene_pass_action.colors[0].clear_value = g.capture_clean
        ? sg_color{0.0f, 0.0f, 0.0f, 1.0f}
        : sg_color{0.02f, 0.02f, 0.06f, 1.0f};
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
        g.post.bloom_strength = 0.0f;
        g.post.flare_strength = 0.0f;
        std::printf("[main] capture-clean studio sun enabled; bloom/flare disabled\n");
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
    sprite_generation_tool::init();

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
        pm.clay_mode     = pm_def.clay_mode;
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
        // Ships scaled by k_ship_size_scale (world_scale.h). Both the
        // rendered billboard AND the hit-radius derived from world_size
        // (see ship::hit_radius_m) grow together, so visual size and
        // collision stay locked in sync — the player can target what
        // they see and trust the bullet will register.
        s.world_size     = sd.length_meters * world_scale::k_ship_size_scale;
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
    g.ships.reserve(g.placed_ship_sprites.size() + 1);

    // Player ship at index 0. No class, no sprite — pose is filled in
    // each frame from g.camera before perception runs. Adding the
    // player to g.ships unifies the perception + AI inner loops on a
    // single "all ships" iteration; without this every later layer
    // would need a special case for "target the player".
    g.ships.push_back(ship::spawn_player());
    g.ships.front().position    = g.camera.position;
    g.ships.front().orientation = g.camera.orientation;

    // Player ship — Tarsus class until the ship-picker flow lands.
    // Class assignment gives the player real armor/shield/energy from
    // the ShipClass data (matters once L4.2 damage lands; today it
    // just feeds firing.cpp's energy budget). Player still moves via
    // camera input, not the flight controller — class.cruise_speed /
    // accel / max_ypr are not consulted for player kinematics.
    {
        Ship& player = g.ships.front();
        if (const ShipClass* tarsus = ship_class::find("tarsus")) {
            player.klass = tarsus;
            // Mirror ship::spawn()'s health-from-class init since the
            // player path doesn't go through that function. (When the
            // ship-picker flow lands, this'll move into a shared
            // ship::init_from_class helper.)
            player.armor_fore_cm = tarsus->armor_fore_cm;
            player.armor_aft_cm  = tarsus->armor_aft_cm;
            player.armor_side_cm = tarsus->armor_side_cm;
            if (tarsus->default_armor) {
                player.armor_fore_cm += tarsus->default_armor->front_cm;
                player.armor_aft_cm  += tarsus->default_armor->back_cm;
                player.armor_side_cm += tarsus->default_armor->side_cm;
            }
            if (tarsus->default_shield) {
                player.shield_fore_cm = tarsus->default_shield->front_cm;
                player.shield_aft_cm  = tarsus->default_shield->back_cm;
                player.shield_side_cm = tarsus->default_shield->side_cm;
            }
            player.energy_gj = tarsus->energy_max;
        }
        // Custom loadout: 2x Meson Blaster, mounts on left and right and
        // dropped ~5° below the crosshair. Y offset (engine quirk: world
        // +Y projects to screen +Y = downward direction, so positive Y
        // appears BELOW center) is sized so the muzzle reads as a
        // chin/wing gun under the cockpit eye line at typical FOV.
        // Tracers still fire ALONG aim, not toward a convergence point;
        // the ITTS gimbal block above lets aim track the locked target
        // within a small cone, so the muzzles appear visibly below
        // while the bullets land where the reticle says.
        GunMount meson_l, meson_r;
        meson_l.type        = GunType::MesonBlaster;
        meson_r.type        = GunType::MesonBlaster;
        meson_l.offset_body = HMM_V3(-10.0f, 5.0f, 0.0f);
        meson_r.offset_body = HMM_V3( 10.0f, 5.0f, 0.0f);
        player.mounts        = { meson_l, meson_r };
        player.gun_cooldowns = { 0.0f, 0.0f };
    }

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

        // Faction override — same Talon hull can spawn as Pirate (the
        // class default), Militia, or Retro. Empty string keeps the
        // class default.
        if (!sd.faction_override.empty()) {
            const Faction f = faction::from_name(sd.faction_override);
            if (f != Faction::Count) {
                inst.faction = f;
            } else {
                std::fprintf(stderr, "[main] unknown faction '%s' on '%s' — using class default\n",
                             sd.faction_override.c_str(), class_name.c_str());
            }
        }

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

        // Translate the optional `ai` JSON block into the AI state.
        // ai_enabled wins over behavior — ship_ai::tick will overwrite
        // it every frame.
        if (sd.ai_enabled) {
            inst.ai.enabled = true;
            const AIState seeded = sd.ai_initial_state.empty()
                ? AIState::Idle
                : ship_ai::from_name(sd.ai_initial_state);
            inst.ai.state = (seeded == AIState::Count) ? AIState::Idle : seeded;
            inst.ai.has_patrol_anchor = sd.ai_has_patrol_anchor;
            inst.ai.patrol_anchor     = sd.ai_patrol_anchor;

            // Cowards (merchants etc.) auto-get their spawn position as
            // a patrol_anchor when one isn't explicitly set in JSON.
            // Used by the Flee state's home-tether: the ship still flees
            // from threats but is pulled back toward this point as it
            // gets further from home, so merchants don't fly off to
            // infinity. Standard-personality ships (pirates, militia)
            // don't get the auto-anchor — they're free hunters.
            if (klass->personality == AIPersonality::Coward
                && !inst.ai.has_patrol_anchor) {
                inst.ai.patrol_anchor     = sd.position;
                inst.ai.has_patrol_anchor = true;
            }
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

    // Pose sync: every ship's canonical position/orientation field
    // (used by perception + AI) gets refreshed from its source of
    // truth. Player (index 0) gets the camera; NPCs get their sprite
    // (the integrator's owner). After this, downstream code reads
    // `s.position` uniformly with no special cases.
    if (!g.ships.empty() && g.ships.front().is_player) {
        Ship& player = g.ships.front();
        player.position       = g.camera.position;
        player.orientation    = g.camera.orientation;
        // Camera carries the player's full 3D world velocity (forward
        // thrust + lateral strafes + persistent coasting under zero
        // damping). Projectiles inherit this in firing.cpp so tracers
        // move with the player's reference frame instead of drifting.
        player.world_velocity = g.camera.velocity;
    }
    for (size_t i = 1; i < g.ships.size(); ++i) {
        ship::sync_from_sprite(g.ships[i]);
    }

    // Perception: every ship learns who's in radar range this tick and
    // how the faction matrix (or player reputation, when the player is
    // involved) classifies them. Runs BEFORE behaviour so any AI that
    // wants to read perception ("chase nearest hostile") sees fresh
    // data. O(N²) over alive ships; cheap at the demo's scale, will
    // need spatial bucketing past ~100 ships.
    perception::tick(g.ships, g.player_rep);

    // AI state machine: between perception (input) and ship::tick
    // (output). For each ai-enabled ship it transitions the state and
    // writes a fresh behaviour for the controller to consume this same
    // tick. ai-disabled ships fall through unchanged — their behaviour
    // came from JSON or stays at None for legacy motion.
    {
        const float t_now = (float)stm_sec(stm_now());   // process uptime
        for (Ship& s : g.ships) ship_ai::tick(s, g.ships, t_now);
    }

    // Player firing input. Hold LEFT_CTRL to fire — clear, modifier-style
    // key that doesn't conflict with the existing W/A/S/D thrust + Q/E
    // roll + SPACE / TAB / X / N controls. The flag drains every frame
    // it's held; firing::tick consumes it and respects per-mount
    // cooldowns + energy budget.
    //
    // Aim direction defaults to camera-forward ("shoot where I'm
    // looking"). When the player has a locked target (T-cycle), an
    // ITTS aim-gimbal nudges the fire direction toward the lead point
    // within a small cone — same lead math as the on-screen ITTS
    // reticle, so the player sees "green crosshair → tracers go
    // there". Outside the cone, falls back to camera-forward and the
    // player has to rotate the ship to engage.
    if (!g.ships.empty() && g.ships.front().is_player) {
        Ship& player = g.ships.front();
        player.controller.fire_guns =
            g.keys_down[SAPP_KEYCODE_LEFT_CONTROL] ||
            g.keys_down[SAPP_KEYCODE_RIGHT_CONTROL];

        HMM_Vec3 aim = g.camera.forward();
        if (g.player_target_id != 0) {
            const Ship* target = nullptr;
            for (const Ship& s : g.ships) {
                if (s.id == g.player_target_id && s.alive) { target = &s; break; }
            }
            if (target) {
                // Average projectile speed across player mounts.
                float proj_speed = 1100.0f;
                {
                    int n = 0; float sum = 0.0f;
                    for (const auto& m : player.mounts) {
                        if ((int)m.type < 0 || (int)m.type >= kGunTypeCount) continue;
                        const GunStats& gs = g_gun_stats[(int)m.type];
                        if (!gs.complete) continue;
                        sum += gs.speed_mps; ++n;
                    }
                    if (n > 0) proj_speed = sum / n;
                }
                // Lead position: target_pos + target_vel * (dist / proj_speed).
                // world_velocity is already populated for both NPCs (via
                // sync_from_sprite) and player (camera.velocity), so this
                // works whether the target is moving on rails or being
                // controlled. Using sprite->position when available so
                // we lead where the visual ship is, not the frame-stale
                // Ship::position snapshot.
                const HMM_Vec3 t_pos = target->sprite ? target->sprite->position
                                                       : target->position;
                const HMM_Vec3 to_t  = HMM_SubV3(t_pos, player.position);
                const float    dist  = std::sqrt(HMM_DotV3(to_t, to_t));
                const float    t_int = (proj_speed > 1.0f) ? dist / proj_speed : 0.0f;
                const HMM_Vec3 lead  = HMM_AddV3(t_pos,
                    HMM_MulV3F(target->world_velocity, t_int));
                const HMM_Vec3 to_lead = HMM_SubV3(lead, player.position);
                const float    ll2     = HMM_DotV3(to_lead, to_lead);
                if (ll2 > 1e-6f) {
                    const HMM_Vec3 lead_dir = HMM_DivV3F(to_lead, std::sqrt(ll2));
                    // ±12° gimbal cone around camera-forward. Wider
                    // than the original 5° because long-range targets
                    // with lateral motion produce big lead offsets —
                    // a fighter at 5 km moving 240 m/s sideways leads
                    // ~270 m which can be 10°+ off the target's
                    // current position, outside a tight cone, so
                    // aim-assist never engages where it'd help most.
                    // Wider cone keeps the assist usable at long range
                    // without becoming "auto-aim everywhere" (40°+
                    // would feel like the gun has a mind of its own).
                    constexpr float gimbal_cos = 0.9781f;   // cos(12°)
                    if (HMM_DotV3(g.camera.forward(), lead_dir) >= gimbal_cos) {
                        aim = lead_dir;
                    }
                }
            }
        }
        player.controller.desired_forward = aim;
    }

    // Firing -> spawn projectiles. Runs after AI/player set fire_guns,
    // before projectile motion so a freshly-spawned projectile gets a
    // first-frame integration step (otherwise it'd appear stuck at the
    // muzzle for one frame).
    firing::tick(g.ships, g.projectiles, dt);
    projectile::tick(g.projectiles, dt);

    // Snapshot alive flags BEFORE damage so we can detect kills this
    // frame and spawn explosions at the right positions. Cheap (one
    // bool per ship); a static thread-local buffer reuses storage so
    // we don't allocate every frame in the steady state.
    static thread_local std::vector<bool> was_alive;
    was_alive.resize(g.ships.size());
    for (size_t i = 0; i < g.ships.size(); ++i) was_alive[i] = g.ships[i].alive;

    // Same idea for shield + armor values — record per-facing cm so we
    // can spawn flashes whenever any facing decreases. 6 floats per ship
    // total; ~400 bytes per frame at the demo's scale.
    struct HpSnap { float sh_fore, sh_aft, sh_side, ar_fore, ar_aft, ar_side; };
    static thread_local std::vector<HpSnap> hp_prev;
    hp_prev.resize(g.ships.size());
    for (size_t i = 0; i < g.ships.size(); ++i) {
        hp_prev[i] = { g.ships[i].shield_fore_cm,
                       g.ships[i].shield_aft_cm,
                       g.ships[i].shield_side_cm,
                       g.ships[i].armor_fore_cm,
                       g.ships[i].armor_aft_cm,
                       g.ships[i].armor_side_cm };
    }

    // Collision + damage. Runs AFTER projectile::tick — by this point
    // each alive projectile sits at its post-integration position, and
    // collide_and_damage reconstructs the previous position via velocity
    // for the swept-segment test against ship hit spheres. A successful
    // hit subtracts shield-then-armor from the right facing, sets
    // alive=false on a kill, and marks the projectile dead so it stops
    // rendering. Shield regen ticks afterwards on the same frame so
    // recently-hit facings honour their pause-after-hit timer before
    // refilling.
    projectile::collide_and_damage(g.projectiles, g.ships, dt);
    for (Ship& s : g.ships) ship::regen_shields(s, dt);

    // Death detection: any ship that flipped alive: true -> false this
    // frame just got killed; spawn an explosion at its last visible
    // position. Use sprite->position (the post-integration latest)
    // when available so the FX lines up with the rendered death pose.
    for (size_t i = 0; i < g.ships.size() && i < was_alive.size(); ++i) {
        if (was_alive[i] && !g.ships[i].alive) {
            const HMM_Vec3 pos = g.ships[i].sprite
                ? g.ships[i].sprite->position
                : g.ships[i].position;
            // Capture the camera basis at the moment of death so the
            // disc shockwave stays where it was if the camera rotates
            // afterwards. Player-facing disc at t=0 reads as a clean
            // expanding ring; later frames the ring may be at a slight
            // angle if the camera moved, which adds parallax.
            explosion::spawn(g.explosions, pos,
                             g.camera.right(), g.camera.up());
        }
    }
    explosion::tick(g.explosions, dt);

    // Shield + armor impact detection. Walk every ship and check if any
    // facing dropped this frame; spawn the appropriate flash. Skip dead
    // ships — the explosion FX already covers their final visual. The
    // PLAYER ship is special-cased: its bubble would render at the
    // camera position and just fill the view, so we only flag the
    // screen-edge vignette for the player rather than spawning a
    // bubble. NPC armor / shield bubbles render as world-space glows.
    for (size_t i = 0; i < g.ships.size() && i < hp_prev.size(); ++i) {
        const Ship& s = g.ships[i];
        if (!s.alive) continue;
        const HpSnap& prev = hp_prev[i];
        const bool sh_hit = (prev.sh_fore > s.shield_fore_cm)
                         || (prev.sh_aft  > s.shield_aft_cm)
                         || (prev.sh_side > s.shield_side_cm);
        const bool ar_hit = (prev.ar_fore > s.armor_fore_cm)
                         || (prev.ar_aft  > s.armor_aft_cm)
                         || (prev.ar_side > s.armor_side_cm);
        if (!(sh_hit || ar_hit)) continue;

        if (s.is_player) {
            // Player: screen-edge vignette instead of a bubble.
            g.player_hit_intensity = 1.0f;
            continue;
        }

        const HMM_Vec3 pos = s.sprite ? s.sprite->position : s.position;
        const float r = ship::hit_radius_m(s);

        if (sh_hit) {
            AppState::ShieldFlash f;
            f.position   = pos;
            f.radius     = r * 1.8f;
            f.age_s      = 0.0f;
            f.lifetime_s = 0.25f;
            g.shield_flashes.push_back(f);
        }
        if (ar_hit) {
            // Armor flash sits ON the hull (1.0x radius, not the
            // wider shield bubble), shorter lifetime, sparkier — these
            // are the "shields down, you're hitting metal now" cue.
            AppState::ArmorFlash f;
            f.position   = pos;
            f.radius     = r * 1.0f;
            f.age_s      = 0.0f;
            f.lifetime_s = 0.18f;
            g.armor_flashes.push_back(f);
        }
    }

    // Flash tick + prune. Inline (small structs, single use site each).
    for (auto& f : g.shield_flashes) {
        f.age_s += dt;
        if (f.age_s >= f.lifetime_s) f.lifetime_s = -1.0f;
    }
    g.shield_flashes.erase(
        std::remove_if(g.shield_flashes.begin(), g.shield_flashes.end(),
                       [](const AppState::ShieldFlash& f){ return f.lifetime_s < 0.0f; }),
        g.shield_flashes.end());
    for (auto& f : g.armor_flashes) {
        f.age_s += dt;
        if (f.age_s >= f.lifetime_s) f.lifetime_s = -1.0f;
    }
    g.armor_flashes.erase(
        std::remove_if(g.armor_flashes.begin(), g.armor_flashes.end(),
                       [](const AppState::ArmorFlash& f){ return f.lifetime_s < 0.0f; }),
        g.armor_flashes.end());

    // Player hit-indicator decay. ~0.5s fade-out (e^(-2*dt) per frame)
    // so the vignette pulses cleanly — bright on impact, gone in half
    // a second. Sustained fire keeps refreshing it via the spawn path
    // above, so it reads as steady-red "you're being hit".
    if (g.player_hit_intensity > 0.0f) {
        g.player_hit_intensity *= std::exp(-2.0f * dt);
        if (g.player_hit_intensity < 0.001f) g.player_hit_intensity = 0.0f;
    }

    // One-shot perception summary on first tick — confirms the wiring
    // without spamming stdout. Static gate flips after the first call.
    {
        static bool s_first_perception_dump = true;
        if (s_first_perception_dump) {
            s_first_perception_dump = false;
            for (const Ship& s : g.ships) {
                const char* name = s.klass     ? s.klass->name.c_str()
                                  : s.is_player ? "player"
                                  :               "?";
                std::printf("[perception] %-8s sees: %d hostile, %d allied, %d neutral",
                            name,
                            s.perception.n_hostile,
                            s.perception.n_allied,
                            s.perception.n_neutral);
                if (s.perception.nearest_hostile_id) {
                    std::printf("  (nearest hostile id=%u @ %.0fm)",
                                s.perception.nearest_hostile_id,
                                s.perception.nearest_hostile_dist);
                }
                std::printf("\n");
            }
        }
    }

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

    // ---- ship-vs-ship collisions -----------------------------------
    // O(N²) sphere-sphere check. When two ships overlap (distance <
    // r1 + r2), apply an elastic-ish bounce by adding impulse to each
    // ship's collision_velocity field, separate them by the overlap
    // amount so they don't keep colliding next frame, and apply damage
    // proportional to closing speed (more KE on impact = bigger
    // crunch). Player special-cased because the camera owns player
    // velocity, not the sprite — bounce hits g.camera.velocity
    // directly instead of the (nonexistent) player sprite.
    {
        // Damage per impact: linear in closing speed plus a base.
        // 50 cm minimum so a slow nudge isn't free; up to 250 cm for a
        // head-on at high relative velocity. Shields/armor are 30 cm
        // each so even a slow bump knocks half a shield down,
        // high-speed ramming chunks armor immediately. Mike's call:
        // "a lot of damage".
        constexpr float k_base_dmg          = 50.0f;
        constexpr float k_dmg_per_mps       = 0.5f;
        constexpr float k_dmg_max           = 250.0f;
        constexpr float k_elasticity        = 0.6f;   // 0=plastic, 1=fully elastic
        constexpr float k_player_hit_radius = 30.0f * 1.4f;  // matches ship::hit_radius_m

        for (size_t i = 0; i < g.ships.size(); ++i) {
            Ship& a = g.ships[i];
            if (!a.alive) continue;
            const HMM_Vec3 a_pos = a.sprite ? a.sprite->position : a.position;
            const float a_r = ship::hit_radius_m(a);
            if (a_r <= 0.0f) continue;
            const HMM_Vec3 a_vel = a.is_player ? g.camera.velocity : a.world_velocity;

            for (size_t j = i + 1; j < g.ships.size(); ++j) {
                Ship& b = g.ships[j];
                if (!b.alive) continue;
                const HMM_Vec3 b_pos = b.sprite ? b.sprite->position : b.position;
                const float b_r = ship::hit_radius_m(b);
                if (b_r <= 0.0f) continue;

                const HMM_Vec3 d = HMM_SubV3(b_pos, a_pos);
                const float d2 = HMM_DotV3(d, d);
                const float r_sum = a_r + b_r;
                if (d2 >= r_sum * r_sum) continue;

                // Normal: from a -> b. Degenerate-overlap fallback to
                // an arbitrary axis so we still separate ships that
                // happen to spawn at the same point.
                float dist = std::sqrt(std::max(d2, 1e-6f));
                const HMM_Vec3 n = (dist > 1e-3f)
                    ? HMM_DivV3F(d, dist) : HMM_V3(1.0f, 0.0f, 0.0f);

                // Closing speed along the normal. Positive = ships are
                // moving toward each other; negative = already
                // separating (skip impulse but still separate by
                // overlap so they don't stay stuck together).
                const HMM_Vec3 v_rel = HMM_SubV3(b.is_player ? g.camera.velocity
                                                              : b.world_velocity,
                                                  a_vel);
                const float v_rel_n = HMM_DotV3(v_rel, n);

                // Position separation — split overlap evenly. Player
                // moves the camera; NPCs nudge their sprite position.
                const float overlap = r_sum - dist;
                const HMM_Vec3 push = HMM_MulV3F(n, overlap * 0.5f);
                if (a.is_player) {
                    g.camera.position = HMM_SubV3(g.camera.position, push);
                } else if (a.sprite) {
                    a.sprite->position = HMM_SubV3(a.sprite->position, push);
                }
                if (b.is_player) {
                    g.camera.position = HMM_AddV3(g.camera.position, push);
                } else if (b.sprite) {
                    b.sprite->position = HMM_AddV3(b.sprite->position, push);
                }

                // Apply impulse only if approaching. For equal masses,
                // the impulse magnitude per ship is
                // (1+e) * v_rel_n / 2; opposite signs so a gets pushed
                // back along -n, b along +n.
                if (v_rel_n > 0.0f) {
                    const float impulse_mag = (1.0f + k_elasticity) * v_rel_n * 0.5f;
                    const HMM_Vec3 impulse_a = HMM_MulV3F(n, -impulse_mag);
                    const HMM_Vec3 impulse_b = HMM_MulV3F(n,  impulse_mag);
                    if (a.is_player) {
                        g.camera.velocity = HMM_AddV3(g.camera.velocity, impulse_a);
                    } else if (a.sprite) {
                        a.sprite->collision_velocity =
                            HMM_AddV3(a.sprite->collision_velocity, impulse_a);
                    }
                    if (b.is_player) {
                        g.camera.velocity = HMM_AddV3(g.camera.velocity, impulse_b);
                    } else if (b.sprite) {
                        b.sprite->collision_velocity =
                            HMM_AddV3(b.sprite->collision_velocity, impulse_b);
                    }
                }

                // Damage: scale with closing speed, with a base so
                // a slow nudge still does something. Apply to both
                // ships, on the facings that hit each other (b's hit
                // is from -n, a's hit is from +n). Player gets a 0.25×
                // multiplier — "reinforced cockpit" handwave so
                // collision feedback doesn't insta-kill the squishy
                // Tarsus the player flies. NPCs eat full damage.
                constexpr float k_player_dmg_multiplier = 0.25f;
                const float dmg = std::clamp(
                    k_base_dmg + k_dmg_per_mps * std::fabs(v_rel_n),
                    k_base_dmg, k_dmg_max);
                const float dmg_a = a.is_player ? dmg * k_player_dmg_multiplier : dmg;
                const float dmg_b = b.is_player ? dmg * k_player_dmg_multiplier : dmg;
                // Compute facings: hit point is approximately at the
                // midpoint between ship centers (where they touched).
                const HMM_Vec3 hit_point = HMM_AddV3(a_pos,
                    HMM_MulV3F(n, a_r));
                ship::take_damage(a, dmg_a, ship::facing_of_hit(a, hit_point));
                ship::take_damage(b, dmg_b, ship::facing_of_hit(b, hit_point));
            }
        }
        (void)k_player_hit_radius;
    }

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

            // Player HP block. Sums per-facing shield + armor; if
            // anything's missing (no class) we just don't print.
            if (!g.ships.empty() && g.ships.front().is_player
                && g.ships.front().klass) {
                const Ship& pl = g.ships.front();
                if (pl.alive) {
                    sdtx_printf("PLAYER  shield F%.0f A%.0f S%.0f  armor F%.0f A%.0f S%.0f\n",
                                pl.shield_fore_cm, pl.shield_aft_cm, pl.shield_side_cm,
                                pl.armor_fore_cm,  pl.armor_aft_cm,  pl.armor_side_cm);
                } else {
                    sdtx_puts("PLAYER  *** DEAD ***\n");
                }
            }

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
                // Perception column: H/A/N counts pulled from the matching
                // Ship. ships[0] is the player; NPC sprites[i] pair with
                // ships[i+1], hence the +1 offset. Empty string when no
                // Ship has perception (placeholder rows). HP column is
                // sum-of-shields and sum-of-armor across all 3 facings —
                // compact "how alive is this thing" readout.
                char percept_buf[32] = {0};
                char hp_buf[64] = {0};
                const size_t ship_idx = i + 1;
                if (ship_idx < g.ships.size() && g.ships[ship_idx].klass) {
                    const Ship& sh = g.ships[ship_idx];
                    const ShipPerception& p = sh.perception;
                    std::snprintf(percept_buf, sizeof(percept_buf),
                                  " [H%d A%d N%d]",
                                  p.n_hostile, p.n_allied, p.n_neutral);
                    if (sh.alive) {
                        const float sh_sum = sh.shield_fore_cm + sh.shield_aft_cm + sh.shield_side_cm;
                        const float ar_sum = sh.armor_fore_cm  + sh.armor_aft_cm  + sh.armor_side_cm;
                        std::snprintf(hp_buf, sizeof(hp_buf),
                                      " S%.0f A%.0f", sh_sum, ar_sum);
                    } else {
                        std::snprintf(hp_buf, sizeof(hp_buf), " DEAD");
                    }
                }
                sdtx_printf(" %zu %-10s cam(az %+4.0f el %+4.0f) -> cell(az %+4.0f el %+4.0f)%s%s%s\n",
                            i, short_buf,
                            s.debug_cam_az_deg, s.debug_cam_el_deg,
                            s.debug_last_az_deg, s.debug_last_el_deg,
                            tag, percept_buf, hp_buf);
            }
            sdtx_color3f(0.7f, 1.0f, 0.9f);   // restore default for any later block
        }

        // Bottom-left: controls reminder in the second, thinner font.
        sdtx_font(1);
        sdtx_color3f(0.5f, 0.6f, 0.7f);
        sdtx_pos(1.0f, fb_h * 0.5f / 8.0f - 4.0f);   // 4 lines up from bottom
        sdtx_puts("W/S throttle   A/D strafe   R/F up/down\n");
        sdtx_puts("mouse aim      SPACE toggle cursor   TAB cruise\n");
        sdtx_puts("X brake        N cycle nav target  T cycle ship target\n");
        sdtx_puts("CTRL+M debug   F2 lights   F3 ship-frame HUD   F6 sprite-gen   ESC x2 quit\n");
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
        if (!g.capture_clean) {
            g.skybox.draw(g.camera, aspect);
            g.dust.draw(g.camera, aspect);
            for (const auto& f : g.asteroid_fields) {
                f.draw(g.camera, aspect, time_sec, g.sun.position, g.sun.core_color);
            }
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

        // Additive glow billboards via the sprite spot pipeline.
        // Combines projectile tracers + explosion FX (flash + shockwave)
        // into one tracer list and submits in one draw call. HDR color
        // values >1.0 are intentional — bloom catches them and blooms
        // explosions look properly bright. Capture-clean mode skips these
        // dynamic effects so atlas reference renders contain only the
        // target object on the cleared background.
        if (!g.capture_clean) {
            std::vector<SpriteRenderer::Tracer> tracers;
            tracers.reserve(g.projectiles.size() + g.explosions.size() * 2);

            // Projectile tracers. Color from the gun type, size scales
            // with damage so Plasma reads chunkier than Laser.
            for (const Projectile& p : g.projectiles) {
                if (!p.alive) continue;
                const GunStats& gs = g_gun_stats[(int)p.type];
                SpriteRenderer::Tracer t;
                t.position = p.position;
                t.color    = gs.tracer_color;
                t.size = 5.0f + p.damage_cm;
                tracers.push_back(t);
            }

            // Explosions: per-explosion two-layer composite.
            //   * Flash: small, near-white, exponentially decaying. Most
            //     of its life is concentrated in the first ~150ms.
            //   * Shockwave: large, expanding, orange. Reads as fireball.
            // Color values >1.0 push past the LDR clamp so bloom
            // amplifies; the bigger the value, the brighter the halo.
            for (const Explosion& e : g.explosions) {
                if (!e.alive) continue;
                const float p = (e.lifetime_s > 0.0f)
                    ? std::clamp(e.age_s / e.lifetime_s, 0.0f, 1.0f) : 1.0f;

                // Flash — exponential decay, peaks at t=0. The (1-p)
                // factor is a small linear taper so it fully extinguishes
                // by end-of-life rather than just asymptoting near zero.
                const float flash_i = std::exp(-p * 8.0f) * (1.0f - p);
                if (flash_i > 0.005f) {
                    SpriteRenderer::Tracer t;
                    t.position = e.position;
                    t.color = HMM_V3(3.5f * flash_i, 3.0f * flash_i, 2.4f * flash_i);
                    t.size  = 80.0f + 60.0f * p;   // small, slowly grows
                    tracers.push_back(t);
                }

                // (Shield flash drawing happens after the explosion
                // loop; see the next block.)

                // Shockwave — radius grows aggressively early then
                // plateaus (the (1 - exp(-3p)) curve). Intensity fades
                // linearly so the halo dims as it expands. Orange-red
                // tint shifts slightly toward red over time so the late
                // frames read as smouldering rather than bright fire.
                const float wave_size  = 50.0f + 450.0f * (1.0f - std::exp(-p * 3.0f));
                const float wave_i     = (1.0f - p) * 1.8f;
                if (wave_i > 0.005f) {
                    SpriteRenderer::Tracer t;
                    t.position = e.position;
                    t.color = HMM_V3(2.5f * wave_i,
                                      1.0f * wave_i * (1.0f - 0.5f * p),
                                      0.3f * wave_i * (1.0f - p));
                    t.size  = wave_size;
                    tracers.push_back(t);
                }
            }

            // Shield flashes — short cyan glow at the ship's center,
            // sized to the hit sphere. Intensity decays linearly with
            // a slight bias toward the front of life so a fresh hit
            // pops bright and fades fast. Color tuned to match the UI
            // shield bar (cyan-blue) so the player intuitively links
            // "flash" to "shield".
            for (const AppState::ShieldFlash& f : g.shield_flashes) {
                const float p = (f.lifetime_s > 0.0f)
                    ? std::clamp(f.age_s / f.lifetime_s, 0.0f, 1.0f) : 1.0f;
                const float intensity = (1.0f - p) * (1.0f - p) * 2.5f;
                if (intensity <= 0.005f) continue;
                SpriteRenderer::Tracer t;
                t.position = f.position;
                t.color = HMM_V3(0.4f * intensity,
                                  1.6f * intensity,
                                  2.6f * intensity);
                t.size = f.radius;
                tracers.push_back(t);
            }

            // Armor flashes — orange-red sparks at the hull. Smaller +
            // shorter than the shield bubble so it reads as "hits ON
            // the metal" rather than "shield envelope flickering".
            // Color matches the orange UI armor bar (and the explosion
            // shockwave palette) for visual consistency.
            for (const AppState::ArmorFlash& f : g.armor_flashes) {
                const float p = (f.lifetime_s > 0.0f)
                    ? std::clamp(f.age_s / f.lifetime_s, 0.0f, 1.0f) : 1.0f;
                const float intensity = (1.0f - p) * (1.0f - p) * 3.0f;
                if (intensity <= 0.005f) continue;
                SpriteRenderer::Tracer t;
                t.position = f.position;
                t.color = HMM_V3(2.8f * intensity,
                                  1.0f * intensity,
                                  0.2f * intensity);
                t.size = f.radius;
                tracers.push_back(t);
            }

            g.sprite_render.draw_tracers(tracers, g.camera, aspect);
        }

        if (!g.capture_clean) {
            g.sun.draw(g.camera, aspect, time_sec);
        }
        sg_end_pass();
    }

    // Pass 2+3: bright-pass + separable gaussian into bloom_b.
    // Capture-clean atlas screenshots need deterministic object-only refs,
    // not stale bloom texture ghosts or lens flare artifacts.
    if (!g.capture_clean) {
        g.post.apply_bloom(g.rt);
    }

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
    // F6 — sprite-generation workbench. Front-end only; launches Python jobs.
    sprite_generation_tool::build();
    if (!g.capture_clean) {
        cockpit_hud::build(g.camera, g.system, g.selected_nav,
                           g.mouse_x, g.mouse_y, g.fly_by_wire,
                           g.ships, g.player_target_id);
        // Big system navmap (Alt+N to toggle). Drawn AFTER the regular
        // HUD so it overlays on top. Mutates selected_nav when the
        // player clicks a nav point — same effect as the N-cycle.
        cockpit_hud::build_navmap(g.camera, g.system, g.selected_nav,
                                   g.ships, g.show_navmap);
    }

    // ---- ship-target indicator ------------------------------------
    // If the player has a target ship locked (T cycles through nearby
    // contacts), draw a screen-space marker. On-screen targets get a
    // four-corner bracket; off-screen ones get an arrow on the screen
    // edge pointing toward where they are. Text below shows class
    // name + distance + faction stance. Drawn into the simgui
    // foreground draw list so it renders on top of everything.
    if (!g.capture_clean && g.player_target_id != 0 && !g.ships.empty()) {
        const Ship* target = nullptr;
        for (const Ship& s : g.ships) {
            if (s.id == g.player_target_id) { target = &s; break; }
        }
        if (target && target->alive) {
            // Engine quirks (mirror cockpit_hud.cpp's well-tested path):
            //   1. ImGui draw lists work in LOGICAL pixels; sapp_width/
            //      _height return PHYSICAL (HiDPI-scaled) pixels, so
            //      divide by sapp_dpi_scale() to get the logical canvas.
            //   2. Our perspective pipeline maps world-up to +screen_y
            //      already (no NDC Y-flip needed), so the textbook
            //      `(1 - ndc_y) * 0.5` inversion would put the bracket
            //      vertically MIRRORED from the ship.
            const float dpi  = sapp_dpi_scale();
            const float fb_w = (float)sapp_width()  / dpi;
            const float fb_h = (float)sapp_height() / dpi;
            const float aspect_loc = fb_w / fb_h;

            // Use the LIVE sprite position when available — the Ship's
            // `position` field is synced from the sprite at frame start,
            // before kinematic integration. By render time the sprite
            // has moved one tick further, and reading sprite->position
            // keeps the bracket locked on the visual ship instead of
            // lagging it by a frame.
            const HMM_Vec3 target_pos = target->sprite ? target->sprite->position
                                                       : target->position;
            const HMM_Mat4 vp_loc = HMM_MulM4(g.camera.projection(aspect_loc), g.camera.view());
            const HMM_Vec4 clip   = HMM_MulM4V4(vp_loc,
                HMM_V4(target_pos.X, target_pos.Y, target_pos.Z, 1.0f));
            const bool behind     = clip.W < 0.0f;

            // NDC. When the target is BEHIND the camera, dividing by a
            // negative W produces a sign-flipped projection — the
            // "true" screen direction is the NEGATIVE of what the
            // divide gives. Negating restores the geometric direction
            // so off-screen arrows point correctly. (My first cut here
            // wrote `-clip.X / -clip.W` which is algebraically the same
            // as `clip.X / clip.W` — a no-op bug.)
            float ndc_x = clip.X / clip.W;
            float ndc_y = clip.Y / clip.W;
            if (behind) { ndc_x = -ndc_x; ndc_y = -ndc_y; }
            const bool offscreen = behind
                                || std::fabs(ndc_x) > 1.0f
                                || std::fabs(ndc_y) > 1.0f;

            // Stance from the player's perception entry for this ship.
            // Drives indicator color: red=hostile, yellow=neutral,
            // green=allied. Default red if not in perception (e.g. the
            // moment after acquisition before the next perception tick).
            ImU32 col_hostile = IM_COL32(255,  80,  80, 255);
            ImU32 col_neutral = IM_COL32(255, 220,  60, 255);
            ImU32 col_allied  = IM_COL32( 80, 255,  80, 255);
            ImU32 color = col_hostile;
            float distance_m = HMM_LenV3(HMM_SubV3(target->position, g.ships.front().position));
            const ShipPerception& pp = g.ships.front().perception;
            for (const PerceivedContact& c : pp.visible) {
                if (c.ship_id == g.player_target_id) {
                    distance_m = c.distance_m;
                    color = (c.stance == Stance::Hostile) ? col_hostile
                          : (c.stance == Stance::Allied)  ? col_allied
                          :                                  col_neutral;
                    break;
                }
            }

            ImDrawList* dl = ImGui::GetForegroundDrawList();
            if (!offscreen) {
                // NDC -> screen. Engine convention: NDC y maps DIRECTLY
                // to screen y (no `(1 - ndc_y)` flip — see cockpit_hud's
                // header notes for the documented quirk).
                const float sx = (ndc_x * 0.5f + 0.5f) * fb_w;
                const float sy = (ndc_y * 0.5f + 0.5f) * fb_h;
                const float r  = 30.0f;
                const float k  = 10.0f;
                const float th = 2.0f;
                // 8 line segments, two per corner (L shape).
                dl->AddLine(ImVec2(sx-r, sy-r), ImVec2(sx-r+k, sy-r), color, th);
                dl->AddLine(ImVec2(sx-r, sy-r), ImVec2(sx-r, sy-r+k), color, th);
                dl->AddLine(ImVec2(sx+r, sy-r), ImVec2(sx+r-k, sy-r), color, th);
                dl->AddLine(ImVec2(sx+r, sy-r), ImVec2(sx+r, sy-r+k), color, th);
                dl->AddLine(ImVec2(sx-r, sy+r), ImVec2(sx-r+k, sy+r), color, th);
                dl->AddLine(ImVec2(sx-r, sy+r), ImVec2(sx-r, sy+r-k), color, th);
                dl->AddLine(ImVec2(sx+r, sy+r), ImVec2(sx+r-k, sy+r), color, th);
                dl->AddLine(ImVec2(sx+r, sy+r), ImVec2(sx+r, sy+r-k), color, th);

                // Label below the bracket.
                char buf[96];
                const char* tname = target->klass ? target->klass->name.c_str()
                                  : target->is_player ? "player" : "?";
                std::snprintf(buf, sizeof(buf), "%s   %.1f km", tname, distance_m * 0.001f);
                dl->AddText(ImVec2(sx - r, sy + r + 6.0f), color, buf);
            } else {
                // Off-screen: arrow on screen-edge box pointing in the
                // (ndc_x, ndc_y) direction from screen center. Margin
                // pulls the arrow inward so it doesn't get clipped.
                const float cx = fb_w * 0.5f;
                const float cy = fb_h * 0.5f;
                const float margin = 80.0f;
                const float half_w = fb_w * 0.5f - margin;
                const float half_h = fb_h * 0.5f - margin;
                const float dxlen = std::sqrt(ndc_x * ndc_x + ndc_y * ndc_y);
                if (dxlen > 1e-6f) {
                    const float dx = ndc_x / dxlen;
                    const float dy = ndc_y / dxlen;
                    // Scale to land on the rectangular edge. NO Y-flip
                    // here either — same engine convention as the
                    // bracket path: NDC y already maps to screen y.
                    const float scale = std::min(
                        half_w / std::max(std::fabs(dx), 1e-6f),
                        half_h / std::max(std::fabs(dy), 1e-6f));
                    const float ax = cx + dx * scale;
                    const float ay = cy + dy * scale;
                    const float arrow = 14.0f;
                    const float perp_x = -dy;
                    const float perp_y =  dx;
                    ImVec2 tip(   ax + dx * arrow,            ay + dy * arrow);
                    ImVec2 base_l(ax + perp_x * arrow * 0.6f, ay + perp_y * arrow * 0.6f);
                    ImVec2 base_r(ax - perp_x * arrow * 0.6f, ay - perp_y * arrow * 0.6f);
                    dl->AddTriangleFilled(tip, base_l, base_r, color);

                    // Label tucked just inside the arrow toward center.
                    char buf[96];
                    const char* tname = target->klass ? target->klass->name.c_str()
                                      : target->is_player ? "player" : "?";
                    std::snprintf(buf, sizeof(buf), "%s  %.1f km", tname, distance_m * 0.001f);
                    const float lx = ax - dx * 60.0f - 30.0f;
                    const float ly = ay - dy * 60.0f - 7.0f;
                    dl->AddText(ImVec2(lx, ly), color, buf);
                }
            }

            // ---- ITTS (Improved Targeting and Tracking System) -------
            // Wing-Commander-style lead reticle: where to aim to hit the
            // moving target given the player's projectile flight time.
            // Compute the target's world-frame velocity (forward * speed
            // for NPCs that have a sprite), the average projectile
            // speed across the player's mounts, and predict an
            // intercept point one iteration deep:
            //
            //   t_int = |target - me| / proj_speed
            //   lead  = target_pos + target_vel * t_int
            //
            // Single-pass is within a few metres at our engagement
            // ranges; a two-pass refinement (re-evaluate distance from
            // lead) tightens it further but isn't worth the math.
            //
            // Drawn ONLY when on-screen (in the camera's view frustum)
            // — an off-screen ITTS would be confusing because it isn't
            // the target itself, just where to aim. Off-screen targets
            // already have the directional arrow above.
            if (!offscreen) {
                const Ship& player = g.ships.front();
                HMM_Vec3 t_pos = target->sprite ? target->sprite->position
                                                 : target->position;
                HMM_Vec3 t_vel = HMM_V3(0, 0, 0);
                if (target->sprite) {
                    const HMM_Mat4 tR  = HMM_QToM4(target->orientation);
                    const HMM_Vec4 tf  = HMM_MulM4V4(tR, HMM_V4(0, 0, 1, 0));
                    t_vel = HMM_MulV3F(HMM_V3(tf.X, tf.Y, tf.Z),
                                        target->sprite->forward_speed);
                }

                // Average player projectile speed (skip null-stat guns).
                float proj_speed = 1100.0f;
                {
                    int n_complete = 0; float sum = 0.0f;
                    for (const auto& m : player.mounts) {
                        if ((int)m.type < 0 || (int)m.type >= kGunTypeCount) continue;
                        const GunStats& gs = g_gun_stats[(int)m.type];
                        if (!gs.complete) continue;
                        sum += gs.speed_mps; ++n_complete;
                    }
                    if (n_complete > 0) proj_speed = sum / n_complete;
                }

                const HMM_Vec3 to_t = HMM_SubV3(t_pos, player.position);
                const float    dist = std::sqrt(HMM_DotV3(to_t, to_t));
                const float    t_int = (proj_speed > 1.0f) ? dist / proj_speed : 0.0f;
                const HMM_Vec3 lead = HMM_AddV3(t_pos, HMM_MulV3F(t_vel, t_int));

                // Project lead to screen with the same vp_loc + Y-quirk
                // we used for the target bracket.
                const HMM_Vec4 lc = HMM_MulM4V4(vp_loc,
                    HMM_V4(lead.X, lead.Y, lead.Z, 1.0f));
                if (lc.W > 0.0f) {
                    // Project regardless of frustum bounds — at long
                    // range with fast lateral targets the lead point
                    // can land outside the view frustum even though
                    // the target itself is on-screen, and the previous
                    // |ndc| <= 1 gate hid the reticle exactly when the
                    // player needed it most. ImGui clips to its window
                    // anyway, so an off-screen reticle just won't be
                    // visible (no further hiding needed).
                    const float lndc_x = lc.X / lc.W;
                    const float lndc_y = lc.Y / lc.W;
                    const float lsx = (lndc_x * 0.5f + 0.5f) * fb_w;
                    const float lsy = (lndc_y * 0.5f + 0.5f) * fb_h;
                    // Reticle: open circle + small inset crosshair,
                    // bright green so it pops against any backdrop.
                    const ImU32 itts_col = IM_COL32(140, 255, 140, 230);
                    dl->AddCircle(ImVec2(lsx, lsy), 9.0f, itts_col, 16, 1.5f);
                    dl->AddLine(ImVec2(lsx - 5, lsy),
                                ImVec2(lsx + 5, lsy), itts_col, 1.0f);
                    dl->AddLine(ImVec2(lsx, lsy - 5),
                                ImVec2(lsx, lsy + 5), itts_col, 1.0f);
                }
            }
        } else {
            // Target died or fell off the world — clear so next T press
            // starts fresh from the nearest current contact.
            g.player_target_id = 0;
        }
    }

    // ---- player damage vignette ------------------------------------
    // Red screen-edge gradient when the player took damage recently.
    // Built from four edge rectangles, each with a multi-color
    // gradient: red+alpha at the screen edge, transparent toward the
    // center. Reads as a ring of damage glow without needing a
    // dedicated post-process shader. Intensity decays exponentially
    // every frame; sustained fire keeps refreshing it so the player
    // sees a steady red ring while being hit.
    if (!g.capture_clean && g.player_hit_intensity > 0.005f) {
        const float dpi = sapp_dpi_scale();
        const float w   = (float)sapp_width()  / dpi;
        const float h   = (float)sapp_height() / dpi;
        const float band = std::min(w, h) * 0.18f;   // band thickness ~18% of min dim
        const int   alpha = (int)std::clamp(g.player_hit_intensity * 200.0f, 0.0f, 200.0f);
        const ImU32 hit  = IM_COL32(255, 30, 30, alpha);
        const ImU32 zero = IM_COL32(255, 30, 30, 0);

        ImDrawList* dl = ImGui::GetForegroundDrawList();
        // Top band — red along screen-top edge, fades downward.
        dl->AddRectFilledMultiColor(ImVec2(0,    0),    ImVec2(w, band),
                                    hit, hit, zero, zero);
        // Bottom band — red along screen-bottom edge, fades upward.
        dl->AddRectFilledMultiColor(ImVec2(0,    h-band), ImVec2(w, h),
                                    zero, zero, hit, hit);
        // Left band — red along screen-left edge, fades rightward.
        dl->AddRectFilledMultiColor(ImVec2(0,    0),    ImVec2(band, h),
                                    hit, zero, zero, hit);
        // Right band — red along screen-right edge, fades leftward.
        dl->AddRectFilledMultiColor(ImVec2(w-band, 0),  ImVec2(w, h),
                                    zero, hit, hit, zero);
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
    sprite_generation_tool::shutdown();
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
    if (sprite_generation_tool::handle_event(ev)) return;
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
        // Alt+N opens the big navigation map overlay instead.
        if (ev->key_code == SAPP_KEYCODE_N) {
            const bool alt_held = (ev->modifiers & SAPP_MODIFIER_ALT) != 0;
            if (alt_held) {
                g.show_navmap = !g.show_navmap;
                std::printf("[navmap] %s\n", g.show_navmap ? "OPEN" : "closed");
            } else if (!g.system.nav_points.empty()) {
                const int n = (int)g.system.nav_points.size();
                g.selected_nav = (g.selected_nav + 1) % n;
                std::printf("[nav] target → %s\n",
                            g.system.nav_points[g.selected_nav].name.c_str());
            }
        }
        // T — cycle target through nearby ships (player's perception).
        // Sorted by distance ascending so repeated presses sweep nearest
        // -> farthest -> wrap. Picking up the cycle from "no target"
        // selects the nearest contact; if the current target has fallen
        // out of perception range since last frame, the search-by-id
        // fails and we restart at index 0.
        if (ev->key_code == SAPP_KEYCODE_T && !g.ships.empty()) {
            const Ship& player = g.ships.front();
            std::vector<PerceivedContact> sorted = player.perception.visible;
            std::sort(sorted.begin(), sorted.end(),
                      [](const PerceivedContact& a, const PerceivedContact& b) {
                          return a.distance_m < b.distance_m;
                      });
            if (sorted.empty()) {
                g.player_target_id = 0;
                std::printf("[target] no contacts in range\n");
            } else {
                int cur = -1;
                for (size_t i = 0; i < sorted.size(); ++i) {
                    if (sorted[i].ship_id == g.player_target_id) {
                        cur = (int)i; break;
                    }
                }
                const int next = (cur + 1) % (int)sorted.size();
                g.player_target_id = sorted[next].ship_id;
                // Look up the ship to print a friendly name.
                const char* name = "?";
                for (const Ship& s : g.ships) {
                    if (s.id == g.player_target_id) {
                        name = s.klass ? s.klass->name.c_str()
                             : s.is_player ? "player" : "?";
                        break;
                    }
                }
                std::printf("[target] → %s (id=%u, %.0f m)\n",
                            name, g.player_target_id, sorted[next].distance_m);
            }
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

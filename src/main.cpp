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

#include "asteroid.h"
#include "camera.h"
#include "debug_panel.h"
#include "dev_remote.h"
#include "dust.h"
#include "mesh_render.h"
#include "sprite.h"
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

    // Held-key flags, indexed by sapp key code. Flight physics reads these
    // every frame so we don't rely on key-repeat timing.
    std::array<bool, SAPP_MAX_KEYCODES> keys_down{};

    bool     mouse_captured = false;
    uint64_t last_frame_ticks = 0;
    uint64_t last_fps_ticks   = 0;
    uint32_t frames_since     = 0;

    // Anti-Boodler escape-hatch: one tap arms it, a second tap within 1s
    // actually quits. Single strays just flash a reminder in the terminal.
    uint64_t escape_armed_ticks = 0;

    std::string system_name = "troy";   // assets/systems/<name>.json
    StarSystem  system{};
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
    if (g.keys_down[SAPP_KEYCODE_SPACE])        t.Y += 1.0f;
    if (g.keys_down[SAPP_KEYCODE_LEFT_SHIFT])   t.Y -= 1.0f;
    return t;
}

// ---- sokol callbacks --------------------------------------------------------

void init_cb() {
    sg_desc desc{};
    desc.environment = sglue_environment();
    desc.logger.func = slog_func;
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
        sprite_light_editor::load_lights_sidecar(stem_full, s.lights);
        g.placed_sprites.push_back(s);
    }

    sapp_lock_mouse(true);
    g.mouse_captured = true;

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

    // Drain any queued dev_remote commands into game state BEFORE
    // physics / rendering — a /camera/set that arrived this frame
    // should be visible in the frame we're about to produce.
    dev_remote::drain_commands(g.camera);

    // --- physics ------------------------------------------------------------
    // Hold-Tab cruise: drive target to 1 while held, 0 otherwise. Camera
    // smooths the lerp so it feels like winding up and winding down.
    g.camera.cruise_target = g.keys_down[SAPP_KEYCODE_TAB] ? 1.0f : 0.0f;
    if (g.keys_down[SAPP_KEYCODE_X]) g.camera.brake();
    g.camera.apply_thrust(thrust_from_keys(), dt);
    g.camera.integrate(dt);

    // --- render -------------------------------------------------------------
    // --- build the on-screen HUD for this frame -----------------------------
    const float fb_w = (float)sapp_width();
    const float fb_h = (float)sapp_height();
    sdtx_canvas(fb_w * 0.5f, fb_h * 0.5f);
    sdtx_font(0);
    sdtx_color3f(0.7f, 1.0f, 0.9f);
    sdtx_pos(1.0f, 1.0f);

    const HMM_Vec3 p = g.camera.position;
    const float speed = HMM_LenV3(g.camera.velocity);
    const float dist  = HMM_LenV3(HMM_SubV3(g.sun.position, p));
    const char* mode  = (g.camera.cruise_level > 0.5f)  ? "CRUISE"
                      : (g.camera.cruise_level > 0.05f) ? "SPOOL "
                      :                                   "NORMAL";
    sdtx_printf("SPEED  %7.0f u/s\n", speed);
    sdtx_printf("MODE   %s\n",        mode);
    sdtx_printf("D(SUN) %7.0f u\n",   dist);
    sdtx_printf("POS    %5.0f %5.0f %5.0f\n", p.X, p.Y, p.Z);

    // Bottom-left: controls reminder in the second, thinner font.
    sdtx_font(1);
    sdtx_color3f(0.5f, 0.6f, 0.7f);
    sdtx_pos(1.0f, fb_h * 0.5f / 8.0f - 4.0f);   // 4 lines up from bottom
    sdtx_puts("W/S throttle  A/D strafe  SPC/SHIFT up/down\n");
    sdtx_puts("TAB cruise   X brake   RMB toggle cursor\n");
    sdtx_puts("CTRL+M debug panel   ESC x2 quit\n");

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
        g.sprite_render.draw(g.placed_sprites, g.camera, aspect, time_sec);
        g.sun.draw(g.camera, aspect, time_sec);
        sg_end_pass();
    }

    // Pass 2+3: bright-pass + separable gaussian into bloom_b.
    g.post.apply_bloom(g.rt);

    // Build the ImGui frame OUTSIDE any pass. This is only widget state;
    // no draw calls are issued yet. Slider mutations feed back into the
    // live PlacedMesh list so changes take effect on the *next* frame.
    debug_panel::build(g.placed_meshes);
    sprite_light_editor::build(g.placed_sprites);

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
        std::printf("[new_privateer] %4d fps  %6s  v=%7.1f u/s  d(sun)=%.0f  pos=(%.0f,%.0f,%.0f)\n",
                    fps, mode, speed, dist_to_sun,
                    p.X, p.Y, p.Z);
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
    // focus or ImGui input capture in debug_panel.
    if (sprite_light_editor::handle_event(ev)) return;
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
        if ((size_t)ev->key_code < g.keys_down.size()) g.keys_down[ev->key_code] = true;
        break;
    case SAPP_EVENTTYPE_KEY_UP:
        if ((size_t)ev->key_code < g.keys_down.size()) g.keys_down[ev->key_code] = false;
        break;
    case SAPP_EVENTTYPE_MOUSE_DOWN:
        if (ev->mouse_button == SAPP_MOUSEBUTTON_RIGHT) {
            g.mouse_captured = !g.mouse_captured;
            sapp_lock_mouse(g.mouse_captured);
        } else if (ev->mouse_button == SAPP_MOUSEBUTTON_LEFT && !g.mouse_captured) {
            g.mouse_captured = true;
            sapp_lock_mouse(true);
        }
        break;
    case SAPP_EVENTTYPE_MOUSE_MOVE:
        if (g.mouse_captured) g.camera.apply_mouse_delta(ev->mouse_dx, ev->mouse_dy);
        break;
    default:
        break;
    }
}

} // namespace

sapp_desc sokol_main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--system") == 0) {
            g.system_name = argv[i + 1];
            ++i;
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

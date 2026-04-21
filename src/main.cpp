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

#include "camera.h"
#include "dust.h"
#include "postprocess.h"
#include "render_config.h"
#include "rendertargets.h"
#include "skybox.h"
#include "sun.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

struct AppState {
    sg_pass_action scene_pass_action{};
    Camera         camera{};
    Skybox         skybox{};
    Sun            sun{};
    DustField      dust{};
    RenderTargets  rt{};
    PostProcess    post{};

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

    std::string seed = "troy";      // skybox directory under assets/skybox/
    std::string star = "yellow";    // stellar classification preset
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

    const std::string dir = "assets/skybox/" + g.seed;
    if (!g.skybox.init(dir, g.seed)) {
        std::fprintf(stderr, "[main] skybox init failed (run tools/gen_skybox.sh %s)\n",
                     g.seed.c_str());
        std::exit(1);
    }

    if (!g.sun.init())  { std::fprintf(stderr, "[main] sun init failed\n");  std::exit(1); }
    if (!g.dust.init()) { std::fprintf(stderr, "[main] dust init failed\n"); std::exit(1); }

    sapp_lock_mouse(true);
    g.mouse_captured = true;

    g.last_frame_ticks = stm_now();
    g.last_fps_ticks   = g.last_frame_ticks;
    std::printf("[new_privateer] backend=%d, skybox='%s', sun+dust up\n",
                (int)sg_query_backend(), g.seed.c_str());
}

void frame_cb() {
    // --- timestep -----------------------------------------------------------
    const uint64_t now = stm_now();
    const float    dt  = (float)stm_sec(stm_diff(now, g.last_frame_ticks));
    g.last_frame_ticks = now;

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
    sdtx_puts("ESC x2 quit\n");

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
        g.skybox.draw(g.camera, aspect);
        g.sun.draw(g.camera, aspect, time_sec);
        g.dust.draw(g.camera, aspect);
        sg_end_pass();
    }

    // Pass 2+3: bright-pass + separable gaussian into bloom_b.
    g.post.apply_bloom(g.rt);

    // Pass 4: composite to swapchain with lens flare + HUD.
    const HMM_Mat4 vp = HMM_MulM4(g.camera.projection(aspect), g.camera.view());
    const HMM_Vec3 flare_tint = g.sun.glow_color;
    g.post.composite_to_swapchain(g.rt, g.sun.position, vp, flare_tint,
                                  sapp_width(), sapp_height());

    sg_commit();

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
        std::printf("[new_privateer] %4.0f fps  %6s  v=%7.1f u/s  d(sun)=%.0f  pos=(%.0f,%.0f,%.0f)\n",
                    g.frames_since / elapsed, mode, speed, dist_to_sun,
                    p.X, p.Y, p.Z);
        g.last_fps_ticks = now;
        g.frames_since   = 0;
    }
}

void cleanup_cb() {
    sdtx_shutdown();
    g.post.destroy();
    g.rt.destroy();
    g.dust.destroy();
    g.sun.destroy();
    g.skybox.destroy();
    sg_shutdown();
}

void event_cb(const sapp_event* ev) {
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
        if (std::strcmp(argv[i], "--seed") == 0) {
            g.seed = argv[i + 1];
            ++i;
        } else if (std::strcmp(argv[i], "--star") == 0) {
            g.star = argv[i + 1];
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
    static std::string title = "new_privateer — stage 3: flight, sun, dust (" + g.seed + ")";
    desc.window_title = title.c_str();
    desc.high_dpi     = true;
    desc.sample_count = kSceneSampleCount;   // MSAA off → matches offscreen
    desc.logger.func  = slog_func;
    return desc;
}

#pragma once
// -----------------------------------------------------------------------------
// sprite.h — camera-facing billboard sprites with hull + lights layers.
//
// The "sprite-with-lights" concept:
//   * HULL layer   — a painted 2D sprite of the object (RGBA, alpha-blended).
//                    This is the primary art (rusted metal, shaded, baked AO).
//   * LIGHTS layer — a second texture with the SAME framing containing only
//                    bright emissive spots on a transparent background.
//                    Additively blended so it contributes to bloom.
//   * Animated spots (np-0kv.3, not yet) — per-object blinking lights driven
//                    by code, drawn additively on top of the lights layer.
//
// Each SpriteObject is rendered as a camera-facing quad at `position` whose
// longest side is `world_size` world units across. For non-square textures
// the shorter axis is scaled via an aspect multiplier so pixels stay square.
//
// Ownership: `hull` and `lights` are the TextureSlots this object owns. The
// SpriteRenderer does NOT own them — sprite assets live in a cache keyed by
// path (one copy per unique sprite file, shared across scene instances).
// Destroy semantics therefore live on the cache, not on this struct.
// -----------------------------------------------------------------------------

#include "sokol_gfx.h"
#include "camera.h"
#include "material.h"   // TextureSlot

#include <string>
#include <vector>

// How a LightSpot's intensity varies with time. All three take the same
// (hz, phase) parameterisation — hz=0 with kind=Steady is the obvious
// "always on" case. Phase is in cycles, i.e. phase=0.5 is half a period
// offset. Using turns (not radians) because the editor UI will expose a
// 0..1 slider and it's more intuitive for authors.
enum class LightKind : uint8_t {
    Steady = 0,   // always full brightness
    Pulse  = 1,   // sinusoidal 0.5..1.0 brightness
    Strobe = 2    // short bright flash, ~10% duty cycle
};

// One animated light on a sprite. Position is in UV space (0..1) so the
// same light definition works regardless of sprite resolution; the renderer
// projects UV into world space using the sprite's billboard basis.
struct LightSpot {
    float     u = 0.5f;          // 0..1, left-to-right on the sprite
    float     v = 0.5f;          // 0..1, top-to-bottom (matches hull UV)
    HMM_Vec3  color{1, 1, 1};    // base RGB in [0..1], gets bloom-amplified
    float     size = 30.0f;      // world-space radius of the glow
    float     hz   = 0.0f;       // cycles per second; 0 = no animation
    float     phase = 0.0f;      // 0..1, staggers blinks across multiple lights
    LightKind kind = LightKind::Steady;
};

// Stored art for a single sprite (hull + lights PNG pair). Loaded once per
// unique file path; multiple SpriteObjects can reference the same Art.
//
// `light_spots` are animated UV-space lights authored alongside this asset
// in `<base>.lights.json`. They live on the ART (not the instance) because
// they describe the cell itself, not where the cell is placed in the world.
// Every SpriteObject using this art inherits the same animated lights —
// crucial for ship-sprite atlases where one cell PNG is referenced by many
// runtime billboards (and we don't want to duplicate the LightSpot list per
// instance).
struct SpriteArt {
    std::string            name;            // diagnostic + sidecar lookup key
    TextureSlot            hull;            // alpha-blended base layer
    TextureSlot            lights;          // additively-blended emissive overlay
    int                    hull_w = 0;      // pixel dims for aspect correction
    int                    hull_h = 0;
    std::vector<LightSpot> light_spots;     // animated UV-space lights from sidecar

    void destroy();
};

// A single scene instance — "this mining base at this position, this big".
// `lights` is the per-instance list of animated glow spots (np-0kv.3). In
// the future this will come from a JSON sidecar authored by the editor UI
// (np-0kv.5); for now it's populated in code where the SpriteObject is built.
struct SpriteObject {
    const SpriteArt*       art = nullptr;   // non-owning; points into a cache
    HMM_Vec3               position{0, 0, 0};
    float                  world_size = 1000.0f;   // longest side in world units
    float                  roll_rad   = 0.0f;      // in-plane billboard rotation, clockwise-ish screen roll
    HMM_Vec4               tint       {1, 1, 1, 1};// rgb tint + global alpha
    std::vector<LightSpot> lights;                 // animated glow spots
};

// Renders a list of SpriteObjects in two passes: hull (alpha) then lights
// (additive). No depth write in the lights pass so additive overlaps blend
// cleanly. Sorting: hulls are drawn back-to-front by distance-to-camera so
// translucent edges composite correctly; lights are drawn in the same order
// (additive is commutative so order doesn't matter for correctness, just
// cache coherence).
struct SpriteRenderer {
    // Textured billboard (sprite.glsl) — used for hull + static lights PNG.
    sg_buffer   vbuf{};
    sg_buffer   ibuf{};
    sg_shader   shader{};
    sg_pipeline pipeline_hull{};      // alpha-blend
    sg_pipeline pipeline_lights{};    // additive (static lights texture)
    sg_sampler  sampler{};

    // Procedural glow spot (sprite_spot.glsl) — used for animated lights.
    // Same quad VBO as above; different shader with radial falloff + no
    // texture sampling. Additive blend, no depth write, tiny world-space
    // quads sized per-light.
    sg_shader   spot_shader{};
    sg_pipeline spot_pipeline{};

    bool init();
    void destroy();

    // Draws every sprite in `sprites`. `cam.view()` provides the camera
    // right/up vectors used for billboarding. `time_sec` drives light
    // animation (blink/pulse/strobe).
    void draw(const std::vector<SpriteObject>& sprites,
              const Camera& cam,
              float aspect,
              float time_sec) const;

    // Draws a list of free-standing additive glow points (no texture,
    // no light animation, no UV-into-billboard math). Used for projectile
    // tracers — same pipeline + shader as the animated light spots, just
    // taking world position + radius + colour directly. No depth write
    // so tracers stack additively when they overlap.
    struct Tracer {
        HMM_Vec3 position;
        HMM_Vec3 color;       // pre-bloom; bright values OK
        float    size = 5.0f; // world-unit radius
    };
    void draw_tracers(const std::vector<Tracer>& tracers,
                      const Camera& cam,
                      float aspect) const;
};

// Load the `<base_path>.png` + `<base_path>_lights.png` pair into `art`.
// Returns false if the hull PNG is missing; a missing lights PNG is
// allowed (the sprite renders with no emissive overlay).
bool load_sprite_art(const std::string& base_path, SpriteArt& art);

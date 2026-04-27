#pragma once
// -----------------------------------------------------------------------------
// ship_sprite.h — multi-view ship sprite atlas selection.
//
// Generic SpriteRenderer draws exactly one billboard art asset. Ship sprites
// need a tiny extra layer: choose the atlas frame whose authored az/el most
// closely matches the camera's view direction, then submit that frame as a
// normal SpriteObject. Keeps the renderer boring. Boring is good. Boring ships.
// -----------------------------------------------------------------------------

#include "camera.h"
#include "sprite.h"

#include <string>
#include <unordered_map>
#include <vector>

struct ShipSpriteFrame {
    float az_deg = 0.0f;
    float el_deg = 0.0f;
    float scale = 1.0f;      // per-frame visual correction multiplier
    float roll_deg = 0.0f;   // per-frame billboard roll correction
    // Precomputed unit vector for this cell's authored direction on the
    // view sphere: (cos(el)·sin(az), sin(el), cos(el)·cos(az)). Filled in
    // by load_ship_sprite_atlas after manifest + tuning load. The scorer
    // in choose_ship_sprite_frame_by_angles uses this for cosine-of-
    // great-circle-angle nearest-cell selection — gimbal-lock-immune at
    // the view-sphere poles (where (az, el) parameterization breaks down).
    HMM_Vec3 dir { 0.0f, 0.0f, 1.0f };
    const SpriteArt* art = nullptr;
};

struct ShipSpriteAtlas {
    std::string key;
    std::vector<ShipSpriteFrame> frames;
};

struct ShipSpriteObject {
    ShipSpriteAtlas* atlas = nullptr;
    HMM_Vec3 position{0, 0, 0};
    float world_size = 18.0f;
    HMM_Vec4 tint{1, 1, 1, 1};
    bool lights_enabled = true;

    // Kinematics. Identity orientation = nose along world +Z, top along
    // world +Y — matches the atlas-authoring convention where az=0 / el=0
    // places the camera in front of the ship's nose. Cell selection uses
    // the inverse of this quaternion to convert the world-frame to_cam
    // vector into ship-local az/el, so a ship spinning in world space
    // shows the right atlas frame at every instant.
    //
    // angular_velocity is in BODY frame, rad/s — "yaw at 60°/s around my
    // own up axis" stays correct after the ship has rolled. forward_speed
    // moves along body +Z at the given m/s; the runtime derives world
    // velocity each frame as `orientation · (0, 0, forward_speed)`. This
    // is the aircraft model: the nose tracks the motion. Free-strafe /
    // inertial drift are intentionally absent — that's a player-only
    // capability (camera.cpp).
    HMM_Quat orientation{0.0f, 0.0f, 0.0f, 1.0f};
    HMM_Vec3 angular_velocity{0.0f, 0.0f, 0.0f};
    float    forward_speed = 0.0f;

    // Atlas inspector override. When enabled, the in-world ship displays
    // the nearest authored frame to manual_{az,el}_deg instead of selecting
    // from camera position. This lets Mike scrub the Tarsus pose while the
    // camera stays fixed — exactly the art-review workflow we need.
    bool  manual_frame_enabled = false;
    float manual_az_deg = 0.0f;
    float manual_el_deg = -22.0f;

    // Debug breadcrumbs from the latest frame selection. Lets us print the
    // exact atlas az/el being used without recomputing and risking drift.
    // `debug_last_*_deg` is the AUTHORED frame az/el (post-binning, i.e. the
    // cell the renderer actually sampled). `debug_cam_*_deg` is the
    // ship-LOCAL camera-relative az/el (after un-rotating to_cam by
    // ship.orientation) — same quantity used for cell selection, so the F3
    // HUD reading directly explains which cell the engine chose and why.
    float debug_last_az_deg = 0.0f;
    float debug_last_el_deg = 0.0f;
    float debug_cam_az_deg  = 0.0f;
    float debug_cam_el_deg  = 0.0f;
};

// Per-frame integration of orientation + position for every ship that has
// non-zero angular_velocity or forward_speed. No-op for static ships, so
// it's safe to call unconditionally on the full vector. Call once before
// append_ship_sprites_for_camera so cell selection sees the post-step pose.
void update_ship_sprite_motion(std::vector<ShipSpriteObject>& ships, float dt);

bool load_ship_sprite_atlas(const std::string& atlas_stem,
                            ShipSpriteAtlas& atlas,
                            std::unordered_map<std::string, SpriteArt>& art_cache);

const ShipSpriteFrame* choose_ship_sprite_frame(const ShipSpriteAtlas& atlas,
                                                const ShipSpriteObject& ship,
                                                const Camera& cam);

const ShipSpriteFrame* choose_ship_sprite_frame_by_angles(const ShipSpriteAtlas& atlas,
                                                          float az_deg,
                                                          float el_deg);

void append_ship_sprites_for_camera(std::vector<ShipSpriteObject>& ships,
                                    const Camera& cam,
                                    std::vector<SpriteObject>& out_sprites);

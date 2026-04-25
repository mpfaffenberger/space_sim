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

    // Atlas inspector override. When enabled, the in-world ship displays
    // the nearest authored frame to manual_{az,el}_deg instead of selecting
    // from camera position. This lets Mike scrub the Tarsus pose while the
    // camera stays fixed — exactly the art-review workflow we need.
    bool  manual_frame_enabled = false;
    float manual_az_deg = 0.0f;
    float manual_el_deg = -22.0f;

    // Debug breadcrumbs from the latest frame selection. Lets us print the
    // exact atlas az/el being used without recomputing and risking drift.
    float debug_last_az_deg = 0.0f;
    float debug_last_el_deg = 0.0f;
};

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

#include "ship_sprite.h"
#include "json.h"

#include <cmath>
#include <cstdio>
#include <filesystem>

namespace {

constexpr float kPi = 3.14159265358979323846f;

float wrap_degrees(float deg) {
    float out = std::fmod(deg, 360.0f);
    return out < 0.0f ? out + 360.0f : out;
}

float angular_delta_degrees(float a, float b) {
    float d = std::fabs(wrap_degrees(a) - wrap_degrees(b));
    return d > 180.0f ? 360.0f - d : d;
}

std::string strip_assets_prefix(std::string path) {
    constexpr const char* prefix = "assets/";
    if (path.rfind(prefix, 0) == 0) return path.substr(7);
    return path;
}

std::string strip_png_suffix(std::string path) {
    constexpr const char* suffix = ".png";
    if (path.size() >= 4 && path.compare(path.size() - 4, 4, suffix) == 0) {
        path.resize(path.size() - 4);
    }
    return path;
}

// Camera-relative spherical coordinates of the camera as seen from the ship.
// az: atan2(X, Z) in degrees, wrapped 0..360. Matches authored atlas frames
// where az=0 means "camera at ship's +Z (in front of nose)" and az grows
// clockwise when viewed from above.
// el: asin(Y / |to_cam|) in degrees, [-90,+90]. Positive = camera above ship.
// Returns false at coincident position (no meaningful direction).
bool compute_cam_az_el(const HMM_Vec3& ship_pos,
                      const HMM_Vec3& cam_pos,
                      float& out_az_deg,
                      float& out_el_deg) {
    const HMM_Vec3 to_cam = HMM_SubV3(cam_pos, ship_pos);
    const float len2 = HMM_DotV3(to_cam, to_cam);
    if (len2 <= 0.0001f) return false;
    const float len = std::sqrt(len2);
    out_az_deg = wrap_degrees(std::atan2(to_cam.X, to_cam.Z) * 180.0f / kPi);
    out_el_deg = std::asin(std::max(-1.0f, std::min(1.0f, to_cam.Y / len))) * 180.0f / kPi;
    return true;
}

} // namespace

bool load_ship_sprite_atlas(const std::string& atlas_stem,
                            ShipSpriteAtlas& atlas,
                            std::unordered_map<std::string, SpriteArt>& art_cache) {
    const std::string path = "assets/" + atlas_stem + ".json";
    if (!std::filesystem::exists(path)) {
        std::fprintf(stderr, "[ship-sprite] atlas not found: %s\n", path.c_str());
        return false;
    }

    const json::Value root = json::parse_file(path);
    const json::Value* samples = root.find("samples");
    if (!root.is_object() || !samples || !samples->is_array()) {
        std::fprintf(stderr, "[ship-sprite] bad atlas manifest: %s\n", path.c_str());
        return false;
    }

    atlas.key = atlas_stem;
    atlas.frames.clear();

    for (const json::Value& sample : samples->as_array()) {
        if (!sample.is_object()) continue;
        const json::Value* sprite = sample.find("sprite");
        if (!sprite || !sprite->is_string()) continue;

        const std::string stem = strip_png_suffix(strip_assets_prefix(sprite->as_string()));
        auto [it, inserted] = art_cache.try_emplace(stem, SpriteArt{});
        if (inserted && !load_sprite_art("assets/" + stem, it->second)) {
            std::fprintf(stderr, "[ship-sprite] skipping atlas frame '%s'\n", stem.c_str());
            art_cache.erase(it);
            continue;
        }

        ShipSpriteFrame frame{};
        if (const json::Value* az = sample.find("az")) frame.az_deg = az->as_float();
        if (const json::Value* el = sample.find("el")) frame.el_deg = el->as_float();
        if (const json::Value* scale = sample.find("scale")) frame.scale = scale->as_float();
        if (const json::Value* roll = sample.find("roll_deg")) frame.roll_deg = roll->as_float();
        frame.art = &it->second;
        atlas.frames.push_back(frame);
    }

    const std::string tuning_path = "assets/" + atlas_stem + ".tuning.json";
    if (std::filesystem::exists(tuning_path)) {
        const json::Value tuning = json::parse_file(tuning_path);
        if (const json::Value* frames = tuning.find("frames"); frames && frames->is_array()) {
            for (const json::Value& t : frames->as_array()) {
                if (!t.is_object()) continue;
                const float az = t.find("az") ? t.find("az")->as_float() : 0.0f;
                const float el = t.find("el") ? t.find("el")->as_float() : 0.0f;
                for (ShipSpriteFrame& f : atlas.frames) {
                    if ((int)std::round(f.az_deg) == (int)std::round(az) &&
                        (int)std::round(f.el_deg) == (int)std::round(el)) {
                        if (const json::Value* scale = t.find("scale")) f.scale = scale->as_float();
                        if (const json::Value* roll = t.find("roll_deg")) f.roll_deg = roll->as_float();
                    }
                }
            }
        }
    }

    std::printf("[ship-sprite] loaded '%s' frames=%zu\n", path.c_str(), atlas.frames.size());
    return !atlas.frames.empty();
}

const ShipSpriteFrame* choose_ship_sprite_frame_by_angles(const ShipSpriteAtlas& atlas,
                                                          float az_deg,
                                                          float el_deg) {
    if (atlas.frames.empty()) return nullptr;

    const ShipSpriteFrame* best = nullptr;
    float best_score = 1.0e30f;
    for (const ShipSpriteFrame& frame : atlas.frames) {
        const float da = angular_delta_degrees(frame.az_deg, az_deg);
        const float de = frame.el_deg - el_deg;
        const float score = da * da + de * de * 2.0f;
        if (score < best_score) {
            best_score = score;
            best = &frame;
        }
    }
    return best;
}

const ShipSpriteFrame* choose_ship_sprite_frame(const ShipSpriteAtlas& atlas,
                                                const ShipSpriteObject& ship,
                                                const Camera& cam) {
    if (ship.manual_frame_enabled) {
        return choose_ship_sprite_frame_by_angles(atlas, ship.manual_az_deg, ship.manual_el_deg);
    }
    float az = 0.0f, el = 0.0f;
    if (!compute_cam_az_el(ship.position, cam.position, az, el)) {
        return atlas.frames.empty() ? nullptr : &atlas.frames.front();
    }
    return choose_ship_sprite_frame_by_angles(atlas, az, el);
}

void append_ship_sprites_for_camera(std::vector<ShipSpriteObject>& ships,
                                    const Camera& cam,
                                    std::vector<SpriteObject>& out_sprites) {
    // Per-frame billboard roll: align each cell's CAPTURE-UP direction
    // with screen-up.
    //
    // Each atlas cell at (az, el) was captured with the camera at
    //   cap_pos = R · (cos(el)·sin(az), sin(el), cos(el)·cos(az))
    // looking back at the origin, with no roll applied. The corresponding
    // capture cam-up vector in world space (which is the direction that
    // appears as "up" inside the captured PNG) is
    //
    //   cap_up(az, el) = ( -sin(el)·sin(az),  cos(el),  -sin(el)·cos(az) )
    //
    // For an equator cell (el=0) this collapses to world up (0,1,0). For
    // higher-elevation cells it tilts away from world up — that's the
    // capture camera's pitch baked into the image.
    //
    // The billboard is camera-facing, so its only freedom is `roll_rad`
    // (an in-plane 2D rotation in the camera's right/up basis). We choose
    // the rotation that maps the cell's image +up axis onto cap_up's
    // current screen projection:
    //
    //   cu_x = cap_up · cam_right          (screen-x of cap_up)
    //   cu_y = cap_up · cam_up             (screen-y of cap_up)
    //   align_roll = atan2(-cu_x, cu_y)
    //
    // What this gives us:
    //   * When the runtime camera matches the cell's authored direction,
    //     cap_up == runtime cam_up, cu_x = 0, cu_y = 1, align_roll = 0 —
    //     i.e. the cell renders as captured. Correct by construction.
    //   * When the runtime camera differs from the cell's authored
    //     direction (the common case, since selection rounds), the cell
    //     is rotated so its painted "up" still lands on screen where its
    //     authored 3D up direction projects. The visible silhouette is a
    //     little off but the orientation reads correctly.
    //   * Camera ROLL automatically tilts the rendered sprite the right
    //     way: rolling rotates cam_right/cam_up, which rotates cap_up's
    //     screen projection, which rotates align_roll. No yaw/pitch leak
    //     because cap_up is fixed in world space — only the projection
    //     onto the camera basis changes.
    //
    // Why this replaces the old screen-projected NOSE alignment: the old
    // math projected world +Z (ship-forward) onto screen and rotated the
    // cell to match. That tries to keep the rendered nose pinned to a
    // world axis, but goes singular when the camera looks along that
    // axis (atan2(tiny, tiny) flips on tiny camera nudges). cap_up is
    // perpendicular to the look direction by construction, so it never
    // shrinks to zero — vastly more stable.
    const HMM_Vec3 cam_right = cam.right();
    const HMM_Vec3 cam_up    = cam.up();

    for (ShipSpriteObject& ship : ships) {
        if (!ship.atlas) continue;

        // Publish the raw camera-relative az/el for the F3 atlas-frame HUD.
        // For ships under manual override we also expose the slider values
        // here so the HUD reflects the user-driven angles instead of the
        // (now-irrelevant) camera position.
        if (ship.manual_frame_enabled) {
            ship.debug_cam_az_deg = ship.manual_az_deg;
            ship.debug_cam_el_deg = ship.manual_el_deg;
        } else {
            float cam_az = 0.0f, cam_el = 0.0f;
            if (compute_cam_az_el(ship.position, cam.position, cam_az, cam_el)) {
                ship.debug_cam_az_deg = cam_az;
                ship.debug_cam_el_deg = cam_el;
            }
        }

        const ShipSpriteFrame* frame = choose_ship_sprite_frame(*ship.atlas, ship, cam);
        if (!frame || !frame->art) continue;

        ship.debug_last_az_deg = frame->az_deg;
        ship.debug_last_el_deg = frame->el_deg;

        // Capture-up axis for this cell (see comment at function top).
        const float az_rad = frame->az_deg * kPi / 180.0f;
        const float el_rad = frame->el_deg * kPi / 180.0f;
        const float sin_az = std::sin(az_rad);
        const float cos_az = std::cos(az_rad);
        const float sin_el = std::sin(el_rad);
        const float cos_el = std::cos(el_rad);
        const HMM_Vec3 cap_up = HMM_V3(-sin_el * sin_az,
                                        cos_el,
                                       -sin_el * cos_az);
        const float cu_x = HMM_DotV3(cap_up, cam_right);
        const float cu_y = HMM_DotV3(cap_up, cam_up);
        const float align_roll = std::atan2(-cu_x, cu_y);

        SpriteObject sprite{};
        sprite.art = frame->art;
        sprite.position = ship.position;
        sprite.world_size = ship.world_size * frame->scale;
        // Authored per-cell roll plus the cap-up alignment roll. See the
        // "Per-frame billboard roll" comment above for the derivation.
        sprite.roll_rad = frame->roll_deg * kPi / 180.0f + align_roll;
        sprite.tint = ship.tint;
        // Animated lights authored on this cell's `.lights.json` ride along
        // with the picked frame. Copy (not reference) so the renderer's pass
        // over `out_sprites` doesn't dangle when the next frame swaps SpriteArt
        // pointers under us. Debug comparison scenes can disable this so glow
        // sprites don't hide the hull silhouette we're trying to inspect.
        if (ship.lights_enabled) {
            sprite.lights = frame->art->light_spots;
        }
        out_sprites.push_back(sprite);
    }
}

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
    // Per-frame roll alignment for the atlas billboard.
    //
    // Goal: render each picked atlas frame so the SHIP-FORWARD direction
    // in the rendered image lands on screen where world +Z (the ship's
    // forward axis) actually projects under the engine camera. This is
    // the alignment the player expects when comparing the sprite to a
    // debug arrow drawn along world +Z.
    //
    // Two unit vectors do the heavy lifting:
    //
    //   img(az, el) = ( -sin(az),  +cos(az)·sin(el) )
    //     The 2D direction in the VISIBLE runtime atlas PNG that corresponds
    //     to world +Z at the capture pose. The raw capture-space derivation
    //     gives -cos(az)·sin(el), but our atlas cell path bakes a vertical
    //     flip on disk and the sprite shader flips V again for Metal/stb
    //     texture-origin sanity. Net result: the authored image's in-plane
    //     Y direction is mirrored relative to the 3D projection math. X is
    //     unchanged; only image-space Y flips. Tiny sign, giant headache.
    //
    //   scr        = ( world_fwd · cam_right,  world_fwd · cam_up )
    //     The 2D direction in the engine's screen plane where world +Z
    //     projects right now.
    //
    // Both live in the same (right, up) basis convention, so the roll
    // that maps img onto scr is simply
    //
    //   align_roll = atan2(scr.y, scr.x) − atan2(img.y, img.x)
    //
    // Why this is smooth across atlas slot boundaries: when the picked
    // frame switches (say cap_az 315° → 337.5°), img_angle jumps, but
    // the new authored image has its ship-forward at the new img_angle,
    // so the rendered ship-forward direction lands at the SAME screen
    // angle on either side of the boundary. The visual result is a
    // continuous nose direction with at most a tiny silhouette change.
    //
    // Degeneracies. img → 0 when ship-forward is parallel to cap_F
    // (front/back equator views: az=0/180 with el=0). scr → 0 when the
    // engine camera is looking straight along world +Z. In either case
    // there is no in-plane direction to align, so we fall back to
    // aligning the picked frame's capture-up axis (cap_U) with the
    // engine's screen-up — which is well-defined for every authored
    // frame because cap_U is perpendicular to cap_F by construction.
    //
    //   cap_U(az, el) = ( -sin(el)·sin(az),  cos(el),  -sin(el)·cos(az) )
    //
    // Visible-image up = +cap_U (world up at capture pose). The disk-flip
    // and shader V-flip cancel each other for the perpendicular up axis,
    // unlike the in-plane forward-axis term which only carries one mirror.
    // So the fallback aligns +cap_U (NOT -cap_U) with cam_up. Hard-learned.
    constexpr float kMinMag2 = 0.04f; // ~12° off the degenerate axis

    const HMM_Vec3 world_fwd = HMM_V3(0.0f, 0.0f, 1.0f);
    const HMM_Vec3 cam_right = cam.right();
    const HMM_Vec3 cam_up    = cam.up();

    const float scr_x = HMM_DotV3(world_fwd, cam_right);
    const float scr_y = HMM_DotV3(world_fwd, cam_up);
    const float scr_mag2 = scr_x * scr_x + scr_y * scr_y;
    const float scr_angle = std::atan2(scr_y, scr_x);

    for (ShipSpriteObject& ship : ships) {
        if (!ship.atlas) continue;
        const ShipSpriteFrame* frame = choose_ship_sprite_frame(*ship.atlas, ship, cam);
        if (!frame || !frame->art) continue;

        ship.debug_last_az_deg = frame->az_deg;
        ship.debug_last_el_deg = frame->el_deg;

        const float az_rad = frame->az_deg * kPi / 180.0f;
        const float el_rad = frame->el_deg * kPi / 180.0f;
        const float sin_az = std::sin(az_rad);
        const float cos_az = std::cos(az_rad);
        const float sin_el = std::sin(el_rad);
        const float cos_el = std::cos(el_rad);

        const float img_x = -sin_az;
        const float img_y = cos_az * sin_el;
        const float img_mag2 = img_x * img_x + img_y * img_y;

        float align_roll;
        if (img_mag2 > kMinMag2 && scr_mag2 > kMinMag2) {
            align_roll = scr_angle - std::atan2(img_y, img_x);
        } else {
            // Pole / head-on fallback: align +cap_U with cam_up.
            const HMM_Vec3 cap_up = HMM_V3(-sin_el * sin_az, cos_el,
                                           -sin_el * cos_az);
            const float a = HMM_DotV3(cap_up, cam_right);
            const float b = HMM_DotV3(cap_up, cam_up);
            align_roll = std::atan2(-a, b);
        }

        SpriteObject sprite{};
        sprite.art = frame->art;
        sprite.position = ship.position;
        sprite.world_size = ship.world_size * frame->scale;
        sprite.roll_rad = frame->roll_deg * kPi / 180.0f + align_roll;
        sprite.tint = ship.tint;
        out_sprites.push_back(sprite);
    }
}

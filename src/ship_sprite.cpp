#include "ship_sprite.h"

#include "world_scale.h"
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

// Camera position expressed in the ship's BODY frame as spherical (az, el).
// az: atan2(X_body, Z_body) in degrees, wrapped 0..360. Matches authored
// atlas frames where az=0 means "camera in front of the ship's nose"
// (body +Z) and az grows clockwise when viewed from above (right-hand
// rotation around body +Y takes body +Z toward body +X).
// el: asin(Y_body / |to_cam|) in degrees, [-90,+90]. Positive = camera
// above the ship's belly (body +Y).
//
// `ship_orientation` is the ship's world-frame orientation quaternion.
// We rotate the world-frame to_cam vector by its inverse so the (az, el)
// we hand to the cell selector are ship-local: a ship turning in world
// space therefore picks a different cell at every instant even though
// the camera hasn't moved. Identity orientation collapses this to the
// pre-existing world-frame behavior — back-compat for static scenes.
//
// Returns false at coincident position (no meaningful direction).
bool compute_cam_az_el(const HMM_Vec3& ship_pos,
                      const HMM_Quat& ship_orientation,
                      const HMM_Vec3& cam_pos,
                      float& out_az_deg,
                      float& out_el_deg) {
    const HMM_Vec3 to_cam_world = HMM_SubV3(cam_pos, ship_pos);
    const float len2 = HMM_DotV3(to_cam_world, to_cam_world);
    if (len2 <= 0.0001f) return false;

    // Rotation matrices: world←body is QToM4. body←world is its transpose
    // (rotation matrices are orthonormal). Stash the world matrix; some
    // callers will want it too in future, but for now we only need the
    // inverse to push to_cam into the body frame.
    const HMM_Mat4 R_world_from_body = HMM_QToM4(ship_orientation);
    const HMM_Mat4 R_body_from_world = HMM_TransposeM4(R_world_from_body);
    const HMM_Vec4 v4 = HMM_MulM4V4(R_body_from_world,
                                    HMM_V4(to_cam_world.X, to_cam_world.Y, to_cam_world.Z, 0.0f));
    const HMM_Vec3 to_cam_body = HMM_V3(v4.X, v4.Y, v4.Z);

    const float len = std::sqrt(len2);  // |v| is rotation-invariant
    out_az_deg = wrap_degrees(std::atan2(to_cam_body.X, to_cam_body.Z) * 180.0f / kPi);
    out_el_deg = std::asin(std::max(-1.0f, std::min(1.0f, to_cam_body.Y / len))) * 180.0f / kPi;
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

    // Precompute each frame's unit-vector direction on the view sphere.
    // Done once at load time after both manifest and tuning have been
    // applied; the scorer reads `frame.dir` directly without trig per
    // call. See ShipSpriteFrame::dir comment for the parameterization.
    for (ShipSpriteFrame& frame : atlas.frames) {
        const float az_rad = frame.az_deg * kPi / 180.0f;
        const float el_rad = frame.el_deg * kPi / 180.0f;
        frame.dir = HMM_V3(std::cos(el_rad) * std::sin(az_rad),
                            std::sin(el_rad),
                            std::cos(el_rad) * std::cos(az_rad));
    }

    std::printf("[ship-sprite] loaded '%s' frames=%zu\n", path.c_str(), atlas.frames.size());
    return !atlas.frames.empty();
}

const ShipSpriteFrame* choose_ship_sprite_frame_by_angles(const ShipSpriteAtlas& atlas,
                                                          float az_deg,
                                                          float el_deg) {
    if (atlas.frames.empty()) return nullptr;

    // Score by cosine of the great-circle angle between the query and
    // each cell's authored direction on the view sphere. Pick the cell
    // with the maximum cosine = the smallest 3D angle = the closest
    // authored viewpoint to the runtime camera.
    //
    // Why not the old `da² + 2·de²` (angular delta in az and el)? That
    // metric lives in the (az, el) parameter space, where a degree of
    // azimuth at the equator covers a full degree of arc on the sphere
    // but a degree of azimuth near the pole covers almost no arc at
    // all. The penalty is wildly miscalibrated near the poles.
    //
    // Concrete consequence with the old scorer: a pole cell at (az=0,
    // el=90) competing against (az=180, el=60) for a runtime sample at
    // (az=180, el=85) — i.e. the camera is 5° from the pole, on the
    // far side. Old scorer:
    //   pole cell:      180² + 2·5²   = 32450
    //   el=60 az=180:     0² + 2·25²  =  1250    ← wins
    // Even though the runtime view is way closer to the pole than to
    // (180, 60) in actual 3D angle (5° vs 25°), the az penalty crushes
    // it. The pole cell only ever wins inside a ~25-40° az cone around
    // az=0, which defeats the entire point of authoring it.
    //
    // With cosine scoring on the same example:
    //   pole cell:      dot = sin(85°)                = 0.996
    //   el=60 az=180:   dot = cos(25°) = sin(65°)     = 0.906
    // Pole wins, as it should. And it wins for *every* az above
    // el ≈ 75°, which is the actual halfway point in 3D angle between
    // an el=60 ring cell and an el=90 pole cell.
    //
    // Cost: one HMM_DotV3 per frame (3 muls + 2 adds) instead of the
    // old (subtract, abs, fmod, conditional, mul, mul, add). Each
    // frame's `dir` was precomputed at atlas load, so no trig in the
    // hot path. Net: this is also faster, not just more correct.
    const float az_rad = az_deg * kPi / 180.0f;
    const float el_rad = el_deg * kPi / 180.0f;
    const HMM_Vec3 qdir = HMM_V3(std::cos(el_rad) * std::sin(az_rad),
                                  std::sin(el_rad),
                                  std::cos(el_rad) * std::cos(az_rad));

    const ShipSpriteFrame* best = nullptr;
    float best_dot = -2.0f;   // strictly less than any real cosine ∈ [-1, 1]
    for (const ShipSpriteFrame& frame : atlas.frames) {
        const float dot = HMM_DotV3(qdir, frame.dir);
        if (dot > best_dot) {
            best_dot = dot;
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
    if (!compute_cam_az_el(ship.position, ship.orientation, cam.position, az, el)) {
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
            if (compute_cam_az_el(ship.position, ship.orientation, cam.position, cam_az, cam_el)) {
                ship.debug_cam_az_deg = cam_az;
                ship.debug_cam_el_deg = cam_el;
            }
        }

        const ShipSpriteFrame* frame = choose_ship_sprite_frame(*ship.atlas, ship, cam);
        if (!frame || !frame->art) continue;

        ship.debug_last_az_deg = frame->az_deg;
        ship.debug_last_el_deg = frame->el_deg;

        // Capture-up axis for this cell (see comment at function top).
        // The (az, el) parameterization is in the ship's BODY frame, so the
        // formula yields cap_up_body — the painted-up direction expressed
        // in ship-local coordinates. To project onto the camera's screen
        // basis we have to push it back into world space via the ship's
        // current orientation. For a static (identity) ship this is a no-op
        // and reproduces the pre-orientation behavior; for a turning ship
        // it rotates the billboard's roll alignment with the hull, so the
        // ship's own "up" tracks its yaw/pitch/roll instead of being pinned
        // to world up.
        const float az_rad = frame->az_deg * kPi / 180.0f;
        const float el_rad = frame->el_deg * kPi / 180.0f;
        const float sin_az = std::sin(az_rad);
        const float cos_az = std::cos(az_rad);
        const float sin_el = std::sin(el_rad);
        const float cos_el = std::cos(el_rad);
        const HMM_Vec3 cap_up_body = HMM_V3(-sin_el * sin_az,
                                             cos_el,
                                            -sin_el * cos_az);
        const HMM_Mat4 R_world_from_body = HMM_QToM4(ship.orientation);
        const HMM_Vec4 cu4 = HMM_MulM4V4(R_world_from_body,
                                         HMM_V4(cap_up_body.X, cap_up_body.Y, cap_up_body.Z, 0.0f));
        const HMM_Vec3 cap_up = HMM_V3(cu4.X, cu4.Y, cu4.Z);
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

// -----------------------------------------------------------------------------
// Per-frame motion integration.
//
// Each ship advances its orientation by the body-frame angular velocity and
// translates along its (body) +Z by forward_speed. World-frame velocity is
// derived implicitly via `world_v = orientation · (0, 0, forward_speed)` —
// no separately-stored linear velocity, so the nose always tracks the
// motion (aircraft model, locked-in by `header comment on ShipSpriteObject`).
//
// Convention check: with identity orientation, body +Z = world +Z and body
// +Y = world +Y. A positive yaw rate (angular_velocity.Y > 0) is a right-
// hand rotation around body +Y, which takes body +Z toward body +X — i.e.
// the ship turns RIGHT when viewed from above. forward_speed = 20,
// angular_velocity_deg.Y = 60 therefore traces a clockwise (viewed from
// +Y) circle of radius v/|omega| ≈ 19.1 m, completing one lap every 6 s.
//
// Composing in BODY frame (`q · dq`) instead of world frame (`dq · q`) is
// what makes the angular velocity body-relative — "yaw 60°/s around my own
// up axis" stays correct after the ship has rolled, which is the whole
// reason we wanted quaternions in the first place. The HMM_NormQ at the
// end fights floating-point drift over many composes (same trick as
// camera.cpp::compose_local).
// -----------------------------------------------------------------------------
void update_ship_sprite_motion(std::vector<ShipSpriteObject>& ships, float dt) {
    for (ShipSpriteObject& s : ships) {
        // Static-ship fast path. Both checks are needed because either DOF
        // alone is meaningful — a ship with forward_speed = 0 but non-zero
        // angular_velocity should still spin in place.
        const float omega2 = HMM_DotV3(s.angular_velocity, s.angular_velocity);
        const bool turning = omega2 > 1e-10f;             // ~6 millideg/s threshold
        const bool moving  = std::fabs(s.forward_speed) > 1e-6f;
        if (!turning && !moving) continue;

        if (turning) {
            const float omega_mag = std::sqrt(omega2);
            const HMM_Vec3 axis_body = HMM_DivV3F(s.angular_velocity, omega_mag);
            const HMM_Quat dq = HMM_QFromAxisAngle_RH(axis_body, omega_mag * dt);
            s.orientation = HMM_NormQ(HMM_MulQ(s.orientation, dq));
        }

        if (moving) {
            // Linear-velocity scaling per world_scale.h: ships slide
            // through space at the global pacing rate. Turn rates
            // (angular_velocity above) intentionally unscaled so AI
            // tracking responsiveness stays at canonical levels.
            const HMM_Mat4 R = HMM_QToM4(s.orientation);
            const HMM_Vec4 fwd_world4 = HMM_MulM4V4(R, HMM_V4(0.0f, 0.0f, s.forward_speed, 0.0f));
            s.position = HMM_AddV3(s.position,
                HMM_MulV3F(HMM_V3(fwd_world4.X, fwd_world4.Y, fwd_world4.Z),
                           dt * world_scale::k_world_velocity_scale));
        }

        // Collision-drift integration. Added even for ships that aren't
        // "moving" by forward-speed metrics, because a stationary ship
        // can still be drifting after being rammed. Decays exponentially
        // (half-life ~1.4s at the 0.5/s rate); without decay the bounce
        // would persist forever in Newtonian space, which is
        // technically correct but reads as "ship is permanently drifting
        // sideways" instead of "ship got hit and is recovering".
        const float cv2 = HMM_DotV3(s.collision_velocity, s.collision_velocity);
        if (cv2 > 1e-6f) {
            s.position = HMM_AddV3(s.position,
                HMM_MulV3F(s.collision_velocity,
                           dt * world_scale::k_world_velocity_scale));
            s.collision_velocity = HMM_MulV3F(s.collision_velocity,
                                              std::exp(-0.5f * dt));
        }
    }
}

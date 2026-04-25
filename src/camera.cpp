#include "camera.h"

#include <algorithm>
#include <cmath>

// ---- helpers ---------------------------------------------------------------

namespace {

// camera-to-world rotation derived from the orientation quaternion.
// Kept in one place so view() and basis accessors stay consistent.
HMM_Mat4 world_from_camera(const Camera& c) {
    return HMM_QToM4(c.orientation);
}

HMM_Vec3 mul_dir(const HMM_Mat4& m, HMM_Vec3 v) {
    const HMM_Vec4 r = HMM_MulM4V4(m, HMM_V4(v.X, v.Y, v.Z, 0.0f));
    return HMM_V3(r.X, r.Y, r.Z);
}

} // namespace

// ---- input -----------------------------------------------------------------

// Compose a small rotation into the orientation, in LOCAL frame:
//   orientation = orientation * delta
// is the canonical "rotate around the ship's current axes" operation,
// which is what gives us no gimbal lock. yaw_rad rotates around local
// +Y (ship up), pitch_rad around local +X (ship right). The HMM_NormQ
// at the end keeps the quaternion unit-length over many composes —
// without it, floating-point drift would slowly desynchronize basis
// vectors over a long flight.
static void compose_local(Camera& c, float yaw_rad, float pitch_rad) {
    const HMM_Quat dq_yaw   = HMM_QFromAxisAngle_RH(HMM_V3(0.0f, 1.0f, 0.0f), yaw_rad);
    const HMM_Quat dq_pitch = HMM_QFromAxisAngle_RH(HMM_V3(1.0f, 0.0f, 0.0f), pitch_rad);
    c.orientation = HMM_NormQ(
        HMM_MulQ(HMM_MulQ(c.orientation, dq_yaw), dq_pitch));
}

void Camera::apply_mouse_delta(float dx, float dy) {
    // FPS-style mouselook (legacy path; kept callable but currently
    // unused — fly-by-wire is the active input). Sign convention:
    //   * dx > 0 (mouse right) → yaw_rad < 0 → ship turns right
    //   * dy > 0 (mouse down)  → pitch_rad < 0 → ship pitches down
    //                            (engine-Y quirk: + sign maps to +screen_y)
    const float yaw_rad   = -dx * mouse_sensitivity;
    const float pitch_rad = +dy * mouse_sensitivity;   // engine-Y flip
    compose_local(*this, yaw_rad, pitch_rad);
}

void Camera::apply_mouse_aim(float off_x, float off_y, float dt) {
    // Soft dead-zone with linear remap of the outside region. Inside
    // dead_zone → exactly 0 (no drift). Outside → linear ramp to ±1.
    const float dz_size = mouse_dead_zone;
    auto dz = [dz_size](float x) {
        const float a = std::abs(x);
        if (a <= dz_size) return 0.0f;
        const float sign = x < 0.0f ? -1.0f : 1.0f;
        const float t    = (a - dz_size) / (1.0f - dz_size);
        return sign * std::clamp(t, 0.0f, 1.0f);
    };

    // Sign convention matches apply_mouse_delta. Pitch flip mirrors the
    // engine's world-up = +screen_y rendering convention (see camera.h
    // header note + the same flip in cockpit_hud nav reticle).
    const float yaw_rad   = -dz(off_x) * max_yaw_rate   * dt;
    const float pitch_rad = +dz(off_y) * max_pitch_rate * dt;
    compose_local(*this, yaw_rad, pitch_rad);
    // No pitch clamp — quaternion + local-frame composition is happy at
    // any angle, no pole degeneracy. This is the whole reason we moved
    // off Euler angles.
}

void Camera::apply_thrust(HMM_Vec3 local_dir, float dt) {
    // Cruise multiplier lerps with cruise_level so engage/disengage feels
    // like a ramp, not a binary switch.
    const float t_mult  = 1.0f + (cruise_thrust_mult - 1.0f) * cruise_level;
    const HMM_Mat4 R = world_from_camera(*this);
    const HMM_Vec3 world_accel = HMM_MulV3F(mul_dir(R, local_dir),
                                            thrust_accel * t_mult);
    velocity = HMM_AddV3(velocity, HMM_MulV3F(world_accel, dt));
}

void Camera::integrate(float dt) {
    // Smooth cruise_level toward its target with an exponential rate.
    // k = 1 - exp(-rate * dt) lerps toward the target by k each frame,
    // which is frame-rate independent and gives an asymptotic approach.
    const float k = 1.0f - std::exp(-cruise_lerp_rate * dt);
    cruise_level += (cruise_target - cruise_level) * k;

    // Cruise also *reduces* damping (higher terminal velocity AND coastier
    // feel) so engaging cruise reads as "the engine wound up."
    const float d_div = 1.0f + (cruise_damp_div - 1.0f) * cruise_level;
    const float damp  = linear_damping / d_div;

    if (damp > 0.0f) {
        velocity = HMM_MulV3F(velocity, std::exp(-damp * dt));
    }
    position = HMM_AddV3(position, HMM_MulV3F(velocity, dt));
}

void Camera::brake() { velocity = HMM_V3(0.0f, 0.0f, 0.0f); }

// ---- queries ---------------------------------------------------------------

HMM_Vec3 Camera::forward() const { return mul_dir(world_from_camera(*this), HMM_V3( 0.0f, 0.0f, -1.0f)); }
HMM_Vec3 Camera::right()   const { return mul_dir(world_from_camera(*this), HMM_V3( 1.0f, 0.0f,  0.0f)); }
HMM_Vec3 Camera::up()      const { return mul_dir(world_from_camera(*this), HMM_V3( 0.0f, 1.0f,  0.0f)); }

HMM_Mat4 Camera::view() const {
    // view = (T * R)^-1, with T being translation-by-position. Rather than
    // inverting a general 4x4 we do it by hand: R^T then translate by -pos.
    const HMM_Mat4 Rinv = HMM_TransposeM4(world_from_camera(*this));
    const HMM_Mat4 Tinv = HMM_Translate(HMM_V3(-position.X, -position.Y, -position.Z));
    return HMM_MulM4(Rinv, Tinv);
}

HMM_Mat4 Camera::view_rotation_only() const {
    return HMM_TransposeM4(world_from_camera(*this));
}

HMM_Mat4 Camera::projection(float aspect) const {
    return HMM_Perspective_RH_NO(effective_fov(), aspect, near_plane, far_plane);
}

float Camera::effective_fov() const {
    return fov_y_radians + cruise_fov_extra * cruise_level;
}

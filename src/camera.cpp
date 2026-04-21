#include "camera.h"

#include <algorithm>
#include <cmath>

// ---- helpers ---------------------------------------------------------------

namespace {

// camera-to-world rotation, derived from yaw then pitch. Kept in one place so
// view() and basis accessors stay consistent.
HMM_Mat4 world_from_camera(const Camera& c) {
    const HMM_Mat4 ry = HMM_Rotate_RH(c.yaw,   HMM_V3(0.0f, 1.0f, 0.0f));
    const HMM_Mat4 rx = HMM_Rotate_RH(c.pitch, HMM_V3(1.0f, 0.0f, 0.0f));
    return HMM_MulM4(ry, rx);
}

HMM_Vec3 mul_dir(const HMM_Mat4& m, HMM_Vec3 v) {
    const HMM_Vec4 r = HMM_MulM4V4(m, HMM_V4(v.X, v.Y, v.Z, 0.0f));
    return HMM_V3(r.X, r.Y, r.Z);
}

} // namespace

// ---- input -----------------------------------------------------------------

void Camera::apply_mouse_delta(float dx, float dy) {
    yaw   -= dx * mouse_sensitivity;
    pitch -= dy * mouse_sensitivity;

    // Keep pitch just shy of straight up/down to avoid view flip.
    constexpr float kLimit = 1.55334f;  // ~89°
    pitch = std::clamp(pitch, -kLimit, kLimit);
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

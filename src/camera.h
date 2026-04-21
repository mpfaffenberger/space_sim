#pragma once
// -----------------------------------------------------------------------------
// camera.h — 6-DOF flying camera.
//
// Orientation stored as yaw/pitch for now (matches the mouselook we built in
// Stage 2; roll can join as a third angle when we decide we want it). Position
// and velocity live here because "the camera IS the ship" for this stage —
// when we add a proper Ship struct later, Camera will become a chase-cam
// that follows the ship's transform.
//
// Physics model: simple Newton with exponential damping. Thrust accelerates
// along camera-local axes; damping decays velocity when no input. This is the
// "assisted flight" feel of Privateer / Elite-with-FA-on. Full inertial
// (Newton with no damp) is just a matter of setting linear_damping = 0.
// -----------------------------------------------------------------------------

#include "HandmadeMath.h"

struct Camera {
    // ---- Orientation (radians) -----------------------------------------
    float yaw   = 0.0f;   // around +Y
    float pitch = 0.0f;   // around +X

    // ---- World-space state ---------------------------------------------
    HMM_Vec3 position{0.0f, 0.0f, 30000.0f};   // ~30 km out from the origin sun
    HMM_Vec3 velocity{0.0f, 0.0f, 0.0f};

    // ---- Projection ----------------------------------------------------
    // Near/far ratio here is tuned for "you can see the sun from 100k out,
    // a ship at arm's length is still crisp." Log-depth or reverse-Z will
    // be the next upgrade when z-fighting bites us — for now, this works.
    float fov_y_radians = HMM_DegToRad * 65.0f;
    float near_plane    = 1.0f;
    float far_plane     = 500000.0f;

    // ---- Tuning --------------------------------------------------------
    float mouse_sensitivity = 0.0025f;
    float thrust_accel      = 80.0f;    // units / s^2, normal flight
    float linear_damping    = 0.5f;     // terminal v ≈ accel / damping

    // ---- Cruise engine (Freelancer-style high-speed traversal) ---------
    // Hold TAB in main to drive `cruise_target` up; integrate() lerps
    // cruise_level toward it, so engage/disengage takes ~1 second and
    // feels like an engine winding up instead of a binary speed change.
    //
    // At cruise_level == 1.0 we multiply thrust by cruise_thrust_mult and
    // DIVIDE damping by cruise_damp_div — both raise the terminal speed,
    // but changing damping also makes the ship feel coastier, which sells
    // "I engaged the afterburner."
    float cruise_target       = 0.0f;   // 0..1, driven by input (toggle)
    float cruise_level        = 0.0f;   // 0..1, smoothed state
    float cruise_lerp_rate    = 3.0f;   // 1 / time-constant (s^-1)
    float cruise_thrust_mult  = 10.0f;  // at level=1
    float cruise_damp_div     = 2.5f;   // at level=1, damping /= this
    float cruise_fov_extra    = HMM_DegToRad * 15.0f;  // widen FOV when cruising

    // ---- Input ---------------------------------------------------------
    void apply_mouse_delta(float dx, float dy);

    // Apply a thrust vector *in camera-local space* for `dt` seconds.
    //   local_dir.x = strafe right
    //   local_dir.y = thrust up
    //   local_dir.z = thrust forward (negative = backwards along view)
    // Internally multiplies by the current cruise_level, so callers don't
    // need to know about cruise mode.
    void apply_thrust(HMM_Vec3 local_dir, float dt);

    // Integrate velocity into position and apply damping.
    void integrate(float dt);

    // Immediate full stop — "X to kill velocity" brake.
    void brake();

    // ---- Queries -------------------------------------------------------
    // World-space basis vectors derived from yaw/pitch. Useful for turning
    // local-space input into world-space thrust, and for positioning HUD
    // elements relative to view.
    HMM_Vec3 forward() const;   // where the camera looks (negative-Z in view space)
    HMM_Vec3 right()   const;
    HMM_Vec3 up()      const;

    // Full view matrix (rotation + translation).
    HMM_Mat4 view() const;

    // View matrix with translation zeroed — for the skybox pass, so the sky
    // never appears to move.
    HMM_Mat4 view_rotation_only() const;

    HMM_Mat4 projection(float aspect) const;

    // Effective FOV (radians) including cruise widening.
    float effective_fov() const;
};

#pragma once
// -----------------------------------------------------------------------------
// camera.h — 6-DOF flying camera.
//
// Orientation is a unit quaternion. Earlier (Stage 2) this was Euler
// yaw/pitch, which gimbal-locked at vertical: try to look straight up
// in fly-by-wire and yaw becomes degenerate (rotates around world-up
// instead of ship-up, ship 'sticks' at the pole). Quaternion + local-
// frame composition (orientation = orientation * delta_q) sidesteps the
// problem entirely — full 360° pitch, no poles, free 6DOF.
//
// Trade-off: the ship can accumulate implicit roll if you combine
// pitch+yaw in non-cardinal patterns (Newtonian-spaceship feel,
// Elite/Descent style). For Freelancer's auto-level horizon we'd later
// add a slow 'level toward world-up' correction term in integrate().
// Position and velocity live here because "the camera IS the ship" for
// this stage — when we add a proper Ship struct later, Camera will
// become a chase-cam that follows the ship's transform.
//
// Physics model: simple Newton with exponential damping. Thrust accelerates
// along camera-local axes; damping decays velocity when no input. This is the
// "assisted flight" feel of Privateer / Elite-with-FA-on. Full inertial
// (Newton with no damp) is just a matter of setting linear_damping = 0.
// -----------------------------------------------------------------------------

#include "HandmadeMath.h"

struct Camera {
    // ---- Orientation ---------------------------------------------------
    // Unit quaternion. Identity = looking down -Z, +Y up (engine default).
    // All apply_* methods compose deltas in *local frame* via
    //   orientation = orientation * delta_q
    // which is gimbal-lock-immune by construction.
    HMM_Quat orientation = HMM_Q(0.0f, 0.0f, 0.0f, 1.0f);

    // ---- World-space state ---------------------------------------------
    HMM_Vec3 position{0.0f, 0.0f, 30000.0f};   // ~30 km out from the origin sun
    HMM_Vec3 velocity{0.0f, 0.0f, 0.0f};

    // ---- Projection ----------------------------------------------------
    // Near/far ratio here is tuned for "you can see the sun from 100k out,
    // a ship at arm's length is still crisp." Log-depth or reverse-Z will
    // be the next upgrade when z-fighting bites us — for now, this works.
    // Vertical FOV. Picked to give ~60° horizontal at 16:10 (aspect 1.6),
    // which is the sweet spot between fish-bowl-wide (>75°) and zoomed-
    // telephoto (<50°): no edge stretch, but enough peripheral vision
    // that flying doesn't feel like staring through a paper-towel tube.
    // Conversion: hfov = 2·atan(aspect · tan(vfov/2)). For 16:9 monitors
    // (1.78) the horizontal works out to ~67°, still comfortably natural.
    float fov_y_radians = HMM_DegToRad * 40.0f;
    float near_plane    = 1.0f;
    float far_plane     = 500000.0f;

    // ---- Tuning --------------------------------------------------------
    float mouse_sensitivity = 0.0025f;

// Fly-by-wire turn rates (rad/s at full ±1 offset). Tuned for a
// Freelancer-snappy feel: ~80°/s peak yaw, ~70°/s peak pitch.
// Pitch slightly slower than yaw to discourage barrel-rolly chaos.
float max_yaw_rate    = 1.4f;
float max_pitch_rate  = 1.2f;
// Roll is keyboard-driven (Q/E in main), not mouse-driven, so it
// doesn't fight the aim-fly-by-wire vector. Peak ~115°/s — fast
// enough to flip the horizon for a strafe-aim, slow enough that a
// quick tap doesn't spin you out of orientation.
float max_roll_rate   = 2.0f;

// Symmetric dead-zone around screen centre, as a fraction of the
// half-screen radius. 0.05 = ignore the inner 5 % so the player
// can fly straight without micrometre-perfect cursor placement.
float mouse_dead_zone = 0.05f;
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
    // Cruise FOV pump scaled to the new base. ~25 % widen at full cruise
    // — same proportion as the previous 65→80 mapping (40→50). Preserves
    // the 'engine wound up' speed cue without re-introducing fish-bowl.
    float cruise_fov_extra    = HMM_DegToRad * 10.0f;

    // ---- Input ---------------------------------------------------------
    void apply_mouse_delta(float dx, float dy);

// Fly-by-wire aim. (off_x, off_y) is the mouse position normalised to
// [-1, 1] from screen centre — 0 = neutral, ±1 = screen edge. Applies
// a soft dead-zone around centre, then linearly maps the remainder
// to angular velocity, then steps the camera by dt. Replaces FPS-
// style relative-mouse look for ship piloting (Freelancer feel:
// where the cursor sits, the nose chases).
void apply_mouse_aim(float off_x, float off_y, float dt);

// Compose a local-frame roll (around camera-local +Z, the view-axis)
// into the orientation. Sign convention matches the rest of the
// quaternion path: positive `rate_sign` (e.g. +1 from E) rolls the
// world clockwise as seen by the pilot — the same way an aircraft
// stick-right roll feels.
void apply_roll(float rate_sign, float dt);

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

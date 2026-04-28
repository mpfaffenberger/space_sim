#pragma once
// -----------------------------------------------------------------------------
// world_scale.h — global pacing knobs.
//
// A single constant tuned at the integrator layer so changing it slows
// the game uniformly across the camera, NPC ships, and projectiles —
// without touching AI tick rates, gun cooldowns, or shield regen
// (those use raw dt and stay snappy at the player-perception level).
//
// k_world_velocity_scale: multiplier on linear velocity at integration
// time. 1.0 = canonical; 0.5 halves how far everything moves per second
// while leaving turn rates, firing rhythm, and decision cadence at
// full real-time speed. Tune here once, every system picks it up.
//
// Why a header constant instead of a runtime knob: keeps the math
// inlinable by the optimizer and makes it impossible to forget to apply
// in any one integrator. Promote to runtime when the game settings UI
// wants a player-facing speed slider.
// -----------------------------------------------------------------------------

namespace world_scale {

// Linear-velocity scale — ships, projectiles, and the camera all
// move through space at this fraction of their canonical speed.
// Back to 1.0 (full speed) — the slower-pacing experiments helped
// validate the visual feedback layers (shield flashes, armor sparks,
// hit vignette), and at full speed the new effects still read
// clearly. Drop back to 0.5-0.6 if a future scenario wants the
// deliberate "time to aim" feel.
constexpr float k_world_velocity_scale = 1.0f;

// Shield-regen slowdown — separate from the velocity knob because
// regen plays a different role (combat attrition pacing, not motion
// feel). 0.0167 ≈ 1/60: brings the player's Shield Generator 1 from a
// canonical 4 cm/s to ~0.067 cm/s = 1 cm every 15 s, which gives
// damage time to actually accumulate between attack runs. Other
// shield types scale proportionally, preserving the catalogue's
// Light/Medium/Heavy/Capital regen relationships.
constexpr float k_shield_regen_scale = 0.0167f;

// Ship-size multiplier — scales every ship's rendered length AND its
// collision hit-radius (which is derived from the rendered size).
// 1.4 makes ships 40% larger — easier to hit at the demo's combat
// distances without the AI's lead-prediction breaking down. Visual
// proportion vs the system (planets, mining bases) shifts only
// slightly because those are sized in tens of km already.
constexpr float k_ship_size_scale = 1.4f;

}

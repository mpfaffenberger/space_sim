#pragma once
// -----------------------------------------------------------------------------
// mobility.h — Privateer's "Acceleration / Max YPR" descriptive tiers.
//
// The original game describes ship agility with five English-language
// labels rather than numbers: Poor / Average / Good / Very Good /
// Excellent. We translate to numeric multipliers off a per-axis base
// rate. The same multiplier applies to:
//
//   * acceleration (m/s²)
//   * max yaw rate   (deg/sec, body Y)
//   * max pitch rate (deg/sec, body X)
//   * max roll rate  (deg/sec, body Z)
//
// Base rates were picked so a "Good" ship feels like the WC1-era Hornet
// (~60 deg/sec yaw); tune in one place if the demo feels too snappy or
// too sluggish, every ship rescales coherently.
// -----------------------------------------------------------------------------

#include <cstdint>
#include <string_view>

enum class MobilityTier : uint8_t {
    Poor      = 0,
    Average   = 1,
    Good      = 2,
    VeryGood  = 3,
    Excellent = 4,
    Count
};

constexpr float k_mobility_mult[5] = {
    0.50f,  // Poor
    0.75f,  // Average
    1.00f,  // Good
    1.25f,  // VeryGood
    1.50f,  // Excellent
};

// Base rates — every ship's actual values are these times the tier multiplier.
constexpr float k_mobility_base_yaw_deg_s   = 60.0f;
constexpr float k_mobility_base_pitch_deg_s = 50.0f;
constexpr float k_mobility_base_roll_deg_s  = 90.0f;
constexpr float k_mobility_base_accel_mps2  = 80.0f;

namespace mobility {

// "poor" / "average" / "good" / "very good" / "very_good" / "excellent",
// case-insensitive. Returns MobilityTier::Count on unknown input.
MobilityTier from_name(std::string_view s);
const char*  to_name(MobilityTier t);

inline float mult(MobilityTier t) {
    if ((int)t < 0 || (int)t >= 5) return 1.0f;
    return k_mobility_mult[(int)t];
}

inline float yaw_rate_deg(MobilityTier t)   { return k_mobility_base_yaw_deg_s   * mult(t); }
inline float pitch_rate_deg(MobilityTier t) { return k_mobility_base_pitch_deg_s * mult(t); }
inline float roll_rate_deg(MobilityTier t)  { return k_mobility_base_roll_deg_s  * mult(t); }
inline float accel_mps2(MobilityTier t)     { return k_mobility_base_accel_mps2  * mult(t); }

} // namespace mobility

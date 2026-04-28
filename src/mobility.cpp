#include "mobility.h"

#include <cctype>
#include <string>

namespace {
std::string lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = (char)std::tolower((unsigned char)c);
    return out;
}
} // namespace

MobilityTier mobility::from_name(std::string_view s) {
    const std::string n = lower(s);
    if (n == "poor")        return MobilityTier::Poor;
    if (n == "average")     return MobilityTier::Average;
    if (n == "good")        return MobilityTier::Good;
    // Accept both "very good" (the JSON spelling Privateer uses) and
    // "very_good" (the spelling C++ identifiers prefer if anyone codes it).
    if (n == "very good" || n == "very_good") return MobilityTier::VeryGood;
    if (n == "excellent")   return MobilityTier::Excellent;
    return MobilityTier::Count;
}

const char* mobility::to_name(MobilityTier t) {
    switch (t) {
        case MobilityTier::Poor:      return "poor";
        case MobilityTier::Average:   return "average";
        case MobilityTier::Good:      return "good";
        case MobilityTier::VeryGood:  return "very good";
        case MobilityTier::Excellent: return "excellent";
        default:                       return "?";
    }
}

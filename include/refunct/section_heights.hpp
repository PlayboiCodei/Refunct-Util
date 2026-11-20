#pragma once
// Refunct v9.30: preset heights captured from 31 native BP_LevelRoot_C actors.
// Pure C++ geometry validation; no Windows APIs, disk cache, or calibration.
#include <cstddef>

namespace refunct::section_heights {
constexpr std::size_t kSectionCount = 31;
constexpr float kSourceZTolerance = 4.0f;
constexpr float kRaisedDetectionTolerance = 8.0f;
struct Preset {
    float expected_source_z;
    float raised_z;
    float lowered_z;
};

// Section 1's lowered height is inferred from SourcePosition and has not yet
// been directly verified in game. Sections 2-31 were measured at both ends.
const Preset& At(std::size_t index);
// Reject unexpected map geometry instead of moving actors to unsafe heights.
bool Target(std::size_t index, bool raised, float actual_source_z, float& out_z);
// True means the measurement is valid; `raised` is false for fully lowered
// or in-transition sections. Unknown/mismatched geometry is not treated as up.
bool IsPhysicallyRaised(std::size_t index, float actual_source_z,
                        float current_z, bool& raised);
} // namespace refunct::section_heights

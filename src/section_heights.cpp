#include "refunct/section_heights.hpp"
#include <array>
#include <cmath>

namespace refunct::section_heights {
namespace {
// Captured from section-calibration.txt on 2026-11-12. Each native raised
// level was at world Z=0; lowered levels match each actor's SourcePosition.Z.
// Do not generalize this table to another Refunct build or custom level.
constexpr std::array<Preset, kSectionCount> kPresets = {{
    Preset{-2499.0f, 0.0f, -2499.0f},
    Preset{-1000.0f, 0.0f, -1000.0f},
    Preset{-1250.0f, 0.0f, -1250.0f},
    Preset{-1250.0f, 0.0f, -1250.0f},
    Preset{-1000.0f, 0.0f, -1000.0f},
    Preset{-2250.0f, 0.0f, -2250.0f},
    Preset{-875.0f, 0.0f, -875.0f},
    Preset{-2375.0f, 0.0f, -2375.0f},
    Preset{-1875.0f, 0.0f, -1875.0f},
    Preset{-1875.0f, 0.0f, -1875.0f},
    Preset{-375.0f, 0.0f, -375.0f},
    Preset{-1125.0f, 0.0f, -1125.0f},
    Preset{-1125.0f, 0.0f, -1125.0f},
    Preset{-1125.0f, 0.0f, -1125.0f},
    Preset{-1000.0f, 0.0f, -1000.0f},
    Preset{-1000.0f, 0.0f, -1000.0f},
    Preset{-1250.0f, 0.0f, -1250.0f},
    Preset{-1750.0f, 0.0f, -1750.0f},
    Preset{-1500.0f, 0.0f, -1500.0f},
    Preset{-1250.0f, 0.0f, -1250.0f},
    Preset{-1125.0f, 0.0f, -1125.0f},
    Preset{-375.0f, 0.0f, -375.0f},
    Preset{-1250.0f, 0.0f, -1250.0f},
    Preset{-1500.0f, 0.0f, -1500.0f},
    Preset{-1125.0f, 0.0f, -1125.0f},
    Preset{-375.0f, 0.0f, -375.0f},
    Preset{-1125.0f, 0.0f, -1125.0f},
    Preset{-1500.0f, 0.0f, -1500.0f},
    Preset{-2000.0f, 0.0f, -2000.0f},
    Preset{-1125.0f, 0.0f, -1125.0f},
    Preset{-1500.0f, 0.0f, -1500.0f}
}};
static_assert(kPresets.size() == kSectionCount);
} // namespace

const Preset& At(std::size_t index) {
    return kPresets.at(index);
}

bool Target(std::size_t index, bool raised, float actual_source_z, float& out_z) {
    if (index >= kSectionCount || !std::isfinite(actual_source_z)) return false;
    const Preset& preset = kPresets[index];
    if (std::abs(actual_source_z - preset.expected_source_z) > kSourceZTolerance)
        return false;
    // Guard against corrupted or accidentally reordered preset data.
    const float lift = preset.raised_z - preset.lowered_z;
    if (!std::isfinite(lift) || lift < 100.0f || lift > 100000.0f)
        return false;
    const float target = raised ? preset.raised_z : preset.lowered_z;
    if (!std::isfinite(target)) return false;
    out_z = target;
    return true;
}
bool IsPhysicallyRaised(std::size_t index, float actual_source_z,
                        float current_z, bool& raised) {
    raised = false;
    float raised_z = 0.0f, lowered_z = 0.0f;
    if (!std::isfinite(current_z) ||
        !Target(index, true, actual_source_z, raised_z) ||
        !Target(index, false, actual_source_z, lowered_z)) return false;
    // Don't enable a button merely because an animation is headed upward.
    raised = std::abs(current_z - raised_z) <= kRaisedDetectionTolerance;
    return true;
}

} // namespace refunct::section_heights

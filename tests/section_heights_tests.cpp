#include "refunct/section_heights.hpp"
#include "refunct/button_sync_policy.hpp"
#include <cassert>
#include <limits>
using namespace refunct::section_heights;
int main() {
    static_assert(kSectionCount == 31);
    for (std::size_t i = 0; i < kSectionCount; ++i) {
        const auto& preset = At(i);
        assert(preset.raised_z == 0.0f);
        assert(preset.lowered_z == preset.expected_source_z);
        float actual = 9.0f;
        assert(Target(i, true, preset.expected_source_z, actual));
        assert(actual == preset.raised_z);
        assert(Target(i, false, preset.expected_source_z, actual));
        assert(actual == preset.lowered_z);
        assert(Target(i, true, preset.expected_source_z + kSourceZTolerance, actual));
        assert(!Target(i, false, preset.expected_source_z + kSourceZTolerance + 0.5f, actual));
        assert(!Target(i, true, std::numeric_limits<float>::quiet_NaN(), actual));
        bool up = false;
        assert(IsPhysicallyRaised(i, preset.expected_source_z, preset.raised_z, up) && up);
        assert(IsPhysicallyRaised(i, preset.expected_source_z,
                                  preset.raised_z - kRaisedDetectionTolerance, up) && up);
        assert(IsPhysicallyRaised(i, preset.expected_source_z,
                                  preset.raised_z - kRaisedDetectionTolerance - 0.5f, up) && !up);
        assert(IsPhysicallyRaised(i, preset.expected_source_z, preset.lowered_z, up) && !up);
        assert(!IsPhysicallyRaised(i, preset.expected_source_z + 10.0f,
                                   preset.raised_z, up) && !up);
        assert(!IsPhysicallyRaised(i, preset.expected_source_z,
                                   std::numeric_limits<float>::quiet_NaN(), up) && !up);
    }
    float target = 0.0f;
    assert(At(0).expected_source_z == -2499.0f); // provisional native source for section 1
    assert(At(28).expected_source_z == -2000.0f);
    assert(!Target(31, true, -1500.0f, target));
    assert(!Target(0, false, At(1).expected_source_z, target)); // changed map

    // Native observations: completed buttons stay pressed; every unfinished
    // button on a fully raised section is unlocked. Lowered buttons stay locked.
    using refunct::button_sync_policy::DesiredIsPressed;
    using refunct::button_sync_policy::VanillaIsPressed;
    for (int finished = 0; finished <= 37; ++finished) {
        for (int i = 0; i < 37; ++i) {
            assert(DesiredIsPressed(i, finished, false));
            assert(DesiredIsPressed(i, finished, true) == (i < finished));
            assert(VanillaIsPressed(i, finished) == (i != finished));
        }
    }
    static_assert(DesiredIsPressed(2, 0, false));
    static_assert(!DesiredIsPressed(2, 0, true));
    static_assert(DesiredIsPressed(0, 1, true));
}

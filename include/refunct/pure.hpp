#pragma once
// Portable, deterministic logic; no Windows SDK, MinHook, or game memory required.
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace refunct {
// Independent cheat-record permissions. Default Off preserves vanilla records.
// Bits are purpose-specific: PB = 1, fastest segments = 2.
enum class CheatRecordMode : std::uint8_t {
    Off = 0, PBOnly = 1, SegmentsOnly = 2, Both = 3
};

[[nodiscard]] constexpr bool CheatRecordsPB(CheatRecordMode mode) noexcept {
    return (static_cast<unsigned>(mode) & 1u) != 0;
}
[[nodiscard]] constexpr bool CheatRecordsSegments(CheatRecordMode mode) noexcept {
    return (static_cast<unsigned>(mode) & 2u) != 0;
}
[[nodiscard]] constexpr CheatRecordMode NextCheatRecordMode(
    CheatRecordMode mode, int direction) noexcept {
    return static_cast<CheatRecordMode>(
        (static_cast<int>(mode) + (direction < 0 ? 3 : 1)) % 4);
}

// Three independent choices for the two time rows in the split popup.
// Stored as a validated 0..2 integer in settings.ini.
enum class SplitRowMode : std::uint8_t { HeaderAndTime = 0, TimeOnly = 1, Off = 2 };

[[nodiscard]] constexpr SplitRowMode CycleSplitRowMode(SplitRowMode mode, int direction) noexcept {
    // direction is -1 for Left and +1 for Right. Wrap at either end.
    const int index = static_cast<int>(mode);
    return static_cast<SplitRowMode>((index + (direction < 0 ? 2 : 1)) % 3);
}

[[nodiscard]] bool ValidProfileName(const std::wstring& name);
[[nodiscard]] std::wstring FormatSplitTime(double seconds);

// A comparison is always against the same button in a DIFFERENT profile.
// PB times are cumulative. Best segments must be summed from button 1
// through the requested button; if any segment is missing, no reference exists.
enum class ProfileCompareMode : std::uint8_t { PersonalBest = 0, BestSegments = 1 };
// How a recorded time should be shown on a persistent HUD.
// Total = cumulative time through the selected button; Segment = this button only.
enum class RecordTimeView : std::uint8_t { Total = 0, Segment = 1 };
[[nodiscard]] constexpr RecordTimeView CycleRecordTimeView(RecordTimeView mode) noexcept {
    return mode == RecordTimeView::Total ? RecordTimeView::Segment : RecordTimeView::Total;
}
constexpr size_t kComparisonButtonCount = 37;
[[nodiscard]] double ProfileReferenceTimeAtButton(
    const std::array<double, kComparisonButtonCount>& pb_splits,
    const std::array<double, kComparisonButtonCount>& best_segments,
    int button, ProfileCompareMode mode) noexcept;
// Returns -1 when necessary source splits are missing or invalid.
[[nodiscard]] double ProfileRecordedTimeAtButton(
    const std::array<double, kComparisonButtonCount>& pb_splits,
    const std::array<double, kComparisonButtonCount>& best_segments,
    int button, ProfileCompareMode source, RecordTimeView view) noexcept;

// Comparison sign uses the displayed millisecond precision, so a displayed
// zero is neutral (never a red +00:00.000 or green -00:00.000).
enum class DeltaDirection : std::uint8_t { Ahead, Behind, Even, Invalid };
struct SplitDeltaDisplay {
    std::wstring text;
    DeltaDirection direction = DeltaDirection::Invalid;
};
[[nodiscard]] SplitDeltaDisplay FormatSplitDelta(double seconds, bool include_time);
[[nodiscard]] bool ParsePBTime(std::wstring value, double& out);
[[nodiscard]] std::wstring PBKey(int button);
} // namespace refunct

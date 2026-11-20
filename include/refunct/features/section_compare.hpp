#pragma once
// Refunct v9.30 original map: 31 sections, 37 native button events.
// Multi-button sections complete only at their final button.
// This header is portable: no engine calls and no Windows SDK dependencies.
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace refunct {

enum class SectionCompareDisplayMode : std::uint8_t {
    HeaderAndTime = 0, TimeOnly = 1, HeaderAndSign = 2,
    SignOnly = 3, Off = 4
};

[[nodiscard]] constexpr SectionCompareDisplayMode CycleSectionCompareDisplayMode(
    SectionCompareDisplayMode mode, int direction) noexcept {
    return static_cast<SectionCompareDisplayMode>(
        (static_cast<int>(mode) + (direction < 0 ? 4 : 1)) % 5);
}

[[nodiscard]] constexpr bool SectionCompareHasHeader(SectionCompareDisplayMode mode) noexcept {
    return mode == SectionCompareDisplayMode::HeaderAndTime ||
           mode == SectionCompareDisplayMode::HeaderAndSign;
}

[[nodiscard]] constexpr bool SectionCompareHasTime(SectionCompareDisplayMode mode) noexcept {
    return mode == SectionCompareDisplayMode::HeaderAndTime ||
           mode == SectionCompareDisplayMode::TimeOnly;
}

struct SectionBoundary {
    int section = 0;             // One-based; zero means not a section endpoint.
    int previous_end_button = 0; // Zero means run start.
    int end_button = 0;
};

// End button indices are checked against kButtonNames in state.hpp.
// Sections 7, 10, 18, 26 and 28 have multiple button events.
inline constexpr std::array<int, 31> kSectionEndButtons{{
    1, 2, 3, 4, 5, 6, 8, 9, 10, 12, 13, 14, 15, 16, 17, 18,
    19, 21, 22, 23, 24, 25, 26, 27, 28, 31, 32, 34, 35, 36, 37
}};

[[nodiscard]] constexpr bool ValidSectionEnds() noexcept {
    int previous = 0;
    for (int value : kSectionEndButtons) {
        if (value <= previous || value > 37) return false;
        previous = value;
    }
    return previous == 37;
}
static_assert(ValidSectionEnds());

[[nodiscard]] constexpr SectionBoundary SectionEndingAtButton(int button) noexcept {
    for (int i = 0; i < static_cast<int>(kSectionEndButtons.size()); ++i) {
        if (kSectionEndButtons[static_cast<size_t>(i)] != button) continue;
        return {i + 1, i == 0 ? 0 : kSectionEndButtons[static_cast<size_t>(i - 1)], button};
    }
    return {};
}

// A label such as 18.1 is an intermediate button in section 18, not a
// section endpoint. The normal completed-section lookup intentionally skips it.
[[nodiscard]] constexpr SectionBoundary SectionContainingButton(int button) noexcept {
    if (button < 1 || button > 37) return {};
    int previous_end = 0;
    for (int i = 0; i < static_cast<int>(kSectionEndButtons.size()); ++i) {
        const int end = kSectionEndButtons[static_cast<size_t>(i)];
        if (button <= end) return {i + 1, previous_end, end};
        previous_end = end;
    }
    return {};
}

[[nodiscard]] constexpr bool IsIntermediateSectionButton(int button) noexcept {
    const auto section = SectionContainingButton(button);
    return section.section != 0 && button < section.end_button;
}

// Calculations shown in the HUD must use the same milliseconds as saved PB
// splits and FormatSplitTime(). Subtracting unrounded timestamps, then rounding
// the result, can differ by 1 ms from subtracting the two displayed times.
[[nodiscard]] inline double DisplayedDurationSeconds(
    double start_seconds, double end_seconds) noexcept {
    if (!std::isfinite(start_seconds) || !std::isfinite(end_seconds) ||
        start_seconds < 0.0 || end_seconds < 0.0 ||
        start_seconds > 86400.0 || end_seconds > 86400.0) return -1.0;
    const auto start_ms = std::llround(start_seconds * 1000.0);
    const auto end_ms = std::llround(end_seconds * 1000.0);
    return end_ms > start_ms
        ? static_cast<double>(end_ms - start_ms) / 1000.0 : -1.0;
}

// A signed delta between two cumulative/displayed times, rounded at the
// individual timestamp boundaries (not after their subtraction).
[[nodiscard]] inline double DisplayedTimestampDeltaSeconds(
    double current_seconds, double reference_seconds) noexcept {
    if (!std::isfinite(current_seconds) || !std::isfinite(reference_seconds) ||
        current_seconds < 0.0 || reference_seconds < 0.0 ||
        current_seconds > 86400.0 || reference_seconds > 86400.0) return std::numeric_limits<double>::quiet_NaN();
    const auto current_ms = std::llround(current_seconds * 1000.0);
    const auto reference_ms = std::llround(reference_seconds * 1000.0);
    return static_cast<double>(current_ms - reference_ms) / 1000.0;
}

// Change in cumulative PB lead/deficit across one real button split.
// Negative = gained time (ahead); positive = lost time (behind).
// Invalid/skipped/restarted button transitions have no meaningful result.
[[nodiscard]] inline double PBLeadChangeSeconds(
    double previous_run_seconds, double current_run_seconds,
    double previous_pb_seconds, double current_pb_seconds) noexcept {
    const double run_duration = DisplayedDurationSeconds(
        previous_run_seconds, current_run_seconds);
    const double pb_duration = DisplayedDurationSeconds(
        previous_pb_seconds, current_pb_seconds);
    if (run_duration < 0.0 || pb_duration < 0.0)
        return std::numeric_limits<double>::quiet_NaN();
    const double current_lead = DisplayedTimestampDeltaSeconds(
        current_run_seconds, current_pb_seconds);
    const double previous_lead = DisplayedTimestampDeltaSeconds(
        previous_run_seconds, previous_pb_seconds);
    if (!std::isfinite(current_lead) || !std::isfinite(previous_lead))
        return std::numeric_limits<double>::quiet_NaN();
    // Do this arithmetic as integer milliseconds, like the displayed timer.
    const auto now_ms = std::llround(current_lead * 1000.0);
    const auto before_ms = std::llround(previous_lead * 1000.0);
    return static_cast<double>(now_ms - before_ms) / 1000.0;
}

// The PB time of ONE button, not an entire multi-button section:
// e.g. 18.1 (button 20) = PB[20] - PB[19], using cumulative timestamps.
[[nodiscard]] inline double PBButtonSegmentDuration(
    const std::array<double, 37>& cumulative_seconds, int button) noexcept {
    if (button < 1 || button > 37) return -1.0;
    const double end = cumulative_seconds[static_cast<size_t>(button - 1)];
    const double start = button == 1 ? 0.0 :
        cumulative_seconds[static_cast<size_t>(button - 2)];
    return DisplayedDurationSeconds(start, end);
}

// Section Practice can begin with the first button already pressed by the
// loader. Compare only the part played after its Ready anchor. For later
// sections, use the full section boundaries again.
[[nodiscard]] constexpr SectionBoundary PracticeSectionComparisonBoundary(
    SectionBoundary section, int anchor_button) noexcept {
    if (section.section == 0 || anchor_button < 0 ||
        anchor_button >= section.end_button) return {};
    return {section.section,
            anchor_button > section.previous_end_button
                ? anchor_button : section.previous_end_button,
            section.end_button};
}

// A native button-contact hook can observe several real progress edges before
// the ordinary per-frame timer/split update runs. Verify the full sequence;
// missing edges are *not* assigned guessed timestamps.
struct NativeButtonObservation {
    double run_start = -1.0;
    double elapsed = -1.0;
    int previous_button = -1;
};

[[nodiscard]] inline bool CompleteNativeButtonSequence(
    const std::array<NativeButtonObservation, 37>& observations,
    int previous_button, int final_button, double start,
    double previous_elapsed, double final_elapsed) noexcept {
    if (previous_button < 0 || previous_button >= final_button ||
        final_button > 37 || !std::isfinite(start) ||
        !std::isfinite(previous_elapsed) || !std::isfinite(final_elapsed) ||
        previous_elapsed < 0.0 || final_elapsed < previous_elapsed) return false;
    double prior_time = previous_elapsed;
    for (int button = previous_button + 1; button <= final_button; ++button) {
        const auto& observed = observations[static_cast<size_t>(button - 1)];
        if (observed.previous_button != button - 1 ||
            !std::isfinite(observed.run_start) ||
            std::abs(observed.run_start - start) > 0.001 ||
            !std::isfinite(observed.elapsed) ||
            observed.elapsed < prior_time ||
            observed.elapsed > final_elapsed + 0.001) return false;
        prior_time = observed.elapsed;
    }
    return true;
}

// Historical records store cumulative button timestamps. Normal full-section
// comparisons subtract the previous section endpoint; practice may subtract
// its explicitly measured later start button instead.
[[nodiscard]] inline double SectionDurationFromPB(
    const std::array<double, 37>& cumulative_seconds,
    SectionBoundary boundary) noexcept {
    if (boundary.section < 1 || boundary.section > 31 ||
        boundary.end_button < 1 || boundary.end_button > 37 ||
        boundary.previous_end_button < 0 ||
        boundary.previous_end_button >= boundary.end_button ||
        SectionEndingAtButton(boundary.end_button).section != boundary.section) return -1.0;
    const double end = cumulative_seconds[static_cast<size_t>(boundary.end_button - 1)];
    const double start = boundary.previous_end_button == 0 ? 0.0 :
        cumulative_seconds[static_cast<size_t>(boundary.previous_end_button - 1)];
    return DisplayedDurationSeconds(start, end);
}


// Sum *individual* button durations for a section. These are NOT cumulative
// button times. Every duration must be recorded; missing entries aren't zero.
// For a loaded practice section, the boundary can start after its first button.
[[nodiscard]] inline double SectionDurationFromSegments(
    const std::array<double, 37>& segment_seconds,
    SectionBoundary boundary) noexcept {
    if (boundary.section < 1 || boundary.section > 31 ||
        boundary.end_button < 1 || boundary.end_button > 37 ||
        boundary.previous_end_button < 0 ||
        boundary.previous_end_button >= boundary.end_button ||
        SectionEndingAtButton(boundary.end_button).section != boundary.section)
        return -1.0;
    double total = 0.0;
    for (int button = boundary.previous_end_button + 1;
         button <= boundary.end_button; ++button) {
        const double duration = segment_seconds[static_cast<size_t>(button - 1)];
        // A recorded best segment cannot be zero or a missing/invalid time.
        // This agrees with ProfileReferenceTimeAtButton and gold saving.
        if (!std::isfinite(duration) || duration <= 0.0 || duration > 86400.0)
            return -1.0;
        total += duration;
    }
    return std::isfinite(total) && total <= 86400.0 ? total : -1.0;
}

// The canonical PB section duration comes from cumulative button boundaries,
// because those are also the values shown in PB Difference. Saved SectionSplits
// is an optional legacy fallback only when a boundary timestamp is missing.
// Partial Section Practice must NEVER use the full-section fallback.
[[nodiscard]] inline double SectionDurationFromRecordedPB(
    const std::array<double, 31>& section_seconds,
    const std::array<double, 37>& cumulative_button_seconds,
    SectionBoundary boundary) noexcept {
    if (boundary.section < 1 || boundary.section > 31 ||
        SectionEndingAtButton(boundary.end_button).section != boundary.section)
        return -1.0;
    // Never prefer the independently rounded/stale SectionSplits value over
    // two real PB cumulative endpoints, including when they differ by 1 ms.
    const double derived = SectionDurationFromPB(cumulative_button_seconds, boundary);
    if (derived >= 0.0) return derived;
    const auto full = SectionEndingAtButton(boundary.end_button);
    if (full.previous_end_button != boundary.previous_end_button) return -1.0;
    const double stored = section_seconds[static_cast<size_t>(boundary.section - 1)];
    return DisplayedDurationSeconds(0.0, stored);
}

} // namespace refunct

#include "refunct/pure.hpp"
#include "refunct/section_compare.hpp"
#include "refunct/jump_modes.hpp"
#include <cassert>
#include <cmath>
#include <limits>
#include <string>

using namespace refunct;

int main() {
    // Preserve the game's v9.30 split profile names.
    assert(ValidProfileName(L"Any%"));
    assert(ValidProfileName(L"100% (Glitchless)"));
    assert(ValidProfileName(L"No-Cubes_1"));
    assert(!ValidProfileName(L""));
    assert(!ValidProfileName(L" bad"));
    assert(!ValidProfileName(L"bad "));
    assert(!ValidProfileName(L"A/B"));
    assert(!ValidProfileName(L"A:B"));
    assert(!ValidProfileName(std::wstring(33, L'a')));
    assert(!ValidProfileName(L"con"));
    assert(!ValidProfileName(L"COM9"));
    assert(!ValidProfileName(L"LPT8"));

    assert(FormatSplitTime(0.0) == L"00:00.000");
    assert(FormatSplitTime(62.345) == L"01:02.345");
    assert(FormatSplitTime(60.0) == L"01:00.000");
    // All clocks, PB files and +/- comparisons must round milliseconds alike.
    assert(FormatSplitTime(60.0006) == L"01:00.001");
    assert(FormatSplitTime(60.0004) == L"01:00.000");
    assert(FormatSplitTime(59.9996) == L"01:00.000");
    assert(FormatSplitTime(0.0006) == L"00:00.001");
    assert(FormatSplitTime(-1.0) == L"--:--.---");
    assert(FormatSplitTime(std::numeric_limits<double>::infinity()) == L"--:--.---");

    double time = -1.0;
    assert(ParsePBTime(L"1:02.345", time) && std::abs(time - 62.345) < 1e-9);
    assert(ParsePBTime(L" 32.125 ", time) && std::abs(time - 32.125) < 1e-9);
    assert(ParsePBTime(L"00:00.000", time) && time == 0.0);
    assert(!ParsePBTime(L"", time));
    assert(!ParsePBTime(L"1:60.000", time));
    assert(!ParsePBTime(L"1:2:3", time));
    assert(!ParsePBTime(L"-2", time));
    assert(!ParsePBTime(L"1.5:02", time));
    assert(!ParsePBTime(L"nan", time));
    assert(!ParsePBTime(L"90001", time));
    // PB save -> reload preserves the value displayed to the user.
    assert(ParsePBTime(FormatSplitTime(18.1236), time) &&
           std::abs(time - 18.124) < 1e-9);

    const auto ahead = FormatSplitDelta(-1.25, true);
    assert(ahead.direction == DeltaDirection::Ahead && ahead.text == L"-00:01.250");
    const auto behind = FormatSplitDelta(1.25, true);
    assert(behind.direction == DeltaDirection::Behind && behind.text == L"+00:01.250");
    assert(FormatSplitDelta(-1.25, false).text == L"-");
    assert(FormatSplitDelta(1.25, false).text == L"+");
    assert(FormatSplitDelta(0.0, true).text == L"00:00.000");
    const auto neutral = FormatSplitDelta(0.0, false);
    assert(neutral.direction == DeltaDirection::Even && neutral.text == L"=");
    assert(FormatSplitDelta(-0.0003, true).direction == DeltaDirection::Even);
    assert(FormatSplitDelta(0.0003, false).text == L"=");
    assert(FormatSplitDelta(-0.0003, false).text == L"=");
    assert(FormatSplitDelta(0.0003, true).direction == DeltaDirection::Even);
    assert(FormatSplitDelta(-0.0007, true).text == L"-00:00.001");
    assert(FormatSplitDelta(std::numeric_limits<double>::infinity(), true).direction == DeltaDirection::Invalid);

    // Both split popup rows share this three-way selector.
    static_assert(CycleSplitRowMode(SplitRowMode::HeaderAndTime, +1) == SplitRowMode::TimeOnly);
    static_assert(CycleSplitRowMode(SplitRowMode::TimeOnly, +1) == SplitRowMode::Off);
    static_assert(CycleSplitRowMode(SplitRowMode::Off, +1) == SplitRowMode::HeaderAndTime);
    static_assert(CycleSplitRowMode(SplitRowMode::HeaderAndTime, -1) == SplitRowMode::Off);
    static_assert(CycleSplitRowMode(SplitRowMode::Off, -1) == SplitRowMode::TimeOnly);

    // Existing save toggles remain separate from cheat-record permissions.
    static_assert(!CheatRecordsPB(CheatRecordMode::Off));
    static_assert(!CheatRecordsSegments(CheatRecordMode::Off));
    static_assert(CheatRecordsPB(CheatRecordMode::PBOnly));
    static_assert(!CheatRecordsSegments(CheatRecordMode::PBOnly));
    static_assert(!CheatRecordsPB(CheatRecordMode::SegmentsOnly));
    static_assert(CheatRecordsSegments(CheatRecordMode::SegmentsOnly));
    static_assert(CheatRecordsPB(CheatRecordMode::Both));
    static_assert(CheatRecordsSegments(CheatRecordMode::Both));
    static_assert(NextCheatRecordMode(CheatRecordMode::Off, -1) == CheatRecordMode::Both);
    static_assert(NextCheatRecordMode(CheatRecordMode::Both, +1) == CheatRecordMode::Off);

    // Eight options: Off plus seven unique combinations of jump/climb.
    static_assert(kJumpSelectorModeCount == 8);
    static_assert(JumpSelectorMode(false, JumpTargets::GroundOnly) == 0);
    static_assert(JumpSelectorMode(true, JumpTargets::GroundOnly) == 1);
    static_assert(NextJumpSelectorMode(0, -1) == 7);
    static_assert(NextJumpSelectorMode(7, +1) == 0);
    static_assert(JumpAllowsGround(JumpTargets::GroundAndWall));
    static_assert(!JumpAllowsClimb(JumpTargets::GroundAndWall));
    static_assert(JumpAllowsClimb(JumpTargets::ClimbOnly));
    static_assert(!JumpAllowsWall(JumpTargets::ClimbOnly));
    static_assert(JumpAllowsWall(JumpTargets::WallAndClimb));
    static_assert(!JumpAllowsGround(JumpTargets::WallAndClimb));
    static_assert(JumpTargetMask(JumpTargets::GroundWallAndClimb) == 7);
    static_assert(JumpTargetsFromStoredValue(0) == JumpTargets::GroundOnly);
    static_assert(JumpTargetsFromStoredValue(1) == JumpTargets::GroundAndWall);
    static_assert(JumpTargetsFromStoredValue(2) == JumpTargets::WallOnly);
    static_assert(JumpTargetsFromStoredValue(6) == JumpTargets::GroundWallAndClimb);
    static_assert(JumpTargetsFromStoredValue(33) == JumpTargets::GroundOnly);

    // Real two-button section 7: both native edges are retained even when
    // Refunct advances its count from 6 to 8 during a single game tick.
    static_assert(SectionEndingAtButton(7).section == 0); // 7.1 isn't the end
    static_assert(SectionEndingAtButton(8).section == 7); // 7.2 ends section 7
    static_assert(SectionEndingAtButton(8).previous_end_button == 6);
    static_assert(SectionEndingAtButton(31).section == 26); // three-button section
    // Loaded button 7.1 is already pressed: compare just 7.1 -> 7.2.
    static_assert(PracticeSectionComparisonBoundary(SectionEndingAtButton(8), 7)
                      .previous_end_button == 7);
    // A later section uses its genuine previous endpoint, not load anchor 7.
    static_assert(PracticeSectionComparisonBoundary(SectionEndingAtButton(9), 7)
                      .previous_end_button == 8);
    static_assert(PracticeSectionComparisonBoundary(SectionEndingAtButton(8), 8)
                      .section == 0); // Loader itself isn't a timed finish.
    std::array<NativeButtonObservation, 37> observed{};
    observed[6] = {100.0, 12.010, 6};
    observed[7] = {100.0, 12.025, 7};
    assert(CompleteNativeButtonSequence(observed, 6, 8, 100.0, 12.0, 12.030));
    observed[6] = {}; // missing 7.1 may NOT be invented
    assert(!CompleteNativeButtonSequence(observed, 6, 8, 100.0, 12.0, 12.030));
    observed[6] = {99.0, 12.010, 6}; // stale event from a different run
    assert(!CompleteNativeButtonSequence(observed, 6, 8, 100.0, 12.0, 12.030));
    observed[6] = {100.0, 12.040, 6}; // out of order relative to tick
    assert(!CompleteNativeButtonSequence(observed, 6, 8, 100.0, 12.0, 12.030));
    observed[6] = {100.0, 12.010, 6};
    observed[7] = {100.0, 11.900, 7}; // time cannot go backwards
    assert(!CompleteNativeButtonSequence(observed, 6, 8, 100.0, 12.0, 12.030));
    std::array<double, 37> pb{};
    pb.fill(-1.0);
    pb[5] = 10.0; // section 6 end
    pb[7] = 12.0; // 7.2 end
    assert(std::abs(SectionDurationFromPB(pb, SectionEndingAtButton(8)) - 2.0) < 1e-9);
    pb[6] = 11.5; // PB for loaded button 7.1
    assert(std::abs(SectionDurationFromPB(pb,
        PracticeSectionComparisonBoundary(SectionEndingAtButton(8), 7)) - 0.5) < 1e-9);
    pb[5] = -1.0;
    assert(SectionDurationFromPB(pb, SectionEndingAtButton(8)) < 0.0);

    // New PB files persist durations from the actual completed PB run.
    // Section 7 spans buttons 7.1 and 7.2, so a missing cumulative
    // endpoint in an old profile can still be resolved by [SectionSplits].
    std::array<double, 31> pb_sections{};
    pb_sections.fill(-1.0);
    pb_sections[6] = 2.125;
    assert(std::abs(SectionDurationFromRecordedPB(
        pb_sections, pb, SectionEndingAtButton(8)) - 2.125) < 1e-9);
    // Practice after the loader has pressed 7.1 needs only the remaining time;
    // never use the complete section's 2.125 seconds to fill the gap.
    assert(std::abs(SectionDurationFromRecordedPB(pb_sections, pb,
        PracticeSectionComparisonBoundary(SectionEndingAtButton(8), 7)) - 0.5) < 1e-9);
    pb_sections[6] = -1.0;
    pb[5] = 10.0;
    assert(std::abs(SectionDurationFromRecordedPB(pb_sections, pb,
        SectionEndingAtButton(8)) - 2.0) < 1e-9); // legacy profile fallback
    pb[5] = -1.0;
    assert(SectionDurationFromRecordedPB(pb_sections, pb,
        SectionEndingAtButton(8)) < 0.0); // don't invent a PB

    // A manually edited/stale SectionSplits entry must not override two
    // valid cumulative PB endpoints. That would contradict PB Difference.
    pb[5] = 10.0;
    pb[7] = 12.0;
    pb_sections[6] = 9.0; // stale: the endpoints unambiguously say 2.000
    assert(std::abs(SectionDurationFromRecordedPB(
        pb_sections, pb, SectionEndingAtButton(8)) - 2.0) < 1e-9);
    pb_sections[6] = 0.0; // an impossible zero-length section is not a PB
    assert(std::abs(SectionDurationFromRecordedPB(
        pb_sections, pb, SectionEndingAtButton(8)) - 2.0) < 1e-9);
    // A separately rounded section can differ by 1 ms from the difference
    // of DISPLAYED cumulative endpoints. Endpoints are authoritative.
    pb_sections[6] = 2.001;
    assert(std::abs(SectionDurationFromRecordedPB(
        pb_sections, pb, SectionEndingAtButton(8)) - 2.000) < 1e-9);
    pb[5] = -1.0; // valid saved section time alone is sufficient
    pb_sections[6] = 2.125;
    assert(std::abs(SectionDurationFromRecordedPB(
        pb_sections, pb, SectionEndingAtButton(8)) - 2.125) < 1e-9);
    pb_sections[6] = 0.0;
    assert(SectionDurationFromRecordedPB(pb_sections, pb,
        SectionEndingAtButton(8)) < 0.0);

    // End and start timestamps are rounded independently for display.
    // Raw subtraction followed by rounding would produce 5.000, but the
    // visible endpoints 15.000 - 10.001 must produce 4.999.
    assert(std::abs(DisplayedDurationSeconds(10.00051, 15.00049) - 4.999) < 1e-9);
    assert(std::abs(DisplayedTimestampDeltaSeconds(15.00049, 13.00051) - 1.999) < 1e-9);
    assert(std::abs(DisplayedTimestampDeltaSeconds(10.00051, 9.00049) - 1.001) < 1e-9);
    std::array<double, 37> precise_pb{};
    precise_pb.fill(-1.0);
    precise_pb[5] = 9.00049; precise_pb[7] = 13.00051;
    assert(std::abs(SectionDurationFromPB(precise_pb, SectionEndingAtButton(8)) - 4.001) < 1e-9);
    std::array<double, 31> rounded_stale{};
    rounded_stale.fill(-1.0);
    rounded_stale[6] = 4.000; // stale, even though only 1 ms apart
    assert(std::abs(SectionDurationFromRecordedPB(
        rounded_stale, precise_pb, SectionEndingAtButton(8)) - 4.001) < 1e-9);
    const double delta_as_section =
        DisplayedDurationSeconds(10.00051, 15.00049) -
        SectionDurationFromRecordedPB(rounded_stale, precise_pb, SectionEndingAtButton(8));
    const double delta_as_cumulative =
        DisplayedTimestampDeltaSeconds(15.00049, 13.00051) -
        DisplayedTimestampDeltaSeconds(10.00051, 9.00049);
    assert(std::abs(delta_as_section - 0.998) < 1e-9);
    assert(std::abs(delta_as_section - delta_as_cumulative) < 1e-9);

    // Real segment arithmetic: current 2.500 - sum of older golds 2.000.
    // This fallback is NOT called 'PB', since golds can be from different runs.
    std::array<double, 37> golds{};
    golds.fill(-1.0);
    golds[6] = 1.100; // button 7.1
    golds[7] = 0.900; // button 7.2
    const double reference = SectionDurationFromSegments(golds, SectionEndingAtButton(8));
    assert(std::abs(reference - 2.000) < 1e-9);
    assert(std::abs((2.500 - reference) - 0.500) < 1e-9);
    golds[7] = -1.0;
    assert(SectionDurationFromSegments(golds, SectionEndingAtButton(8)) < 0.0);
    golds[7] = 0.0; // a missing/unrealistic gold is never a valid reference
    assert(SectionDurationFromSegments(golds, SectionEndingAtButton(8)) < 0.0);
    // Section 26 has 3 buttons; all three must contribute to the duration.
    golds[28] = 0.2; golds[29] = 0.3; golds[30] = 0.4;
    assert(std::abs(SectionDurationFromSegments(golds, SectionEndingAtButton(31)) - 0.9) < 1e-9);
    golds[29] = std::numeric_limits<double>::infinity();
    assert(SectionDurationFromSegments(golds, SectionEndingAtButton(31)) < 0.0);

    // Section 18 contains button 18.1 (ordinal 20) and 18.2 (ordinal 21).
    // 18.1 must display its own PB button-segment difference immediately.
    const auto section18 = SectionContainingButton(20);
    assert(section18.section == 18 && section18.previous_end_button == 19 &&
           section18.end_button == 21);
    assert(SectionEndingAtButton(20).section == 0);
    assert(IsIntermediateSectionButton(20));
    assert(!IsIntermediateSectionButton(21));
    assert(IsIntermediateSectionButton(7));
    assert(IsIntermediateSectionButton(11));
    assert(IsIntermediateSectionButton(29));
    assert(IsIntermediateSectionButton(30));
    assert(IsIntermediateSectionButton(33));
    std::array<double, 37> button_pb{};
    button_pb.fill(-1.0);
    button_pb[18] = 100.000;  // button 17 / last section endpoint
    button_pb[19] = 101.750;  // 18.1
    button_pb[20] = 102.900;  // 18.2
    const double pb181 = PBButtonSegmentDuration(button_pb, 20);
    assert(std::abs(pb181 - 1.750) < 1e-9);
    assert(std::abs((2.100 - pb181) - 0.350) < 1e-9);
    assert(std::abs(SectionDurationFromPB(button_pb, SectionEndingAtButton(21)) - 2.900) < 1e-9);
    button_pb[18] = -1.0;
    assert(PBButtonSegmentDuration(button_pb, 20) < 0.0);
    assert(PBButtonSegmentDuration(button_pb, 0) < 0.0);
    assert(PBButtonSegmentDuration(button_pb, 38) < 0.0);
    button_pb[18] = 100.0;
    button_pb[19] = 100.0; // simultaneous/missing time is not a real section split
    assert(PBButtonSegmentDuration(button_pb, 20) < 0.0);

    // Table-driven audit across all 37 buttons and 31 sections. Separate
    // cumulative PB splits from per-button gold durations, even when a
    // multi-button section spans several physical buttons.
    std::array<double, 37> all_pb{}, all_golds{};
    for (int i = 0; i < 37; ++i) {
        all_pb[static_cast<size_t>(i)] = (i + 1) * 1.25;
        all_golds[static_cast<size_t>(i)] = 0.75;
    }
    for (int button = 1; button <= 37; ++button) {
        const double pb_total = ProfileRecordedTimeAtButton(all_pb, all_golds,
            button, ProfileCompareMode::PersonalBest, RecordTimeView::Total);
        const double pb_segment = ProfileRecordedTimeAtButton(all_pb, all_golds,
            button, ProfileCompareMode::PersonalBest, RecordTimeView::Segment);
        const double best_total = ProfileRecordedTimeAtButton(all_pb, all_golds,
            button, ProfileCompareMode::BestSegments, RecordTimeView::Total);
        const double best_segment = ProfileRecordedTimeAtButton(all_pb, all_golds,
            button, ProfileCompareMode::BestSegments, RecordTimeView::Segment);
        assert(std::abs(pb_total - button * 1.25) < 1e-8);
        assert(std::abs(pb_segment - 1.25) < 1e-8);
        assert(std::abs(best_total - button * 0.75) < 1e-8);
        assert(std::abs(best_segment - 0.75) < 1e-8);
        // A run taking 1.50s/button is behind its PB by 0.25s/button.
        assert(std::abs((button * 1.50 - pb_total) - button * 0.25) < 1e-8);
    }
    // New per-button PB gain/loss is the change in cumulative deficit.
    // Ahead by 400ms -> ahead by 440ms means a 40ms gain (negative/green).
    assert(std::abs(PBLeadChangeSeconds(12.6, 14.16, 13.0, 14.6) + 0.040) < 1e-10);
    // Behind by 400ms -> behind by 800ms means a 400ms loss (positive/red).
    assert(std::abs(PBLeadChangeSeconds(12.4, 13.8, 12.0, 13.0) - 0.400) < 1e-10);
    // Timer and PB times are rounded at their own endpoint to the nearest ms.
    assert(std::abs(PBLeadChangeSeconds(1.23449, 2.67849,
                                        1.63451, 3.11851) + 0.040) < 1e-10);
    assert(std::isnan(PBLeadChangeSeconds(5.0, 4.0, 5.0, 6.0)));
    assert(std::isnan(PBLeadChangeSeconds(4.0, 5.0, -1.0, 6.0)));
    // Fundamental identity at every completed section boundary:
    // Section vs PB = (current cumulative - PB cumulative) at end
    //               - (current cumulative - PB cumulative) at start.
    // It must hold for single-, two-, and three-button sections alike.
    std::array<double, 37> live_cumulative{};
    std::array<double, 31> direct_sections{};
    direct_sections.fill(-1.0);
    for (int i = 0; i < 37; ++i)
        live_cumulative[static_cast<size_t>(i)] = (i + 1) * 1.63;
    for (int end_button : kSectionEndButtons) {
        const auto boundary = SectionEndingAtButton(end_button);
        const double current_end = live_cumulative[static_cast<size_t>(end_button - 1)];
        const double pb_end = all_pb[static_cast<size_t>(end_button - 1)];
        const double current_start = boundary.previous_end_button == 0 ? 0.0 :
            live_cumulative[static_cast<size_t>(boundary.previous_end_button - 1)];
        const double pb_start = boundary.previous_end_button == 0 ? 0.0 :
            all_pb[static_cast<size_t>(boundary.previous_end_button - 1)];
        const double section_delta = (current_end - current_start) -
            SectionDurationFromRecordedPB(direct_sections, all_pb, boundary);
        const double cumulative_delta_change =
            (current_end - pb_end) - (current_start - pb_start);
        assert(std::abs(section_delta - cumulative_delta_change) < 1e-8);
    }
    for (int section_number = 1; section_number <= 31; ++section_number) {
        int last_button = 0;
        for (int button = 1; button <= 37; ++button) {
            const auto boundary = SectionEndingAtButton(button);
            if (boundary.section != section_number) continue;
            last_button = button;
            const int button_count = boundary.end_button - boundary.previous_end_button;
            assert(std::abs(SectionDurationFromPB(all_pb, boundary) -
                            button_count * 1.25) < 1e-8);
            assert(std::abs(SectionDurationFromSegments(all_golds, boundary) -
                            button_count * 0.75) < 1e-8);
        }
        assert(last_button != 0);
    }
    // Regression: fractional-millisecond endpoints at all 31 boundaries
    // must satisfy the HUD identity, using exactly the printed millisecond
    // values. The former raw-double subtraction disagreed by +/-1 ms.
    std::array<double, 37> precise_run{}, precise_saved_pb{};
    for (int i = 0; i < 37; ++i) {
        precise_run[static_cast<size_t>(i)] = (i + 1) * 1.625 +
            (i % 2 == 0 ? 0.00049 : 0.00051);
        precise_saved_pb[static_cast<size_t>(i)] = (i + 1) * 1.500 +
            (i % 2 == 0 ? 0.00051 : 0.00049);
    }
    for (int end_button : kSectionEndButtons) {
        const auto boundary = SectionEndingAtButton(end_button);
        const double start_run = boundary.previous_end_button == 0 ? 0.0 :
            precise_run[static_cast<size_t>(boundary.previous_end_button - 1)];
        const double start_pb = boundary.previous_end_button == 0 ? 0.0 :
            precise_saved_pb[static_cast<size_t>(boundary.previous_end_button - 1)];
        const double finish_run = precise_run[static_cast<size_t>(end_button - 1)];
        const double finish_pb = precise_saved_pb[static_cast<size_t>(end_button - 1)];
        const double section_vs_pb =
            DisplayedDurationSeconds(start_run, finish_run) -
            SectionDurationFromPB(precise_saved_pb, boundary);
        const double pb_delta_change =
            DisplayedTimestampDeltaSeconds(finish_run, finish_pb) -
            DisplayedTimestampDeltaSeconds(start_run, start_pb);
        assert(std::abs(section_vs_pb - pb_delta_change) < 1e-9);
    }

    // No inferred per-button duration when an intermediate PB edge is missing.
    all_pb[19] = -1.0; // 18.1's missing time
    assert(PBButtonSegmentDuration(all_pb, 20) < 0.0);
    assert(ProfileRecordedTimeAtButton(all_pb, all_golds, 21,
        ProfileCompareMode::PersonalBest, RecordTimeView::Segment) < 0.0);
    // Full section 18 remains measurable using the two surviving boundaries.
    assert(std::abs(SectionDurationFromPB(all_pb,
        SectionEndingAtButton(21)) - 2.5) < 1e-8);

    assert(PBKey(1) == L"Button01");
    assert(PBKey(37) == L"Button37");
}

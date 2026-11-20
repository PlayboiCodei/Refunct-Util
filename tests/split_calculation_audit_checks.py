"""Source-level integration checks for every split/time display path.

Numeric reference math is exercised across all 37 buttons in pure_tests.cpp.
This test checks the Windows-only record and UI wiring that Linux tests cannot compile.
"""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
hud = (root / "src/hud.cpp").read_text(encoding="utf-8")
records = (root / "src/records.cpp").read_text(encoding="utf-8")
practice = (root / "src/practice.cpp").read_text(encoding="utf-8")
menu = (root / "src/menu.cpp").read_text(encoding="utf-8")
timer = (root / "src/timer.cpp").read_text(encoding="utf-8")
pure = (root / "src/pure.cpp").read_text(encoding="utf-8")

# Split popup: cumulative, individual segment, PB delta, old fastest,
# external profile delta, individual-button and completed-section deltas.
for required in (
    'FormatSplitTime(split.run_seconds)',
    'FormatSplitTime(split.segment_seconds)',
    'ActivePBRecordedTimeAtButton(',
    'DisplayedTimestampDeltaSeconds(split.run_seconds, best)',
    'split.segment_seconds - split.comparison_best_seconds',
    'split.run_seconds - reference',
    'current_section_seconds - reference_seconds',
    'completed->section_pb_reference_seconds',
    'completed->section_actual_seconds',
    'SectionDurationFromRecordedPB(',
    'SectionDurationFromSegments(',
    'ProfileCompareMode::BestSegments',
):
    assert required in hud, required

# "Compare against" must also apply to the section-vs-file mode, not just
# comparison HUD and button popups.
assert 'const bool prefer_best = g_compare_mode == ProfileCompareMode::BestSegments;' in hud
assert 'const double reference_seconds = prefer_best ? gold_seconds : pb_seconds;'  in hud
assert 'const double reference_seconds = old_pb_section;'  in hud

# No zero-length/fictional split if the button count skips a native edge.
assert 'double segment = -1.0;' in records
assert 'segment = DisplayedDurationSeconds(previous, observed_elapsed);' in records
assert 'consecutive && has_split_predecessor' in records
assert 'if (has_practice_anchor)' in records
assert 'segment = DisplayedDurationSeconds(' in records
assert 'g_practice_gold_anchor_seconds, observed_elapsed);' in records

# Practice must supply a real timestamp even when Save Fastest Segments is OFF.
assert 'if (g_engine_timer_valid) {' in practice
assert 'g_practice_gold_anchor_button = state->buttons;' in practice
assert 'g_practice_gold_anchor_seconds = anchor;' in practice
assert 'g_practice_gold_anchor_button = observed_button;' in records

# A legitimate manual full-run PB RunTime is usable at the finish, but must
# never be substituted for missing intermediate splits.
assert 'button == kRefunctButtonCount' in hud
assert 'view == RecordTimeView::Total' in hud

# Live timer and saved PB use the same rounding, and sign-only ties show =.
assert 'FormatSplitTime(elapsed)' in timer
assert 'std::llround(seconds * 1000.0)' in pure
assert 'include_time ? FormatMilliseconds(0) : L"="' in pure

# All named split/time menu paths remain available after the audit.
for label in ('Split Popups & Comparisons', 'Saved Times', 'Configure Popup Rows',
              'Compare Profile & Mode', 'Full-run PB time', 'Sum of best segments',
              'Run time at last button', 'Fastest button record',
              'PB time at last button', 'Total vs other profile', 'Section time vs PB',
              'Section time vs other profile'):
    assert label in menu, label
print('PASS: all split and timer paths use consistent references and validity rules')

"""Regression guards for the HUD section comparison after another button event."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
hud = (root / 'src/hud.cpp').read_text(encoding='utf-8')
records = (root / 'src/records.cpp').read_text(encoding='utf-8')

# A button in the next section may be the latest entry while the previous
# section's original 4-second popup is still active.
assert 'item.section_popup_expires > popup_now' in hud
assert 'const double old_pb_section = completed->section_pb_reference_seconds;' in hud
assert 'split.section_pb_reference_seconds' not in hud
assert 'split.section_pb_reference_seconds = SectionDurationFromRecordedPB(' in records
assert 'split.section_popup_expires = popup_expires;' in records

# Do not fabricate a section duration when the run did not begin at zero,
# or when section practice is using synthetic button progression.
assert 'if (!g_run_started_at_zero) return -1.0;' in records
assert 'g_practice_origin != PracticeOrigin::Section' in records
assert 'PracticeSectionComparisonBoundary(' in records
assert 'split.button_number == boundary.previous_end_button' in records
assert 'if (g_practice_active)' in records
assert 'g_practice_phase != PracticePhase::Ready' in records

# PB and external-file display both use the same measured section duration.
assert '? completed->section_actual_seconds : -1.0;' in hud
assert 'draw_section_delta(g_section_vs_file_display,' in hud
print('PASS: section popup retains completed PB snapshot and measured boundaries')

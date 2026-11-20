"""Protect real native split capture and consistent N/A comparison placeholders."""
from pathlib import Path
root = Path(__file__).resolve().parents[1]
h = (root / 'include/refunct/section_compare.hpp').read_text(encoding='utf-8')
records = (root / 'src/records.cpp').read_text(encoding='utf-8')
sections = (root / 'src/sections.cpp').read_text(encoding='utf-8')
hud = (root / 'src/hud.cpp').read_text(encoding='utf-8')
assert 'CompleteNativeButtonSequence(' in h
assert 'PracticeSectionComparisonBoundary(' in h
assert 'g_practice_section_anchor_seconds' in records
assert 'g_practice_section_anchor_button' in records
assert 'completed_section.section != 0 && timed_section' in records
assert 'g_practice_phase == PracticePhase::Ready' in records
assert 'void CaptureNativeButtonSplit(' in records
assert 'CaptureNativeButtonSplit(player, before, after, actor);' in sections
assert 'CaptureGlobalLastButtonContact(player, before, after);' in sections
assert 'if (complete_native_sequence)' in records
assert 's_native_button_observations[button - 1].elapsed' in records
assert 'record_button(button_count, g_elapsed_seconds, previous_button);' in records
assert 'ClearNativeButtonObservations();' in records
assert 'section_pb_reference_seconds = SectionDurationFromRecordedPB(' in records
assert 'const double old_pb_section = completed->section_pb_reference_seconds;' in hud
assert 'prefix + L"N/A"' in hud
assert '(vs sum of best)' in hud
assert '? completed->section_actual_seconds : -1.0;' in hud
print('PASS: multi-button native capture and honest missing-reference HUD')

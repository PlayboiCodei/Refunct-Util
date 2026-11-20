"""Regression guards: every enabled split row displays N/A when unavailable."""
from pathlib import Path

hud = (Path(__file__).resolve().parents[1] / 'src/hud.cpp').read_text(encoding='utf-8')

# PB line must reserve layout even if there is no valid reference or in practice.
assert 'const bool has_pb_row = g_show_splits && g_pb_row_mode != SplitRowMode::Off;' in hud
assert '(has_pb_row ? 30.0f : 0.0f)' in hud
assert 'if (has_pb_row) {' in hud
assert 'if (has_pb_delta) {' in hud
assert 'DrawCenteredText(hud, prefix + L"N/A", center_x, y, 0.96f,' in hud

# Enabled popup rows always receive the same missing-value marker, rather
# than being skipped or relying on nonstandard fallback strings.
assert 'label + L"N/A"' in hud
assert 'L"No reference"' not in hud
assert 'L"No previous best"' not in hud
assert 'L"Practice N/A"' not in hud
assert 'L" (file unavailable)"' not in hud
assert 'L" (no PB / best reference)"' not in hud
assert 'L" (no reference)"' not in hud
assert 'split.first_best_without_reference' not in hud

# Section comparisons reserve one row regardless of whether a full section
# ended, then only read the completed section when it exists.
assert 'g_section_vs_pb_display != SectionCompareDisplayMode::Off;' in hud
assert 'g_section_vs_file_display != SectionCompareDisplayMode::Off;' in hud
assert '(show_section_pb ? 30.0f : 0.0f)' in hud
assert '(show_section_file ? 30.0f : 0.0f)' in hud
for mode in ('pb', 'file'):
    assert f'if (show_section_{mode} && !section_completed) {{' in hud
    assert f'if (show_section_{mode} && section_completed) {{' in hud

# Invalid timing values should not render nonsensical negative or NaN text.
assert 'valid_total ? FormatSplitTime(split.run_seconds) : L"N/A"' in hud
assert 'valid_segment ? FormatSplitTime(split.segment_seconds) : L"N/A"' in hud
assert 'std::isfinite(reference) && reference >= 0.0' in hud
assert 'DeltaColor(DeltaDirection::Invalid, fade)' in hud
print('PASS: enabled split rows preserve layout and use neutral N/A consistently')

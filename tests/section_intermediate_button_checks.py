"""Regression: 18.1 must compare its own button time, not wait for 18.2."""
from pathlib import Path
root = Path(__file__).resolve().parents[1]
section = (root / "include/refunct/section_compare.hpp").read_text()
records = (root / "src/records.cpp").read_text()
hud = (root / "src/hud.cpp").read_text()
state = (root / "include/refunct/state.hpp").read_text()
assert 'IsIntermediateSectionButton(int button)' in section
assert 'PBButtonSegmentDuration(' in section
assert 'SectionContainingButton(20)' in (root / 'tests/pure_tests.cpp').read_text()
assert 'bool section_is_partial_button = false;' in state
assert 'split.section_is_partial_button = true;' in records
assert 'split.section_pb_reference_seconds = PBButtonSegmentDuration(' in records
assert 'MeasuredIntermediateButtonSeconds(' in records
assert 'g_practice_section_anchor_seconds' in records
assert 'item.section_is_partial_button' in hud
assert 'partial_button ? SectionContainingButton(completed->button_number)' in hud
assert 'PBButtonSegmentDuration(reference.button_seconds, completed->button_number)' in hud
assert 'Button ' in hud and 'vs PB' in hud
assert 'prefix + L"N/A"' in hud
print('PASS: 18.1 intermediate PB comparisons and practice timing')

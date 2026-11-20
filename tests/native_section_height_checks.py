"""Static preset integration checks; runtime gameplay requires Windows x86."""
from pathlib import Path
root = Path(__file__).resolve().parents[1]
sections = (root / 'src/sections.cpp').read_text()
practice = (root / 'src/practice.cpp').read_text()
menu = (root / 'src/menu.cpp').read_text()
cmake = (root / 'CMakeLists.txt').read_text()
source = (root / 'src/section_heights.cpp').read_text()
assert 'src/sections.cpp' in cmake and 'src/section_heights.cpp' in cmake
assert 'refunct_section_heights_tests' in cmake
assert 'kSectionCount = 31' in (root/'include/refunct/section_heights.hpp').read_text()
assert source.count('Preset{') == 31
assert 'Preset{-2499.0f, 0.0f, -2499.0f}' in source
assert 'section_heights::Target' in sections
assert 's_sections.mismatched_preset' in sections
assert 'preset source-height mismatch' in sections
assert 'ObserveNativeSectionHeights' not in sections
assert 'section-heights.ini' not in sections
assert 'section-calibration.txt' not in sections
assert 'SectionTargetZ(i, SectionPlacement::Auto, auto_z)' in sections
assert 'const auto old_modes' in sections and 'original_positions' in sections
assert 'GetSectionRootLocation(actor, actual)' in sections
assert 'IsSyntheticPracticeButtonContact()' in sections
assert 'IsSyntheticPracticeButtonContact()' in practice
# V4's World menu exposes offset controls, not the old status-only row.
assert 'SectionHeightStatus()' in sections
assert 'SectionHeightStatus()' in (root / 'include/refunct/features/practice.hpp').read_text()
assert 'L"Adjust section positions  >"' in menu
assert 'case Page::WorldSectionHeights:' in menu
print('PASS: embedded presets, geometry checks, rollback and menu')

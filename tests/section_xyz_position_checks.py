"""Integration checks for section X/Y/Z movement and safe native offsets.

Tests source-level wiring; native Refunct Win32 runtime validation remains necessary.
"""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
sections = (root / 'src/sections.cpp').read_text(encoding='utf-8')
menu = (root / 'src/menu.cpp').read_text(encoding='utf-8')
state = (root / 'include/refunct/state.hpp').read_text(encoding='utf-8')
feature = (root / 'include/refunct/features/practice.hpp').read_text(encoding='utf-8')

# XY anchor is taken from the actual level actor before the first horizontal edit.
for token in ('x_offsets', 'y_offsets', 'xy_anchors', 'xy_anchor_valid',
              'new_anchors[i] = original_positions[i];',
              'target.x = anchor.x + s_sections.x_offsets[index];',
              'target.y = anchor.y + s_sections.y_offsets[index];',
              'target.z = target_z;', 'const bool move_xy',
              'if (preserve_auto_z && mode == SectionPlacement::Auto)',
              'const bool xy_only_auto =', 'const bool preserve_native_z =',
              'std::abs(final_x) > 10000000.0f'):
    assert token in sections, token

# An edit to any axis is applied atomically; failures restore original positions.
for token in ('bool ApplySectionPositionOffsets(',
              'const auto old_x = s_sections.x_offsets;',
              'const auto old_y = s_sections.y_offsets;',
              'const auto old_z = s_sections.height_offsets;',
              'const auto old_transitions = s_sections.transitions;',
              's_sections.x_offsets = old_x;',
              's_sections.y_offsets = old_y;',
              's_sections.height_offsets = old_z;',
              's_sections.xy_anchor_valid = old_valid;',
              's_sections.transitions = old_transitions;',
              's_sections.height_offsets[i] != 0.0f ||',
              's_sections.x_offsets[i] != 0.0f ||',
              's_sections.y_offsets[i] != 0.0f',
              'g_recording_tainted = true;',
              'g_run_pb_eligible = false;'):
    assert token in sections, token

# All-section and individual paths support all 3 axes and preserve old Z controls.
for fn in ('GetSectionPositionOffset', 'AdjustSectionPositionOffset',
           'AdjustAllSectionPositionOffsets', 'ResetSectionPositionOffsets',
           'ResetAllSectionPositionOffsets', 'GetSectionHeightOffset',
           'AdjustSectionHeightOffset'):
    assert fn in feature and fn in sections, fn
for token in ('WorldSectionPositionEdit', 'L"All sections X: "',
              'L"All sections Y: "', 'L"All sections Z: "',
              'L" X: "', 'L" Y: "', 'L" Z: "',
              'AdjustAllSectionPositionOffsets(g_selection, key == VK_RIGHT ? +1 : -1)',
              'AdjustSectionPositionOffset(s_position_edit_section, g_selection,',
              'ResetSectionPositionOffsets(s_position_edit_section)',
              'ResetAllSectionPositionOffsets()',
              'SetPage(Page::WorldSectionPositionEdit);',
              'SetPage(Page::WorldIndividualSectionHeights); break;'):
    assert token in menu, token
assert 'WorldSectionPositionEdit' in state
print('Section XYZ position integration checks passed')

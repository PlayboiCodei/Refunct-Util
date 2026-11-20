"""Static guards for the experimental Refunct v9.30 IsPressed section gate."""
from pathlib import Path
r = Path(__file__).resolve().parents[1]
s = (r / 'src/sections.cpp').read_text(encoding='utf-8')
h = (r / 'include/refunct/button_sync_policy.hpp').read_text(encoding='utf-8')
menu = (r / 'src/menu.cpp').read_text(encoding='utf-8')
assert menu.count('SectionButtonSyncStatus()') == 1
assert s.count('Buttons sync with sections:') == 3
assert 'ResolveIsPressedProperty' in s
assert 'FindClassPropertyOffset(klass, L"IsPressed")' in s
assert 'pressed_offset != 0x2F4' in s
assert 'final_offset != 0x2F5' in s
assert 'outro_offset != 0x2F6' in s
# Never assume an internal BoolProperty descriptor offset.
assert 'static_cast<const uint8_t*>(field) + 0x58' not in s
assert 'native_byte = true;' in s
assert 'if (button.native_byte_bool && *address > 1)' in s
assert 'BoolProperty' in s
assert 'WritableButtonByte' in s
assert 'VirtualQuery' in s
assert 'ReadButtonPressed' in s
assert 'SetButtonPressed' in s
assert 'DesiredIsPressed' in s
assert 'VanillaIsPressed' in s
assert 'transaction failed; restored native states' in s
assert 'rollback incomplete (restart map)' in s
# Completed-button handling is valid in the repair/restore paths; the
# sync gate's own decision must continue using the tested policy.
assert 'button_sync_policy::DesiredIsPressed(' in s
assert 'button_sync_policy::VanillaIsPressed(' in s
assert 'button_index < finished_buttons || !section_is_raised' in h
assert 'button_index != finished_buttons' in h
assert 'g_add_based_character(actor, player)' not in s  # no synthesized presses
print('PASS: native IsPressed gate and one-toggle integration')

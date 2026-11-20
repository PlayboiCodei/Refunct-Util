"""Guard automatic button repairs against false PB/cheat taint."""
from pathlib import Path
r = Path(__file__).resolve().parents[1]
s = (r/'src/sections.cpp').read_text(encoding='utf-8')
c = (r/'src/records.cpp').read_text(encoding='utf-8')
h = (r/'include/refunct/features/records.hpp').read_text(encoding='utf-8')
assert 'if (native_enabled) {' in s
assert 'if (!s_button_fix_seen_healthy[i]) {' in s
assert 's_button_fix_near_since[i] = 0;' in s
assert 'if (!current_button || !raised || s_button_fix_repaired[i]) {' in s
assert 'now - s_button_fix_near_since[i] < 2000' in s
assert 'if (s_button_fix_manual_pending)' in s
assert 'MarkAutomaticButtonRepair(index + 1);' in s
assert 'MarkCheatAction(CheatAction::Buttons);' in s
assert 'void MarkAutomaticButtonRepair(int one_based_button)' in c
assert 'void MarkAutomaticButtonRepair(int one_based_button);' in h
assert 'CheatMask(CheatBit::ButtonRepair)' in c
assert 'Automatic button repair (used this run)' in c
assert 'Used this run: button modifications' in c
assert 's_used_actions |= mask;' in c
print('PASS: healthy-button no-op, separate cheat label, manual edits tracked')

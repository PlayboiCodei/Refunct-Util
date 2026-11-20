"""Source-level checks for cumulative-PB gain/loss popup integration."""
from pathlib import Path
root = Path(__file__).resolve().parents[1]
menu = (root/'src/menu.cpp').read_text(encoding='utf-8')
hud = (root/'src/hud.cpp').read_text(encoding='utf-8')
rec = (root/'src/records.cpp').read_text(encoding='utf-8')
state = (root/'include/refunct/state.hpp').read_text(encoding='utf-8')
store = (root/'src/storage.cpp').read_text(encoding='utf-8')
assert 'Time gained/lost at button: ' in menu
assert 'g_selection == 8' in menu
assert 'else if (i == 8) { ChangeSectionCompareMode(g_pb_gain_loss_display, +1);' in menu
assert 'g_pb_gain_loss_display = SectionCompareDisplayMode::Off;' in menu
assert 'L"PBGainLossDisplay"' in store
assert 'split.pb_gain_loss_seconds = PBLeadChangeSeconds(' in rec
assert 'consecutive && has_split_predecessor' in rec
assert '!g_practice_active && g_run_started_at_zero' in rec
assert 'double pb_gain_loss_seconds = std::numeric_limits<double>::quiet_NaN();' in state
assert 'FormatSplitDelta(split.pb_gain_loss_seconds, with_time)' in hud
assert 'show_pb_gain_loss ? 30.0f' in hud
assert 'if (show_pb_gain_loss) {' in hud
assert (root/'include/refunct/section_compare.hpp').read_bytes() == (root/'include/refunct/features/section_compare.hpp').read_bytes()
print('PASS: PB gain/loss is an independent signed, snapshotted popup row')

"""Static parity checks for the Cheats Active HUD (Windows runtime not required)."""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
records = (root / 'src/records.cpp').read_text(encoding='utf-8')
hud = (root / 'src/hud.cpp').read_text(encoding='utf-8')
menu = (root / 'src/menu.cpp').read_text(encoding='utf-8')
state = (root / 'include/refunct/state.hpp').read_text(encoding='utf-8')
storage = (root / 'src/storage.cpp').read_text(encoding='utf-8')

# Replaced the duplicated HUD list with the central status registry.
mask = records.split('uint64_t AllSettingMask() {', 1)[1].split('uint64_t SettingCheatMask()',1)[0]
labels = records.split('std::vector<std::wstring> LabelsForMask', 1)[1].split('} // namespace',1)[0]
block = hud.split('if (g_show_active_cheats) {', 1)[1].split('if (g_practice_active &&', 1)[0]
flags = (
    'g_walk_speed_percent', 'g_swim_speed_percent', 'g_climb_speed_percent',
    'g_slide_speed_percent', 'g_slide_distance_percent', 'g_jump_height_percent',
    'g_acceleration_percent', 'g_game_speed_percent', 'g_preserve_momentum',
    'g_scroll_to_jump', 'g_hold_space_to_jump', 'g_buttons_sync_with_sections',
    'g_flying', 'g_noclip', 'g_restore_flying_pending', 'g_restore_noclip_pending',
)
for flag in flags:
    assert flag in mask, f'cheat not tracked in shared registry: {flag}'
assert 'ActiveCheatLabels()' in block
assert 'LabelsForMask(ActiveCheatMask() & ~CheatMask(CheatBit::WorldObjectRemoval))' in records
assert 'add_line(HudElement::ActiveCheats, name' in block
assert 'Used this run:' in labels

# HUD toggles and preference switches never appear in the active mask.
for excluded in ('g_show_timer', 'g_show_enabled_modules', 'g_recording_tainted',
                 'g_save_personal_bests', 'g_cheat_record_mode'):
    assert excluded not in mask, f'HUD/preference incorrectly marked cheat: {excluded}'
assert 'if (g_show_active_cheats)' in hud
assert 'Cheats Active (' in block
assert 'L"None"' in block
assert 'L"Active gameplay cheats"' in state or 'L"Active gameplay cheats"' in menu or 'L"Active gameplay cheats"' in hud
assert 'g_show_active_cheats = !g_show_active_cheats' in menu
assert 'g_show_active_cheats = false' in menu
assert 'extern bool g_show_active_cheats' in state
assert 'L"ActiveCheatsPosition"' in state
assert 'L"ShowActiveCheats"' in storage
assert 'g_show_active_cheats = GetPrivateProfileIntW' in storage
assert 'g_hud_corners = kDefaultHudCorners' in menu
print('PASS: Cheats Active HUD tracks every current cheat and persists independently')

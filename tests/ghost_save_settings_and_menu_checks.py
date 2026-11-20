"""Persistent save toggles, menu navigation and cheat permission consistency."""
from pathlib import Path
r = Path(__file__).resolve().parents[1]
read=lambda p:(r/p).read_text(encoding='utf-8')
state, settings, menu, records, ghost = (
    read('src/state.cpp'),read('src/storage.cpp'),read('src/menu.cpp'),
    read('src/records.cpp'),read('src/ghost.cpp'))
for option, ini in [('g_save_pb_ghosts','SavePBGhosts'),('g_save_fastest_ghosts','SaveFastestGhosts')]:
    assert f'bool {option} = true;' in state
    assert f'extern bool {option};' in read('include/refunct/state.hpp')
    assert f'L"{ini}", {option} ? L"1" : L"0"' in settings
    assert f'L"Recording", L"{ini}", 1, g_settings_path.c_str()' in settings
    assert f'{option} = !{option};' in menu
    assert f'!{option}' in ghost
assert 'Save PB ghosts:' in menu and 'Save fastest-section ghosts:' in menu
assert 'Save with cheats:' in menu
assert 'CycleCheatRecordMode(+1)' in menu
assert 'CycleCheatRecordMode(key == VK_RIGHT ? +1 : -1)' in menu
assert 'else if (i == 4) CycleCheatRecordMode(+1)' in menu
assert 'g_selection == 4' in menu
assert 'g_auto_create_pb_profile' in menu and 'g_auto_pb_copy_best_segments' in menu
assert 'g_selection == 5' in menu and 'g_selection == 6' in menu
# Respect the same cheat gate used to write records, not a special uncheatable ghost rule.
assert '!RecordsAllowedForPB()' in ghost
assert '!RecordsAllowedForSegments()' in ghost
assert 'CheatRecordsPB(g_cheat_record_mode)' in records
assert 'CheatRecordsSegments(g_cheat_record_mode)' in records
assert 'g_pb_cheats_allowed_for_run && CheatRecordsPB(g_cheat_record_mode)' in records
assert 'if (PBGhostSavedForLatestPB() && IsRegularFile(source_ghost)' in records
assert 's_pb_ghost_saved_for_latest_pb = false;' in ghost
assert 's_pb_ghost_saved_for_latest_pb = true;' in ghost
# Ghost save settings are not gameplay changes.
mask = records.split('uint64_t AllSettingMask()',1)[1].split('uint64_t SettingCheatMask()',1)[0]
assert 'g_save_pb_ghosts' not in mask and 'g_save_fastest_ghosts' not in mask
print('PASS: ghost save toggles persisted and wired to menu, modes, and no-cheat HUD')

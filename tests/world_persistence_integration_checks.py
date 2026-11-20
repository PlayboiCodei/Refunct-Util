"""World persistence UI, storage and native restart wiring checks."""
from pathlib import Path
root = Path(__file__).resolve().parents[1]
read = lambda p: (root / p).read_text(encoding="utf-8")
menu = read("src/menu.cpp")
move = read("src/movement.cpp")
tick = read("src/game_tick.cpp")
state = read("src/state.cpp")
header = read("include/refunct/state.hpp")
storage = read("src/storage.cpp")
assert "Page::MovementPersistence" not in menu and "MovementPersistence," not in header
assert 'L"Persistence  >"' in menu
assert 'Settings / Persistence' in menu
for row in ('Keep movement on game launch:', 'Keep movement on map restart:',
            'Keep world on game launch:', 'Keep world on map restart:'):
    assert row in menu
for field in ('g_keep_world_on_game_launch', 'g_keep_world_on_map_restart'):
    assert f'extern bool {field};' in header
    assert f'bool {field} = false;' in state
    assert field in storage and field in move and field in menu
for key in ('L"World", L"KeepOnGameLaunch"', 'L"World", L"KeepOnMapRestart"'):
    assert storage.count(key) == 2  # one reader, one writer
assert 'if (!g_keep_world_on_map_restart) ResetWorldGameplayForPersistence();' in move
assert 'if (!g_keep_world_on_game_launch) ResetWorldGameplayForPersistence();' in move
assert 'if (!force_defaults) ResetWorldOnNormalRestart();' in move
assert 'ResetWorldOnNormalRestart();' in tick
assert 'ApplyWorldGameLaunchPersistence();' in tick
# Native actor replacement must never run World native setters on stale pointers.
assert tick.index('const bool native_world_restart') < tick.index('if (g_player != player)')
assert tick.index('if (native_world_restart) ResetWorldOnNormalRestart();') > tick.index('if (post_tick_valid) {')
assert 'if (!WorldGameplayOptionsModified()) return;' in menu
reset_code = menu.split('void ResetWorldGameplayForPersistence()', 1)[1].split('// These are defined in refunct', 1)[0]
assert 'if (!RestoreDefaultButtons())' not in reset_code
assert 'ResetWorldObjectRemovals()' in reset_code
assert 'ResetAllSectionPositionOffsets()' in reset_code
assert 'NoteWorldProfilePersistenceReset();' in reset_code
assert 'SaveMovementModeSettings();' in menu  # fast Fly/Noclip save restored
print('PASS: four persistence rows, independent storage, lifecycle, quiet reset and linker fix')

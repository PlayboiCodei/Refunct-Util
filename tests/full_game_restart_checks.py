"""Regression for the full-level reload replacing process-relaunch.

Retain the old filename so upgrades overwrite the previous helper-based test.
"""
from pathlib import Path
root = Path(__file__).resolve().parents[1]
restart = (root / 'src/restart.cpp').read_text(encoding='utf-8')
tick = (root / 'src/game_tick.cpp').read_text(encoding='utf-8')
menu = (root / 'src/menu.cpp').read_text(encoding='utf-8')
state = (root / 'include/refunct/state.hpp').read_text(encoding='utf-8')
cmake = (root / 'CMakeLists.txt').read_text(encoding='utf-8')
bat = (root / 'build_win32.bat').read_text(encoding='utf-8')
header = (root / 'include/refunct/features/restart.hpp').read_text(encoding='utf-8')
assert 'Page::ConfirmLevelReload' in menu
assert 'ConfirmLevelReload' in state
assert 'Full level reload (keep game open)' in menu
assert 'RequestFullLevelReload()' in menu
assert 'ExecuteFullLevelReload(player)' in tick
assert 'UpdatePreloadedRestart();' in tick and 'CheckFullLevelReloadCompletion();' in tick
assert 'g_original_tick(player, dt)' in tick
assert 'bool RequestFullLevelReload();' in header
assert 'bool ExecuteFullLevelReload(MyCharacter* player);' in header
assert 'UGameplayStatics' in restart
assert 'FindGameplayStaticsDefaultObject()' in restart
assert 'FindReflectedFunction(statics, L"OpenLevel")' in restart
for key, kind, offset in [('WorldContextObject','ObjectProperty',0),
                          ('LevelName','NameProperty',4),
                          ('bAbsolute','BoolProperty',12),
                          ('Options','StrProperty',16)]:
    assert f'HasOpenLevelParameter(function, L"{key}", L"{kind}", {offset})' in restart
assert 'RestartRun(player)' in restart.split('bool ExecuteFullLevelReload(', 1)[1]
assert 'ResetSectionPlacementInternals();' in restart
assert 'g_practice_level_actors.fill(nullptr);' in restart
assert 'g_process_event(statics, function, params.data());' in restart
assert 'CheckFullLevelReloadCompletion()' in restart
for forbidden in ['RequestFullGameRestart','CreateProcessW','WM_CLOSE','RefunctUtilityRestartHelper']:
    assert forbidden not in restart
assert 'RefunctUtilityRestartHelper' not in cmake
assert 'RefunctUtilityRestartHelper' not in bat
assert '--target RefunctUtility --clean-first' in bat
print('PASS: menu and native full-level reload validated without process relaunch')

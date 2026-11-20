"""Regression: Restart with Progress must always restore existing buttons.

This file keeps its original name so users can overwrite the older soft-reload
checks without manually deleting files from a project directory.
"""
from pathlib import Path
root = Path(__file__).resolve().parents[1]
restart = (root / 'src/restart.cpp').read_text(encoding='utf-8')
practice = (root / 'src/practice.cpp').read_text(encoding='utf-8')
tick = (root / 'src/game_tick.cpp').read_text(encoding='utf-8')
menu = (root / 'src/menu.cpp').read_text(encoding='utf-8')
storage = (root / 'src/storage.cpp').read_text(encoding='utf-8')
state = (root / 'include/refunct/state.hpp').read_text(encoding='utf-8')
body = practice.split('void RequestProgressRestart()', 1)[1].split('// TAS New Game Actions', 1)[0]
assert 'RequestPreparedPractice(pressed, Vec3{}, nullptr, PracticeOrigin::ProgressRestart)' in body
assert 'g_progress_restart_pending = true' in body
assert 'g_progress_restart_active = true' in body
assert 'RefunctSaveExists' not in body
assert 'OpenLevel' not in body
assert 'g_practice_origin == PracticeOrigin::ProgressRestart' in tick
assert 'ResetMovementOnNormalRestart(player)' in tick
assert 'ReloadCurrentLevelFromSave' not in tick
assert 'UpdateSoftReloadLoadingUiSuppression' not in tick
assert 'ReloadCurrentLevelFromSave' not in restart
assert 'bool ExecuteFullLevelReload(MyCharacter* player)' in restart
assert 'OpenLevel' in restart
assert 'Progress restart method:' not in menu
assert 'ToggleProgressRestartMethod' not in menu
assert 'ProgressRestartMethod' not in state
assert 'ProgressRestartMethod' not in storage
assert 'R / restore rise animation:' in menu
assert 'else if (i == 3) ToggleProgressRiseAnimation();' in menu
assert 'g_page == Page::RestartOptions && g_selection == 3' in menu
print('PASS: progress restart remains native button replay; separate full level reload allowed')

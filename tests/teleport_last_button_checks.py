"""Global teleport is independent of per-run B/N checkpoint replay."""
from pathlib import Path
root = Path(__file__).resolve().parents[1]
menu = (root / 'src/menu.cpp').read_text(encoding='utf-8')
practice = (root / 'src/practice.cpp').read_text(encoding='utf-8')
movement = (root / 'src/movement.cpp').read_text(encoding='utf-8')
api = (root / 'include/refunct/features/practice.hpp').read_text(encoding='utf-8')
movement_h = (root / 'include/refunct/features/movement.hpp').read_text(encoding='utf-8')
assert 'void TeleportToLastPressedButton(MyCharacter* player);' in api
assert 'L"Teleport to last button (global)  ["' in menu
assert menu.split('case Page::SavePositions:', 1)[1].split('case Page::TeleportCoordinates:', 1)[0].count('Teleport to last button (global)') == 1
assert 'Teleport to last button (global)' not in menu.split('case Page::ButtonCheckpoints:', 1)[1].split('case Page::Practice:', 1)[0]
assert 'else if (i == 3) ExecuteAction(Action::TeleportLastButtonGlobal, player);' in menu
assert 'else if (i == 2) {' in menu
assert 'g_page == Page::ButtonCheckpoints && g_selection == 2' in menu
assert 'else if (i == 5) GoBack();' in menu
assert 'bool allow_when_practice_ready = false' in movement_h
assert 'g_practice_phase == PracticePhase::Ready' in movement
assert 's_global_last_contact = contact;' in practice

# Global last is stored atomically alongside run contacts, but survives
# ClearButtonHistoryOnRestart and does not gate on current progress.
assert 'worker->latest.global = s_global_last_contact;' in practice
assert 'EncodeButtonContact(snapshot.global)' in practice
assert 'GetPrivateProfileStringW(L"History", L"Global"' in practice
assert 'global = last;' in practice  # backward-compatible old save files
restart = practice.split('void ClearButtonHistoryOnRestart() {', 1)[1].split('\n}', 1)[0]
assert 'LoadButtonHistoryOnce();' in restart
assert 's_last_contact = {};' in restart
assert 's_global_last_contact = {};' not in restart

action = practice.split('void TeleportToLastPressedButton(MyCharacter* player) {', 1)[1].split('\n}', 1)[0]
assert 'ValidButtonContact(s_global_last_contact)' in action
assert 's_global_last_contact.checkpoint' in action
assert 's_pending_global_teleport.destination = checkpoint.location;' in action
assert 'UpdateGlobalLastButtonTeleport(player);' in (root / 'src/game_tick.cpp').read_text(encoding='utf8')
assert 'CustomMapApplying()' in practice
assert 'TeleportToCoordinates(player, request.destination, true)' in practice
assert 'PracticeLevelState()' not in action
assert 'g_last_button_checkpoint_valid' not in action
assert 'RequestPreparedPractice' not in action
assert 'RequestPractice(' not in action
assert 'g_practice_restart_requested' not in action
assert 'progress unchanged' in practice
print('PASS: global button teleport persists across maps and runs, while B/N stays run-scoped')

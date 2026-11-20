"""Regression checks for the hotkey added to native Full Level Reload.

Run: python tests/full_level_reload_hotkey_checks.py
"""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
state = (root / "include/refunct/state.hpp").read_text(encoding="utf-8")
menu = (root / "src/menu.cpp").read_text(encoding="utf-8")
storage = (root / "src/storage.cpp").read_text(encoding="utf-8")
restart = (root / "src/restart.cpp").read_text(encoding="utf-8")
tick = (root / "src/game_tick.cpp").read_text(encoding="utf-8")

assert "LoadSecondLastButton, FullLevelReload, // Existing action IDs never move." in state, "Do not renumber old actions"
assert '{L"Full level reload", L"FullLevelReload", \'L\'}' in state
assert re.search(r"case Action::FullLevelReload:\s*//.*?RequestFullLevelReload\(\);\s*break;", menu, re.S)
assert 'KeyName(Hotkey(Action::FullLevelReload))' in menu
assert 'case Page::ConfirmLevelReload:' in menu, "Keep menu confirmation"
assert 'if (i == 0) RequestFullLevelReload();' in menu
assert 'for (size_t i = 0; i < kActionCount; ++i)' in storage
assert 'kActions[i].ini_name' in storage, "Persist key by stable INI name"
assert 'g_keys[i].store(loaded[i]);' in storage, "Load customized bindings"
assert 'key != 0 && key == g_keys[i].load()' in menu, "Hotkey dispatcher must include the new action"
assert 'ExecuteAction(static_cast<Action>(i), player);' in menu
assert 'g_full_level_reload_requested = true;' in restart
assert 'ExecuteFullLevelReload(player);' in tick
assert 'OpenLevel' in restart and 'CreateProcessW' not in restart
print('PASS: L hotkey wired through settings, input, and native level reload')

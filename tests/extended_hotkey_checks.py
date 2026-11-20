"""Hotkey integration checks for append-only action IDs and unassigned bindings."""
from pathlib import Path
import re
root = Path(__file__).resolve().parents[1]
header = (root/'include/refunct/state.hpp').read_text(encoding='utf8')
menu = (root/'src/menu.cpp').read_text(encoding='utf8')
core = (root/'src/core.cpp').read_text(encoding='utf8')
input_source = (root/'src/input.cpp').read_text(encoding='utf8')
storage = (root/'src/storage.cpp').read_text(encoding='utf8')
hud = (root/'src/hud.cpp').read_text(encoding='utf8')
enum = header.split('enum class Action : size_t {')[1].split('};')[0]
existing=['Menu','Restart','Fly','Noclip','SavePosition','RestorePosition','Coordinates','Timer','SpeedUp','SpeedDown','ReplaySection','SaveState','LoadState','LoadLastButton','PreloadedRestart','RestartWithProgress','LoadSecondLastButton','FullLevelReload']
ids=[part.strip().split()[0] for part in re.sub(r'//[^\n]*','',enum).split(',') if part.strip()]
assert ids[:len(existing)] == existing, (ids[:len(existing)], existing)
assert ids[-1]=='Count'
new_actions=ids[len(existing):-1]
assert len(new_actions)>=20
for action in new_actions:
    assert f'case Action::{action}:' in menu, action
assert len(re.findall(r'\{L"[^"\n]+", L"[^"\n]+", 0\}',header)) == len(new_actions)
assert re.search(r'vk == 0.*?L"None"',core)
assert 'case VK_BACK:' in core and 'case VK_BACK:' not in core.split('std::wstring KeyName',1)[0]
assert 'loaded[i] != 0 && !IsBindable(loaded[i])' in storage
assert 'vk != 0 && loaded[j] == vk' in storage
assert 'key != 0 && binding.load(' in input_source
assert 'key != 0 && key == g_keys[i].load()' in menu
assert 'key == VK_BACK' in menu and 'g_keys[g_rebind_action].store(0)' in menu
assert 'BACKSPACE' in hud and 'Hotkey cleared: None' in menu
assert 'TeleportToLastPressedButton(player)' in menu
assert 'case Action::TeleportLastButtonGlobal:' in menu
assert 'case Action::ToggleAirJump: ToggleAirJump();' in menu
assert 'case Action::RestoreOriginalMap:' in menu
assert 'RefreshCustomMapFiles()' in menu
print(f'PASS: {len(existing)} old hotkey IDs stable, {len(new_actions)} optional hotkeys and None/rebinding support')

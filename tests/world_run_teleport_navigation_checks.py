"""Regression contracts for World > Run & Practice and global teleport placement.

Validates index-to-action and Back routes without requiring the game process.
"""
from pathlib import Path

menu = (Path(__file__).resolve().parents[1] / 'src/menu.cpp').read_text(encoding='utf-8')

render = menu.split('std::vector<std::wstring> BuildEntries() {', 1)[1].split('const wchar_t* MenuBreadcrumb(', 1)[0]
actions = menu.split('void ActivateSelected(MyCharacter* player) {', 1)[1].split('void ProcessKey(', 1)[0]
back = menu.split('void GoBack() {', 1)[1].split('void ToggleProgressRiseAnimation()', 1)[0]
help_text = menu.split('std::wstring MenuSelectionDescription(', 1)[1].split('void GoBack()', 1)[0]

def page(source, name):
    marker = f'case Page::{name}:'
    assert marker in source
    chunk = source.split(marker, 1)[1]
    return chunk.split('case Page::', 1)[0]

main = page(render, 'Main')
world = page(render, 'World')
positions = page(render, 'SavePositions')
buttons = page(render, 'ButtonCheckpoints')
assert 'L"Run & Practice  >"' not in main
assert 'L"Run & Practice  >"' in world
assert world.index('L"Sections  >"') < world.index('L"Run & Practice  >"') < world.index('L"Buttons  >"')
assert 'L"Teleport to last button (global)  ["' in positions
assert 'Teleport to last button (global)' not in buttons
assert 'L"Teleport to coordinates  >"' in positions

assert 'else if (i == 1) SetPage(Page::World);' in page(actions, 'Main')
assert 'else if (i == 1) SetPage(Page::SaveStates);' in page(actions, 'World')
assert 'else if (i == 3) ExecuteAction(Action::TeleportLastButtonGlobal, player);' in page(actions, 'SavePositions')
assert 'TeleportToLastPressedButton(player)' not in page(actions, 'ButtonCheckpoints')
assert 'else if (i == 2) {' in page(actions, 'ButtonCheckpoints')
assert 'g_keep_last_button_momentum = !g_keep_last_button_momentum;' in page(actions, 'ButtonCheckpoints')
assert 'case Page::SaveStates: SetPage(Page::World); break;' in back
assert 'case Page::Practice: case Page::SavePositions:' in back
assert 'case Page::TeleportCoordinates: SetPage(Page::SavePositions); break;' in back
assert 'case Page::SaveStates: return L"World / Run & Practice";' in menu
assert 'case Page::SavePositions: return L"World / Run & Practice / Positions & Teleport";' in menu
assert help_text.count('Teleport to the latest real button press, across maps and restarts;') == 1
assert 'case Action::TeleportLastButtonGlobal:' in menu  # existing hotkey works unchanged
print('PASS: World menus and global teleport routes, breadcrumbs, descriptions and hotkey')

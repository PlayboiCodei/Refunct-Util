"""Automatic ghost repair + World > Water menu integration guards.

Source-only guards: native Windows x86 compilation and game contact require
in-game verification.
"""
from pathlib import Path
import re
root=Path(__file__).resolve().parents[1]
menu=(root/'src/menu.cpp').read_text(encoding='utf-8')
sections=(root/'src/sections.cpp').read_text(encoding='utf-8')
records=(root/'src/records.cpp').read_text(encoding='utf-8')
api=(root/'include/refunct/features/practice.hpp').read_text(encoding='utf-8')

def chunk(source, name, next_name):
    return source.split(f'case Page::{name}:',1)[1].split(f'case Page::{next_name}:',1)[0]
render=menu.split('std::vector<std::wstring> BuildEntries() {',1)[1].split('// Show the current location',1)[0]
world=chunk(render,'World','Maps')
water=chunk(render,'WorldWater','WorldWaterColours')
buttons=chunk(render,'WorldButtonActions','WorldButtonColours')
assert 'L"Remove water: "' not in world
assert 'L"Remove water: "' in water
assert water.index('L"Remove water: "') < water.index('L"Remove water surface only: "')
assert 'Enable button fix' not in buttons
assert 'L"Repair ghost buttons now"' in buttons
assert 'ToggleButtonFix' not in menu
activate=menu.split('void ActivateSelected(MyCharacter* player) {',1)[1]
world_actions=chunk(activate,'World','WorldCubes')
water_actions=chunk(activate,'WorldWater','WorldWaterColours')
button_actions=chunk(activate,'WorldButtonActions','WorldButtonColours')
assert 'else if (i == 8) SetPage(Page::WorldWater);' in world_actions
assert 'else if (i == 12) ResetWorldGameplayOptions();' in world_actions
assert 'SetAllWaterRemoved(' not in world_actions
assert 'if (i == 0) SetAllWaterRemoved(!IsAllWaterRemoved());' in water_actions
assert 'if (i == 1) SetWaterRemoved(!IsWaterRemoved());' in water_actions
assert 'else if (i == 5) {' in water_actions
assert 'else if (i == 7) RepairGhostButtonsNow();' in button_actions
assert 'else if (i == 8) ToggleButtonTileSelection();' in button_actions
arrows=menu.split('void HandleHorizontalMovement(int key, MyCharacter* player) {', 1)[1].split('} // namespace\n\nvoid ProcessKey(int key, MyCharacter* player) {', 1)[0]
assert 'g_page == Page::WorldWater && g_selection == 0' in arrows
assert 'g_page == Page::WorldWater && g_selection == 3' in arrows
assert 'g_page == Page::WorldButtonActions && g_selection == 8' in arrows
assert 'g_page == Page::World && g_selection == 9' in arrows  # Remove All Objects toggle
assert 'ToggleButtonFix()' not in api
assert 'IsButtonFixEnabled()' not in api
assert 's_button_fix_enabled' not in sections
assert 'EnableButtonFixAll' not in sections
assert 'UpdateButtonFix();' in sections
assert 's_button_fix_retry_after_ms = now + 10000;' in sections
assert 'bool RepairGhostButtonsNow()' in sections
assert 'MarkCheatAction(CheatAction::Buttons);' in sections
# Healthy buttons never trigger automatic writes; genuine repairs still taint.
mask=records.split('uint64_t AllSettingMask() {',1)[1].split('uint64_t SettingCheatMask()',1)[0]
assert 'set(CheatBit::ButtonRepair, IsButtonFixEnabled())' not in mask
repair=sections.split('bool RepairUsableButton(int index) {',1)[1].split('bool ButtonFixPlayerIsNear(',1)[0]
assert repair.index('g_recording_tainted = true;') < repair.index('g_process_event(button.actor, reenable, args.data());')
assert 'MarkCheatAction(CheatAction::Buttons);' in repair
assert 'MarkAutomaticButtonRepair(index + 1);' in repair
assert 'if (native_enabled) {' in sections
assert 'if (!current_button || !raised || s_button_fix_repaired[i])' in sections
assert 'SetAllWaterRemoved(false);' in menu
assert 'ToggleButtonFix()' not in menu
print('PASS: button repair always armed and water master moved with correct menu actions')

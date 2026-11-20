"""World resets: settings and colours are independent and non-destructive.

Source-level checks only; Windows and live native actor behavior need game testing.
"""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
menu = (root / 'src/menu.cpp').read_text(encoding='utf-8')
tiles = (root / 'src/tiles.cpp').read_text(encoding='utf-8')
tiles_h = (root / 'include/refunct/features/tiles.hpp').read_text(encoding='utf-8')

world_entries = menu.split('case Page::World:\n            return {', 1)[1].split('};', 1)[0]
assert world_entries.index('L"Reset World gameplay settings"') < world_entries.index('L"Back"')
assert 'Reset World customisation' not in world_entries
custom_entries = menu.split('case Page::Customisation:\n            return {', 1)[1].split('};', 1)[0]
assert custom_entries.index('L"Restore default customisations"') < custom_entries.index('L"Back"')

activate = menu.split('void ActivateSelected(MyCharacter* player) {', 1)[1]
world_action = activate.split('case Page::World:', 1)[1].split('case Page::Customisation:', 1)[0]
custom_action = activate.split('case Page::Customisation:', 1)[1].split('case Page::WorldWater:', 1)[0]
assert 'else if (i == 8) SetPage(Page::WorldWater);' in world_action
assert 'else if (i == 9) SetAllWorldObjectsRemoved(!AreAllWorldObjectsRemoved());' in world_action
assert 'else if (i == 10) ResetWorldObjectRemovals();' in world_action
assert 'else if (i == 12) ResetWorldGameplayOptions();' in world_action
assert 'if (i == 0) SetAllWaterRemoved(!IsAllWaterRemoved());' in activate.split('case Page::WorldWater:',1)[1].split('case Page::WorldWaterColours:',1)[0]
assert 'ResetWorldCustomisation()' not in world_action
assert 'else if (i == 12) SetPage(Page::WorldPlayer);' in custom_action
assert 'else if (i == 16) ResetWorldCustomisation();' in custom_action

settings = menu.split('void ResetWorldGameplayOptions() {', 1)[1].split('void ResetWorldCustomisation() {', 1)[0]
for fn in (
    'ResetWorldObjectRemovals()', 'ResetJumpPadHeight()', 'ResetPlatformSpeed()', 'ResetTubeTravelSpeed()',
    'ResetSectionSpeed()', 'ResetAllSectionPlacements()',
    'ResetAllSectionPositionOffsets()', 'SetButtonsSyncWithSections(false)',
    'RestoreDefaultButtons()',
    'ToggleButtonTileSelection()', 'ToggleTileSelectionOnContact()',
    'SaveSettings()',
):
    assert fn in settings, f'Missing World-settings reset: {fn}'
assert 'g_player_fov_degrees = 0;' not in settings
assert 'g_animate_section_changes = true;' in settings
for forbidden in ('DeselectWorldTiles(', 'SelectWorldTiles(', 'PressAllAvailableButtons(',
                  'ResetMovementConfiguration(', 'g_game_speed_percent =',
                  'g_run_pb_eligible = true;', 'g_recording_tainted = false;'):
    assert forbidden not in settings, f'World settings reset should not {forbidden}'

custom = menu.split('void ResetWorldCustomisation() {', 1)[1].split('int s_position_edit_section', 1)[0]
assert 'g_player_fov_degrees = 0;' in custom
for fn in (
    'SetButtonCustomColourEnabled(pressed, false)',
    'SetButtonBorderCustomColourEnabled(pressed, false)',
    'SetButtonBeaconVisible(true)', 'SetButtonBeaconCustomColourEnabled(false)',
    'RestoreDefaultTileColours()', 'SetWallCustomColourEnabled(selected, false)',
    'SetTubeCustomColourEnabled(false)', 'SetPlatformCustomColourEnabled(false)',
    'SetJumpPadCustomColourEnabled(false)',
    'SetWaterCustomColourEnabled(false)',
):
    assert fn in custom, f'Missing customisation reset: {fn}'
for forbidden in ('ResetJumpPadHeight(', 'ResetPlatformSpeed(', 'ResetTubeTravelSpeed(',
                  'ResetAllSectionPlacements(', 'RestoreDefaultButtons(',
                  'DeselectWorldTiles(', 'PressAllAvailableButtons(', 'g_run_pb_eligible ='):
    assert forbidden not in custom, f'Customisation reset must not change gameplay: {forbidden}'

assert 'void RestoreDefaultTileColours();' in tiles_h
helper = tiles.split('void RestoreDefaultTileColours() {', 1)[1].split('void SetTileHexColour(', 1)[0]
for token in (
    'CustomColour(selected).enabled = false;', 'SaveCustomColour(selected);',
    's_tile_palette_choices = {{0, 0}};', 's_random_tile_rgb_enabled = false;',
    'L"SelectedPreset"', 'L"UnselectedPreset"', 'L"RandomSelectedRGB"',
    's_tile_palette.restore_defaults = true;',
):
    assert token in helper
for forbidden in ('LevelState.platforms =', 'DeselectWorldTiles(',
                  'state->platforms =', 'SelectWorldTiles('):
    assert forbidden not in helper
print('World settings/customisation reset wiring and native-progression guards passed')

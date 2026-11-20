"""Checks for selectable-row descriptions and their opt-in HUD presentation.

These source-level assertions complement the portable C++ algorithm tests.
Full in-game HUD rendering still requires the actual Win32 game.
"""
import re
from pathlib import Path

root = Path(__file__).resolve().parents[1]
menu = (root / 'src/menu.cpp').read_text(encoding='utf-8')
hud = (root / 'src/hud.cpp').read_text(encoding='utf-8')
state_h = (root / 'include/refunct/state.hpp').read_text(encoding='utf-8')
state_cpp = (root / 'src/state.cpp').read_text(encoding='utf-8')
storage = (root / 'src/storage.cpp').read_text(encoding='utf-8')
menu_h = (root / 'include/refunct/features/menu.hpp').read_text(encoding='utf-8')

assert 'extern bool g_show_menu_descriptions;' in state_h
assert 'bool g_show_menu_descriptions = false;' in state_cpp
assert 'ShowMenuDescriptions' in storage
assert 'GetPrivateProfileIntW(L"Display", L"ShowMenuDescriptions", 0' in storage
assert 'MenuSelectionDescription(Page page, int selected,' in menu_h
assert 'MenuSelectionDescription(' in hud and 'g_page, g_selection, menu_items)' in hud
assert 'g_show_menu_descriptions = !g_show_menu_descriptions;' in menu
assert 'g_show_menu_descriptions = false;' in menu  # Restore HUD defaults
assert 'std::wstring(L"Show option descriptions: ")' in menu
assert 'case Page::HudGameplay:' in menu

# Every page in the enum has its own description mapping (or a special dynamic one).
page_enum = re.search(r'enum class Page\s*\{(.*?)\};', state_h, re.S).group(1)
page_enum = re.sub(r'//[^\n]*', '', page_enum)
pages = [x.strip() for x in page_enum.split(',') if x.strip()]
desc_section = menu.split('std::wstring MenuSelectionDescription(', 1)[1].split('void GoBack()', 1)[0]
for page in pages:
    assert f'case Page::{page}:' in desc_section, f'Missing help for {page}'

# Fixed-page descriptions match their selectable non-Back rows exactly.
# Main has eight modules, Search and Hide; all other pages have an appended Hide row.
fixed_counts = {
    'Main': 12, 'ProfilesHub': 5, 'RestartOptions': 5, 'ConfirmLevelReload': 2,
    'Movement': 4, 'MovementModes': 3, 'MovementPhysics': 9,
    'MovementJump': 3, 'Persistence': 4, 'GameplayRecords': 7, 'Ghost': 7, 'GhostReplay': 10,
    'World': 13, 'WorldObjectRemoval': 0, 'WorldCubes': 6, 'Customisation': 17, 'Crosshair': 7, 'CustomisationShadows': 4, 'CustomisationPillars': 3, 'CustomisationSky': 5, 'CustomisationSkyColours': 8, 'CustomisationSkyVisibility': 3, 'CustomisationFogColours': 4, 'CustomisationFogVisibility': 2, 'CustomisationTime': 5, 'WorldWater': 6, 'WorldWaterColours': 8, 'WaterReflections': 7, 'WaterBeamReflections': 0, 'WorldJumpPads': 2, 'WorldJumpPadColours': 2, 'WorldJumpPadHeight': 3, 'WorldPlatforms': 2, 'WorldPlatformColours': 2, 'WorldPlatformSpeed': 3, 'WorldTubes': 2, 'WorldTubeColours': 2, 'WorldTubeSpeed': 3, 'WorldPlayer': 2, 'WorldSections': 8,
    'WorldSectionHeights': 5, 'WorldSectionPositionEdit': 4, 'WorldButtons': 3, 'WorldButtonActions': 10,
    'WorldButtonColours': 8, 'WorldButtonBeacons': 3,
    'WorldTiles': 3, 'WorldTileActions': 6, 'WorldTileColours': 5,
    'WorldWalls': 4, 'SaveStates': 5, 'SavePositions': 5,
    'TeleportCoordinates': 5, 'SavedCheckpoints': 3,
    'ButtonCheckpoints': 5, 'Practice': 3, 'Hud': 5, 'Splits': 4, 'HudKeyDisplay': 2,
    'HudTiming': 3, 'HudRecordTimes': 6,
    'SplitDisplay': 9, 'HudComparisons': 3, 'Settings': 3, 'Logs': 3,
}
# Both retired reflection pages share an empty help branch.
assert 'case Page::WaterObjectReflections: case Page::WaterBeamReflections:\n            return DescriptionAt(selected, {});' in desc_section
for page, expected in fixed_counts.items():
    block = desc_section.split(f'case Page::{page}:', 1)[1].split('case Page::', 1)[0]
    m = re.search(r'return DescriptionAt\(selected,\s*\{(.*?)\}\);', block, re.S)
    assert m, f'No fixed-row description list for {page}'
    found = len(re.findall(r'L"', m.group(1)))
    assert found == expected, f'{page}: {found} descriptions for {expected} rows'

for dynamic_page in (
    'WorldIndividualSections', 'WorldIndividualSectionHeights',
    'HudPositions', 'HudAppearance', 'HudAppearanceEdit', 'HudGameplay', 'HudKeyDisplay', 'Profiles', 'Hotkeys',
):
    assert f'case Page::{dynamic_page}:' in desc_section

# Generic navigation rows and invalid selections are handled safely.
assert 'static_cast<size_t>(selected) >= entries.size()' in desc_section
assert 'label == L"Back"' in desc_section
assert 'label.rfind(L"Hide menu", 0) == 0' in desc_section
assert 'kHudElementCount' in desc_section
assert 'kPracticeClusterCount' in desc_section
assert 'kActionCount' in desc_section

# Help panel grows for complex descriptions, leaves one-liners compact,
# caches text measurements, and shows an ellipsis on small viewports.
assert 'natural_menu_height = kMenuChromeHeight' in hud
assert 'wanted_help_height' in hud
assert 'kMenuHelpMinHeight' in hud
assert 'kMenuHelpMaxLines' in hud
assert 'available_menu_height - kMenuChromeHeight - help_height' in hud
assert 'kMenuChromeHeight + kMenuRowHeight * static_cast<float>(visible_rows) + help_height' in hud
assert 'description.find(L\' \', cursor)' in hud
assert 'menu_help_text_width' in hud
assert 'cached_help_description' in hud
assert 'description != cached_help_description' in hud
assert 'menu_help_text_width != cached_help_width' in hud
assert 'SELECTED OPTION' in hud
assert 'menu_help_lines.size()' in hud
assert 'drawn_lines < menu_help_lines.size()' in hud

print('Menu description mapping, persistence, bounds and layout checks passed')

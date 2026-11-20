"""Static regression checks for persisted HUD placement, not a Windows game test."""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
header = (root / 'include/refunct/state.hpp').read_text(encoding='utf-8')
state = (root / 'src/state.cpp').read_text(encoding='utf-8')
storage = (root / 'src/storage.cpp').read_text(encoding='utf-8')
menu = (root / 'src/menu.cpp').read_text(encoding='utf-8')
hud = (root / 'src/hud.cpp').read_text(encoding='utf-8')

keys_match = re.search(r'kHudPositionKeys\{\{(.*?)\}\};', header, re.S)
labels_match = re.search(r'kHudPositionLabels\{\{(.*?)\}\};', header, re.S)
defaults_match = re.search(r'kDefaultHudCorners\{\{(.*?)\}\};', header, re.S)
assert keys_match and labels_match and defaults_match
keys = re.findall(r'L"([^"]+)"', keys_match.group(1))
labels = re.findall(r'L"([^"]+)"', labels_match.group(1))
defaults = re.findall(r'HudCorner::(\w+)', defaults_match.group(1))
enum_match = re.search(r'enum class HudElement\s*:\s*size_t\s*\{(.*?)\};', header, re.S)
assert enum_match, 'missing HudElement enum'
enum_values = [item.strip() for item in enum_match.group(1).split(',')]
assert enum_values[-1] == 'Count'
assert len(keys) == len(labels) == len(defaults) == len(enum_values) - 1 == 22
assert len(set(keys)) == len(keys)
assert defaults[7:9] == ['TopRight', 'TopRight'], 'FPS/modules default to top-right'
assert defaults[11] == 'TopLeft', 'speed HUD defaults to top-left'
assert all(v == 'TopLeft' for v in defaults[12:16]), 'new record and completion HUD defaults'
assert defaults[16:18] == ['TopRight', 'TopRight'], 'cheat and customisation lists default top-right'
assert keys[12:16] == ['CompareTimePosition', 'BestSegmentTimePosition',
                           'PBSplitTimePosition', 'CompletionPercentPosition']
assert keys[16:18] == ['ActiveCheatsPosition', 'ActiveCustomisationsPosition']
assert keys[18:] == ['GameTimePosition', 'LocalTimePosition', 'ButtonNumberPosition', 'KeyDisplayPosition']
assert defaults[18:] == ['TopLeft', 'TopRight', 'TopLeft', 'BottomRight']
assert all(v == 'TopLeft' for v in defaults[:7]), 'legacy HUD defaults unchanged'
assert 'g_hud_corners = kDefaultHudCorners' in state
assert 'write(L"HudPositions", kHudPositionKeys[i]' in storage
assert 'GetPrivateProfileIntW(\n            L"HudPositions", kHudPositionKeys[i]' in storage
assert 'value >= 0 && value <= static_cast<int>(HudCorner::Center) ? value : fallback' in storage
assert 'TopRight = 0, TopLeft = 1, BottomRight = 2, BottomLeft = 3, Center = 4' in header
assert 'constexpr size_t kHudPlacementCount' in header
assert 'g_hud_corners = kDefaultHudCorners' in menu, 'reset display resets positions'
assert 'L"Customisations  >"' in menu
assert 'case Page::HudPositions:' in menu
assert 'case Page::SplitDisplay: case Page::HudComparisons:' in menu
assert 'case Page::HudPositions: case Page::HudAppearance:' in menu
assert 'SetPage(Page::Hud); break;' in menu and 'SetPage(Page::HudTiming); break;' in menu
assert 'CycleMenuPosition(key == VK_RIGHT ? +1 : -1)' in menu
assert 'g_menu_position = MenuPosition::TopRight' in menu
assert 'L"F5MenuPosition"' in storage
assert 'CycleHudCorner(s_hud_style_element, key == VK_RIGHT ? +1 : -1)' in menu
assert 'std::array<std::vector<HudLine>, kHudPlacementCount> corners' in hud
assert 'g_hud_corners[index]' in hud
for name in ('Timer', 'PBRun', 'SumOfBest', 'LastButtonSplit', 'Coordinates',
             'Cubes', 'Tiles', 'FPS', 'EnabledModules', 'PracticeStatus', 'RecordStatus',
             'Speed', 'CompareTime', 'BestSegmentTime', 'PBSplitTime', 'CompletionPercent', 'ActiveCheats', 'ActiveCustomisations', 'GameTime', 'LocalTime', 'ButtonNumber', 'KeyDisplay'):
    assert f'HudElement::{name}' in hud, f'{name} not positioned'
assert 'popup_center_y' in hud, 'split popup must keep original position'
assert 'g_show_split_fastest_delta' in hud, 'fastest comparison preserved'
assert 'g_show_enabled_modules' in hud
assert 'g_show_active_cheats' in hud
assert 'HudLine overflow' in hud, 'small-viewport overflow label required'
assert 'g_show_speed = !g_show_speed' in menu
assert 'g_show_speed = false' in menu
assert 'g_speed_units_per_second' in hud
assert 'L"ShowSpeed"' in storage
print('PASS: twenty-two independently saved HUD placements including Center; menu and popups unchanged')

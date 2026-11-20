"""HUD appearance settings: saved defaults, menus, navigation and isolation."""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
h = (root/'include/refunct/state.hpp').read_text('utf-8')
state = (root/'src/state.cpp').read_text('utf-8')
storage = (root/'src/storage.cpp').read_text('utf-8')
menu = (root/'src/menu.cpp').read_text('utf-8')
hud = (root/'src/hud.cpp').read_text('utf-8')

assert 'HudAppearance, HudAppearanceEdit' in h
assert 'kHudColourModeCount = 11' in h
assert 'kHudMinSizePercent = 50' in h
assert 'kHudMaxSizePercent = 200' in h
assert 'kHudSizeStepPercent = 10' in h
for v in ('g_hud_colour_modes','g_hud_custom_colours','g_hud_size_percent'):
    assert f'extern std::array' in h and v in h and v in state
    assert v in storage and v in menu
assert 'g_hud_colour_modes{};' in state  # old defaults unchanged
assert 'values.fill(100)' in state
assert 'values.fill(0xFFFFFF)' in state
assert 'L"HudAppearance"' in storage and 'L"HudPositions"' in storage
for suffix in ('ColourMode','CustomRGB','SizePercent'):
    assert f'L"{suffix}"' in storage
assert 'colour >= 0 && colour < kHudColourModeCount' in storage
assert 'size >= kHudMinSizePercent' in storage
assert 'size <= kHudMaxSizePercent' in storage
assert 'size % kHudSizeStepPercent == 0' in storage
assert 'rgb >= 0 && rgb <= 0xFFFFFF' in storage
assert 'write(L"HudPositions", kHudPositionKeys[i]' in storage

for page in ('HudAppearance', 'HudAppearanceEdit'):
    assert menu.count(f'case Page::{page}:') >= 4
assert 'L"Customisations  >"' in menu
assert 'L"Show header: "' in menu
assert 'L"Position: "' in menu
assert 'SetPage(Page::HudAppearance);' in menu
assert 'SetPage(Page::HudAppearanceEdit);' in menu
assert 'CycleHudColour(key == VK_RIGHT ? +1 : -1)' in menu
assert 'AdjustHudTextSize(key == VK_RIGHT ? +1 : -1)' in menu
assert 's_tile_hex_axis = 9' in menu
assert 'g_hud_custom_colours[s_hud_style_element] = hex;' in menu
assert 'g_hud_colour_modes[s_hud_style_element] = 10;' in menu
assert 's_tile_hex_input.size() != 6' in menu
assert 'kHudPositionLabels[i]' in menu
assert 'kHudPositionKeys[i]' in storage
assert 'g_hud_colour_modes.fill(0)' in menu
assert 'g_hud_custom_colours.fill(0xFFFFFFu)' in menu
assert 'g_hud_size_percent.fill(100)' in menu
assert 'g_hud_show_headers = kDefaultHudShowHeaders' in menu

assert 'styled_color(index, color)' in hud
assert 'scale * size_factor' in hud and 'advance * size_factor' in hud
assert 'original.a' in hud
assert 'detail_height_cache' in hud  # cached to avoid per-render allocations
assert 'used_detail_height += height' in hud
assert 'used_detail_height -= detail_heights[--shown_modules]' in hud
# Do not change the menu, popup, timer, cheat logic or saved PB records.
assert 'styled_color(' not in hud.split('if (g_show_splits &&',1)[1]
assert 'g_hud_colour_modes' not in (root/'src/records.cpp').read_text('utf-8')
assert 'g_hud_size_percent' not in (root/'src/records.cpp').read_text('utf-8')
print('PASS: 21 HUD colour/size preferences, backward-compatible settings and menu wiring')

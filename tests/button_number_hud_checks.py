"""Button-number HUD source-level regression checks (not in-game validation)."""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
h = (root/'include/refunct/state.hpp').read_text('utf-8')
s = (root/'src/state.cpp').read_text('utf-8')
storage = (root/'src/storage.cpp').read_text('utf-8')
menu = (root/'src/menu.cpp').read_text('utf-8')
hud = (root/'src/hud.cpp').read_text('utf-8')
records = (root/'src/records.cpp').read_text('utf-8')

# Existing element IDs and setting names stay in the same order.
enum = re.search(r'enum class HudElement : size_t \{(.*?)\};', h, re.S).group(1)
assert re.search(r'GameTime, LocalTime, ButtonNumber, KeyDisplay, Count', enum)
keys = re.search(r'kHudPositionKeys\{\{(.*?)\}\};', h, re.S).group(1)
assert 'L"LocalTimePosition",\n    L"ButtonNumberPosition"' in keys
assert 'L"Current button number"' in h
assert 'g_show_button_number = false' in s
assert 'g_show_button_number = false' in menu
assert 'L"ShowButtonNumber"' in storage
assert 'L"Display", L"ShowButtonNumber", 0' in storage
assert 'L"ButtonNumberPosition"' in h
assert 'case Page::HudGameplay:' in menu
assert 'L"Current button number"' in h
assert 'case HudElement::ButtonNumber: g_show_button_number = !g_show_button_number; break;' in menu
assert 'HudElementIsVisible(element) ? L"ON" : L"OFF"' in menu
assert 'g_hud_colour_modes[s_hud_style_element]' in menu and 'kHudElementCount' in menu
assert 'g_last_button_count' in hud and 'g_button_state_initialized' in hud
assert 'L"Current button: " + value + L"/"' in hud
assert 'std::to_wstring(kRefunctButtonCount)' in hud
assert 'add_line(HudElement::ButtonNumber' in hud
assert 'const int button_count = state->buttons;' in records
assert 'g_last_button_count = button_count;' in records
assert 'g_button_splits.back().button_number' not in hud.split('if (g_show_button_number)',1)[1].split('if (g_show_coordinates)',1)[0]
# Purely HUD: no changes to records, native contact or button fixing.
print('PASS: native button counter HUD, 0/37 progression, display persistence, position/style')

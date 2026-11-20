"""Key visualizer regression: saved/independent controls and read-only key state."""
from pathlib import Path
import re
r=Path(__file__).resolve().parents[1]
h=(r/'include/refunct/state.hpp').read_text('utf-8')
s=(r/'src/state.cpp').read_text('utf-8')
m=(r/'src/menu.cpp').read_text('utf-8')
d=(r/'src/storage.cpp').read_text('utf-8')
u=(r/'src/hud.cpp').read_text('utf-8')
assert 'ButtonNumber, KeyDisplay, Count' in h # append-only preserves old saved indices
assert 'L"KeyDisplayPosition"' in h
assert 'L"Key presses (WASD / Space / Shift)"' in h
assert 'g_show_key_display = false;' in s and 'extern bool g_show_key_display;' in h
assert 'L"ShowKeyDisplay"' in d
assert 'L"Display", L"ShowKeyDisplay", 0' in d
assert 'case HudElement::KeyDisplay: return g_show_key_display;' in m
assert 'case HudElement::KeyDisplay: g_show_key_display = !g_show_key_display; break;' in m
assert 'g_show_key_display = false;' in m
assert 'case Page::HudKeyDisplay:' in m
assert 'add_line(HudElement::KeyDisplay' in u
assert 'g_hud_show_headers[index]' in u
assert 'styled_color(index, color)' in u
assert 'size_factor' in u and 'g_hud_corners[index]' in u
for key in ("held('W')", "held('A')", "held('S')", "held('D')",
            'held(VK_SPACE)', 'held(VK_SHIFT)', 'held(VK_LSHIFT)', 'held(VK_RSHIFT)'):
    assert key in u,key
assert 'GetForegroundWindow()' in u and 'GetAsyncKeyState(virtual_key) & 0x8000' in u
assert 'g_draw_rect(hud, fill' in u
block=u.split('    if (g_show_key_display) {',1)[1].split('    if (g_show_coordinates)',1)[0]
assert 'MarkCheat' not in block and 'SaveSettings' not in block
# Change isolation: no gameplay or cheat recording code modified by the patch.
print('PASS: key display HUD controls, per-element appearance, persistence and safe polling')

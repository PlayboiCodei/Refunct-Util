"""Regression guard for explanatory HUD help and responsive, cached layout.

Static checks only; font placement must still be verified in Refunct Win32.
"""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
menu = (root / 'src/menu.cpp').read_text(encoding='utf-8')
hud = (root / 'src/hud.cpp').read_text(encoding='utf-8')

help_section = menu.split('std::wstring MenuSelectionDescription(', 1)[1].split(
    '// HUD reset remains display-only:', 1)[0]
texts = re.findall(r'L"(?:\\.|[^"\\])*"', help_section)
assert len(texts) > 240, 'Expected descriptions across the complete menu tree'
assert sum(text.count('.') >= 2 for text in texts) >= 150, (
    'Complex menu options should explain behaviour in multiple sentences'
)

for detail in (
    'negative = ahead, positive = behind PB',
    'green minus = time gained, red plus = time lost',
    'previous button and the latest button',
    '0.400 ahead to 0.440 ahead displays -0.040',
    'Automatic ghost detection',
    'If Remove Water is ON, it overrides this visual-only choice',
    'original cooked water-volume boundary does not move',
    'saves a faster eligible PB',
    'not necessarily a time you have completed in one run',
    'The centre option is available for HUD overlays',
):
    assert detail in help_section, f'Missing important help explanation: {detail}'

# Wrapping and spacing: dynamically adapt to length and viewport dimensions.
for pattern in (
    'constexpr size_t kMenuHelpMaxLines = 8;',
    'wanted_help_height',
    'std::max(kMenuHelpMinHeight,',
    'std::min(wanted_help_height, available_menu_height',
    'drawn_lines < menu_help_lines.size()',
    'kMenuHelpLineHeight',
    'g_show_menu_descriptions',
    'const float width = menu_width;',
):
    assert pattern in hud, f'Missing adaptive layout behaviour: {pattern}'

# Avoid the expensive per-frame series of font-size measurements.
assert 'static std::wstring cached_help_description;' in hud
assert 'static float cached_help_width = -1.0f;' in hud
assert 'description != cached_help_description' in hud
assert 'menu_help_text_width != cached_help_width' in hud
assert 'cached_help_description.clear();' in hud, 'Closing help must invalidate cache'

# The change remains opt-in. It must not force HUD descriptions to be visible.
assert 'g_show_menu_descriptions = !g_show_menu_descriptions;' in menu
assert 'g_show_menu_descriptions = false;' in menu
print('PASS: detailed menu help coverage, cached wrapping and adaptive panel layout')

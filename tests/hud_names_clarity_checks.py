"""HUD terminology only: validate clarity and unchanged persisted setting identities.

This is a source-level UI check, not proof of in-game rendering or an MSVC build.
"""
from pathlib import Path
import re

root=Path(__file__).resolve().parents[1]
menu=(root/'src/menu.cpp').read_text('utf-8')
hud=(root/'src/hud.cpp').read_text('utf-8')
state=(root/'include/refunct/state.hpp').read_text('utf-8')
storage=(root/'src/storage.cpp').read_text('utf-8')

# Distinguish signed overall deficit from the change across just one button.
for label in (
    'Total ahead/behind PB:',
    'Time gained/lost at button:',
    'Time since previous button:',
    'Button time vs fastest:',
    'Section time vs PB:',
    'Section time vs other profile:',
    'Total vs other profile:',
    'Full-run PB time:',
    'Sum of best segments:',
    'PB time at last button:',
    'Other profile saved time:',
):
    assert label in menu, f'Missing descriptive HUD setting: {label}'
for label in ('Game day/night clock', 'PC local clock'):
    assert label in state
for label in (
    'L"Total vs PB  "',
    'L"Time gained/lost here  "',
    'L"Run time at last button: "',
    'L"PC time: "',
    'L"Game clock: "',
):
    assert label in hud, f'Missing matching HUD label: {label}'

# Informative descriptions disambiguate sign and per-button vs cumulative.
assert 'negative = ahead, positive = behind PB' in menu
assert 'green minus = time gained, red plus = time lost' in menu
assert 'between the previous button and the latest button' in menu

# Persisted keys / enum ordering must not be renamed with displayed text.
match=re.search(r'constexpr std::array<const wchar_t\*, kHudElementCount> kHudPositionKeys\{\{(.*?)\}\};',state,re.S)
assert match and 'L"TimerPosition"' in match.group(1)
assert 'L"PBRunPosition"' in match.group(1)
assert 'L"LocalTimePosition"' in match.group(1)
for key in ('Timer','ButtonSplits','PBGainLossDisplay','SectionVsPBDisplay',
            'SectionVsFileDisplay','ShowGameTime','ShowLocalTime'):
    assert key in storage, f'Saved config key changed: {key}'
assert 'g_pb_gain_loss_display' in hud and 'g_pb_row_mode' in hud
print('PASS: HUD names explain scope/sign; existing settings and rendering switches retained')

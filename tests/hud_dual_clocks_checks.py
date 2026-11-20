"""Read-only in-game clock, real local clock, and independent HUD settings."""
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
state = (ROOT/'include/refunct/state.hpp').read_text()
def src(name): return (ROOT/'src'/name).read_text()
menu, hud, storage, time = (src(name) for name in
    ('menu.cpp','hud.cpp','storage.cpp','time_of_day.cpp'))
header = (ROOT/'include/refunct/features/time_of_day.hpp').read_text()
assert 'int GetLiveGameTimeMinutes();' in header
assert 'int GetLiveGameTimeMinutes() {' in time
assert 'if (!custom_enabled)' in time
assert 'if (!g_show_game_time_hud) { s_visible_minute = -1; return; }' in time
assert 'return; // Do not write CurrentMinute or TimeSpeed in Game Default.' in time
assert 's_restore_after_disable' in time
assert 'if (TimeSettings().enabled)' in time  # avoid showing HUD-only discovery popup
assert 'GetLiveGameTimeMinutes()' in hud
assert 'GetLocalTime(&local_time)' in hud
assert 'next_local_update = fps_now + 250' in hud
assert 'L"Game clock: "' in hud and 'L"PC time: "' in hud
for field in ['g_show_game_time_hud', 'g_show_local_time_hud']:
    assert f'extern bool {field}' in state
    assert f'bool {field} = false' in src('state.cpp')
    assert field in menu and field in hud and field in storage
for key in ['ShowGameTime', 'ShowLocalTime']:
    assert f'L"{key}"' in storage
for key in ['GameTimePosition', 'LocalTimePosition']:
    assert f'L"{key}"' in state
assert 'L"Game day/night clock"' in state
assert 'L"PC local clock"' in state
assert 'HudElement::GameTime' in menu and 'HudElement::LocalTime' in menu
assert 'g_show_game_time_hud = false;' in menu
assert 'g_show_local_time_hud = false;' in menu
assert 'UpdateCustomTimeOfDay(player);' in src('game_tick.cpp')
assert 'src/time_of_day.cpp' in (ROOT/'CMakeLists.txt').read_text()
print('PASS: two clocks, 24-hour readout, default-mode observation, settings and HUD positions')

"""Source-level checks for separate time anchor, lock, and native speed controls.

These checks are not a substitute for a Windows x86 ASI build or in-game test.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
code = (ROOT / 'src/time_of_day.cpp').read_text(encoding='utf-8')
menu = (ROOT / 'src/menu.cpp').read_text(encoding='utf-8')
api = (ROOT / 'include/refunct/features/time_of_day.hpp').read_text(encoding='utf-8')

for name in (
    'IsCustomTimeLocked', 'SetCustomTimeLocked',
    'GetCustomTimeSpeedPercent', 'SetCustomTimeSpeedPercent',
    'AdjustCustomTimeSpeedPercent',
):
    assert name in api and name in code, name

for value in (
    'L"Locked"', 'L"SpeedPercent"', 'kMinSpeedPercent = 25',
    'kMaxSpeedPercent = 1000', 'kSpeedStepPercent = 25',
    's_clock.original_speed', 's_clock.last_written_speed',
    's_pending_minute', 's_visible_minute',
):
    assert value in code, value

assert 'if (s_settings.locked || s_pending_minute)' in code
assert 'if (s_settings.locked) return 0.0;' in code
assert 's_settings.locked ? kLockedApplyIntervalMs : kApplyIntervalMs' in code
assert 's_clock.speed_owned' in code
assert 'WriteScalar(s_clock.object, s_clock.speed, s_clock.original_speed)' in code
assert 'std::abs(current - s_clock.last_minute) <= 0.5' in code
assert 'L"TimeSpeed"' in code

assert 'L"Lock Time: "' in menu
assert 'L"Time Speed: "' in menu
assert 'SetCustomTimeLocked(!IsCustomTimeLocked())' in menu
assert 'AdjustCustomTimeSpeedPercent(key == VK_RIGHT ? +1 : -1)' in menu
assert 'else if (i == 3) SetCustomTimeSpeedPercent(100);' in menu
assert 'else if (i == 4) SetCustomTimeOfDayEnabled(false);' in menu
assert 'L"ON freezes the day/night clock' in menu
assert 'L"Restore the original game time and day/night progression speed.' in menu
assert 'UpdateCustomTimeOfDay(player);' in (ROOT / 'src/game_tick.cpp').read_text(encoding='utf-8')
assert 'src/time_of_day.cpp' in (ROOT / 'CMakeLists.txt').read_text(encoding='utf-8')

print('Time lock, native speed multiplier, restore, and menu wiring: passed')

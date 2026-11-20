"""Contract tests for World > Sections > Section Speed.

Checks that the setting changes the native rise-speed field (not character
speed), survives settings persistence, and coexists with TAS fast-rise.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
def read(path):
    return (ROOT / path).read_text(encoding='utf-8')

state = read('include/refunct/state.hpp')
state_cpp = read('src/state.cpp')
sections = read('src/sections.cpp')
menu = read('src/menu.cpp')
storage = read('src/storage.cpp')
records = read('src/records.cpp')
tick = read('src/game_tick.cpp')
feature = read('include/refunct/features/practice.hpp')

assert 'WorldSectionSpeed' in state
assert 'extern int g_section_speed_percent;' in state
assert 'int g_section_speed_percent = 100;' in state_cpp
assert 'L"SectionSpeedPercent"' in storage
assert 'g_section_speed_percent = ReadUncappedPercentSetting(' in storage
assert 'case Page::WorldSectionSpeed:' in menu
assert 'SetPage(Page::WorldSectionSpeed)' in menu
assert 'ResetSectionSpeed()' in menu
assert 'AdjustSectionSpeed(key == VK_RIGHT ? +1 : -1)' in menu
assert 'L"World / Sections / Speed"' in menu
assert 'AdjustSectionSpeed(int direction)' in feature
assert 'void UpdateNativeSectionSpeed();' in feature
assert 'UpdateNativeSectionSpeed();' in tick
assert 'IsSectionSpeedModified()' in records
assert 'g_practice_speeds_overridden) return;' in sections
assert 'g_practice_speed_offset' in sections
assert 'g_practice_level_actors[i]' in sections
assert 'cached.original' in sections
assert 'cached.modified = instant_mode || g_section_speed_percent != 100;' in sections
assert 'g_section_speed_percent) / 100.0' in sections
assert 'g_section_speed_percent, 25)' in sections
assert 'g_recording_tainted = true;' in sections
assert 'g_run_pb_eligible = false;' in sections
assert 's_native_section_speed_next_ms = now + 100;' in sections
print('Section speed integration checks passed')

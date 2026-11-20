"""Regression checks for automatic section rises while animation is OFF.

The native BP_LevelRoot Speed acceleration is applied at the game tick; no
LevelState writes, synthetic presses or repeated actor-teleport loops.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
def read(path):
    return (ROOT / path).read_text(encoding='utf-8')

sections = read('src/sections.cpp')
menu = read('src/menu.cpp')
storage = read('src/storage.cpp')
state = read('src/state.cpp')
records = read('src/records.cpp')
tick = read('src/game_tick.cpp')

assert 'const bool instant_mode = !g_animate_section_changes;' in sections
assert 'constexpr float kInstantAutoRiseSpeed = 9000000.0f;' in sections
assert 'const double desired = instant_mode' in sections
assert 'if (!instant_mode && g_section_speed_percent == 100 && !pending_restore) return;' in sections
assert 'cached.modified = instant_mode || g_section_speed_percent != 100;' in sections
assert 'if (!g_pb_cheats_allowed_for_run) g_run_pb_eligible = false;' in sections
assert 'g_practice_speeds_overridden) return;' in sections
assert 's_native_section_speed_next_ms = now + 100;' in sections
assert 'UpdateNativeSectionSpeed();' in tick
assert 'L"World", L"AnimateSectionChanges", 1,' in storage
assert 'bool g_animate_section_changes = true;' in state
assert 'set(CheatBit::InstantRise, !g_animate_section_changes);' in records
assert 'automatic rises instant (assisted)' in menu
assert 'normal automatic rises' in menu
assert 'SetSectionRootLocation(g_practice_level_actors[i], current)' not in sections.split('void UpdateNativeSectionSpeed()',1)[1].split('// World > Buttons > Button Colours',1)[0]
print('Automatic instant section rise integration checks passed')

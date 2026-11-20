"""Source-level protection for native cube controls, menu routing and PB safety."""
from pathlib import Path
r = Path(__file__).resolve().parents[1]
c = (r/'src/cubes.cpp').read_text()
m = (r/'src/menu.cpp').read_text()
h = (r/'include/refunct/state.hpp').read_text()
rec = (r/'src/records.cpp').read_text()
cmake = (r/'CMakeLists.txt').read_text()
game = (r/'src/game_tick.cpp').read_text()
assert 'src/cubes.cpp' in cmake
assert 'WorldCubes' in h
for action in ('DisableAll','CollectAll','ShowAll','ShowAboveWater','RestoreDefault'):
    assert action in c
assert 'case Page::WorldCubes:' in m
assert 'UpdateWorldCubes(player);' in game
assert 'Collectibles' in c and 'BP_PowerCore_C' in c and 'kTotalCubes' in c
assert 'g_process_event(cube.actor, api.pickup' in c
assert 'BndEvt__Trigger_K2Node_ComponentBoundEvent_24_ComponentBeginOverlapSignature__DelegateSignature' in c
assert 'g_run_pb_eligible = false' in c
assert 'CheatBit::UsedCubes' in rec
for forbidden in ('state->cubes =', 'state->cubes++', 'state->cubes--', 'g_progress_counters.cubes ='):
    assert forbidden not in c
assert 'if (picked) return true;' in c
assert 'kWaterPlaneZ' in c
assert 'void ResetWorldCubeCache()' in c
print('PASS: native cube collection, visibility, menu, CMake, progress and PB guards')

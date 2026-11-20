"""Source-level plumbing/safety regression checks for World > Tubes.
The actual material names and shader compatibility must be tested in Refunct.
"""
from pathlib import Path
root = Path(__file__).resolve().parents[1]
menu = (root / 'src/menu.cpp').read_text()
tubes = (root / 'src/tubes.cpp').read_text()
header = (root / 'include/refunct/features/tubes.hpp').read_text()
tick = (root / 'src/game_tick.cpp').read_text()
cmake = (root / 'CMakeLists.txt').read_text()
state = (root / 'include/refunct/state.hpp').read_text()

assert 'WorldTubes, WorldTubeColours' in state
assert 'L"Tubes  >"' in menu and 'L"Tube Colours  >"' in menu
assert 'case Page::WorldTubes:' in menu
assert 'case Page::WorldTubeColours:' in menu
assert 'L"Customisation / Tube Colours"' in menu
assert 'SetPage(Page::WorldTubeSpeed)' in menu
assert 'SetPage(Page::WorldTubeColours)' in menu
assert 'SetTubeCustomColourEnabled(!IsTubeCustomColourEnabled())' in menu
assert 'SetTubeCustomColourHex(hex)' in menu
assert 'OpenTubeColourPicker()' in menu
assert 'UpdateTubeColours(player);' in tick
assert 'src/tubes.cpp' in cmake
assert 'refunct/features/tubes.hpp' in (root / 'include/refunct/internal_api.hpp').read_text()
for name in ('IsTubeCustomColourEnabled', 'SetTubeCustomColourEnabled',
             'GetTubeCustomColourHex', 'SetTubeCustomColourHex',
             'OpenTubeColourPicker', 'UpdateTubeColours'):
    assert name in header and name in tubes
assert 'L"TubeColours", L"Hex"' in tubes
assert 'L"TubeColours", L"CustomEnabled"' in tubes
assert 'IsLiveStaticMeshComponent' in tubes
assert 'TubeIdentity(item.object, name)' in tubes
assert 'FindClassPropertyOffset(object->class_ptr, L"StaticMesh")' in tubes
assert 'TintParameter(source, parameter)' in tubes
assert 'CreateMID(target.component, source)' in tubes
assert 'ValidTarget(target, objects)' in tubes
assert 'g_restart_requested || g_full_level_reload_requested' in tubes
assert 'SetMaterial(target.component, target.source)' in tubes
assert 'kPerTick = 2048' in tubes
assert 'ColourWorkBudget scan_budget(800)' in tubes
assert 'const size_t batch = s_pending_apply ? 5 : 4;' in tubes
assert 'no live tube/pipe StaticMeshComponent identified' in tubes
print('Tube colour menu, persistence, native guards, discovery and restoration checks passed')

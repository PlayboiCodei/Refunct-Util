"""Pillar colours: exact cooked-asset targeting and menu/build lifecycle checks.

Source-level regression only. Requires a real Win32 Refunct v9.30 run to
confirm all background meshes accept the MID and render the desired tint.
"""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
source = (root/'src/pillars.cpp').read_text(encoding='utf-8')
menu = (root/'src/menu.cpp').read_text(encoding='utf-8')
state = (root/'include/refunct/state.hpp').read_text(encoding='utf-8')
records = (root/'src/records.cpp').read_text(encoding='utf-8')
cmake = (root/'CMakeLists.txt').read_text(encoding='utf-8')
game_tick = (root/'src/game_tick.cpp').read_text(encoding='utf-8')
api = (root/'include/refunct/internal_api.hpp').read_text(encoding='utf-8')

assert 'src/pillars.cpp' in cmake and 'src/fog.cpp' in cmake
assert '#include "refunct/features/pillars.hpp"' in api
assert 'UpdatePillarColours(player);' in game_tick
assert 'BP_BuildingDistributor_C' in source
assert 'SM_Block_1000x1000x20000' in source
assert 'M_Concrete_Rough_Bisque' in source and 'M_Concrete_Rough_Brown' in source
assert 'ReadReflectedObject(object, L"StaticMesh")' in source
assert 'ReadReflectedObject(current, L"Parent")' in source
assert 'ObjectName(p->object.class_ptr) == type' in source, 'Validate UProperty class, not its metaclass'
assert 'values.count != 1' in source and 'UnrealName(out) == L"Color"' in source
assert 'CreateDynamicMaterialInstance' in source and 'SetVectorParameterValue' in source
assert 'GetMaterial' in source and 'SetMaterial' in source
assert 'GetMaterial(mesh) == material' in source
assert 'objects->objects[target.index].serial_number == target.serial' in source
assert 'ColourWorkBudget budget(500)' in source
assert 'ColourWorkBudget budget(850)' in source
assert 'if (!need_discovery && s_targets.empty()) return;' in source
assert 's_discovery_complete = true;' in source and 'kRescanMs' not in source
assert 'target.mid = nullptr;' in source and 'SetMaterial(target.mesh, target.source)' in source
assert 'QueueSettingsValue(L"PillarColours", L"CustomEnabled"' in source
assert 'SetPillarColourHex(result)' in source
assert 'IsPillarColourEnabled' in records and 'CheatMask(CheatBit::PillarColour)' in records
assert 'add(CheatBit::PillarColour, L"Pillar colours")' in records
assert 'SetPillarColourEnabled(false)' in menu
assert 'Page::CustomisationPillars' in menu and 'CustomisationPillars' in state
assert 'PillarColourMethodLabel()' in menu and 'OpenPillarColourMethod()' in menu
assert 's_tile_hex_axis == 15' in menu and 'SetPillarColourHex(hex)' in menu
assert 's_pillar_use_windows_picker = !s_pillar_use_windows_picker' in menu
assert 'raw <= static_cast<int>(Page::Crosshair)' in menu
assert 'else if (i == 11) SetPage(Page::CustomisationPillars)' in menu
assert 'else if (i == 16) ResetWorldCustomisation()' in menu

# The old page IDs must not move when adding a new page.
enum = state.split('enum class Page {',1)[1].split('};',1)[0]
assert 'Search, CustomisationSky,\n    CustomisationPillars' in enum
# The filter must be concrete-only and must never mutate score eligibility.
assert 'g_recording_tainted' not in source
assert 'g_run_pb_eligible' not in source
print('PASS: cooked pillar asset targeting, native material, menu, persistence, restore and PB isolation')

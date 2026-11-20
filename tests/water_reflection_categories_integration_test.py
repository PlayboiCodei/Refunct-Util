"""Reflection categories stay independent, saved, and capture-only.

This is source-level integration in addition to the compiled native mocks;
actual visuals still require the pinned Refunct Win32 build.
"""
from pathlib import Path
root = Path(__file__).resolve().parents[1]
read = lambda name: (root / name).read_text(encoding='utf-8')
water = read('src/water.cpp')
menu = read('src/menu.cpp')
header = read('include/refunct/features/water.hpp')
state = read('include/refunct/state.hpp')
profiles = read('src/customisation_profiles.cpp')
records = read('src/records.cpp')
for item in ('Walls', 'Buttons', 'Pipes', 'Beacons', 'JumpPads', 'Cubes'):
    assert f'AreWaterReflected{item}Visible()' in header
    assert f'SetWaterReflected{item}Visible(bool visible)' in header
    assert f'AreWaterReflected{item}Visible()' in menu

for prop in ('ReflectionGeometryVisible', 'ReflectionButtonsVisible',
             'ReflectionPipesVisible', 'ReflectionBeaconsVisible',
             'ReflectionJumpPadsVisible', 'ReflectionCubesVisible'):
    assert f'L"{prop}", 1, g_settings_path.c_str()' in water, prop
    assert f'write({prop}, L"{prop}"' in water, prop
    profile_key = 'WaterReflectionGeometryVisible' if prop == 'ReflectionGeometryVisible' else f'Water{prop}'
    assert f'add("{profile_key}"' in profiles
    assert f'changed("{profile_key}")' in profiles
    assert profile_key not in records

for prop in ('WaterReflectionButtonsVisible', 'WaterReflectionPipesVisible',
             'WaterReflectionBeaconsVisible', 'WaterReflectionJumpPadsVisible',
             'WaterReflectionCubesVisible'):
    assert f'values.contains(name)' in profiles
    assert f'"{prop}"' in profiles

assert 'WaterObjectReflections, WaterBeamReflections' in state
assert 'L"Water Object Reflections  >"' not in menu
assert 'L"Water Beam Reflections  >"' not in menu
for label in ('Reflect walls/platforms', 'Reflect buttons', 'Reflect pipes',
              'Reflect button beacon beams', 'Reflect jump pads', 'Reflect cubes'):
    assert label in menu
for cls in ('BP_IslandChunk_C', 'BP_Lift_C', 'BP_Mover_C', 'BP_Button_C', 'BP_TravelPipe_C', 'BP_Jumppad_C', 'BP_PowerCore_C'):
    assert f'L"{cls}"' in water
assert 'ReadReflectedObject(item.object, L"Beacon")' in water
assert 'ReadReflectedObject(item.object, L"Mover")' in water
assert 'ReadReflectedObject(item.object, L"Mesh")' in water
for index, name in enumerate(('Walls', 'Buttons', 'Pipes', 'JumpPads', 'Cubes', 'Beacons'), 1):
    assert f'case {index}: SetWaterReflected{name}Visible(!AreWaterReflected{name}Visible()); return true;' in menu
    assert f'if (!AreWaterReflected{name}Visible()) SetWaterReflected{name}Visible(true);' in menu
assert 'ReadReflectedObject(item.object, L"Button base")' in water
assert 's_geometry_applied_mask != hide_mask' in water
assert 'WaterRestoreGeometryReflections(objects)' in water
assert '(!wanted || !geometry_wanted || s_reflection_restore_pending)' in water
assert 'wanted && (s_reflection_restore_pending || !current)' in water
assert 'ColourWorkBudget budget(450)' in water
assert 'ColourWorkBudget apply_budget(450)' in water
assert 'L"HideComponent"' in water
assert 's_geometry_hidden' in water
assert 'WaterReflectionCategoryChanged' in water
for forbidden in ('WaterReflectionShadowsVisible', 'SetWaterReflectedShadowsVisible',
                  'WaterDiagnoseCaptureFlagEntries'):
    assert forbidden not in water
assert 'MarkCheatAction' not in water
print('PASS: water reflection categories, startup, profiles, menu and capture safety')

# Dedicated menu, with colour settings unaffected.
def block(code, page):
    return code.split(f'case Page::{page}:',1)[1].split('case Page::',1)[0]
render = menu.split('std::vector<std::wstring> BuildEntries() {',1)[1].split('// Show the current location',1)[0]
assert 'L"Water Reflections  >"' in block(render, 'Customisation')
assert 'All water reflections' not in block(render, 'WorldWaterColours')
assert 'All water reflections' in block(render,'WaterReflections')
assert 'raw <= static_cast<int>(Page::Crosshair)' in menu
assert 'case Page::WaterReflections:\n            SetPage(Page::Customisation); break;' in menu
assert 'case Page::WaterObjectReflections: case Page::WaterBeamReflections:\n            SetPage(Page::WaterReflections); break;' in menu
assert 'else if (i == 13) SetPage(Page::WaterReflections);' in menu
assert 'case Page::WaterReflections:\n            ToggleWaterReflectionRow(g_selection);' in menu
assert 'case Page::WaterReflections:\n            if (!ToggleWaterReflectionRow(i)) GoBack();' in menu

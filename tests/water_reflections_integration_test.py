"""Water-only planar-reflection toggle: menu, persistence, migration and safety.

Static/runtime source integration checks; Windows x86 and in-game still required.
"""
from pathlib import Path
root = Path(__file__).resolve().parents[1]
read = lambda path: (root / path).read_text(encoding='utf8')
water = read('src/water.cpp')
menu = read('src/menu.cpp')
api = read('include/refunct/features/water.hpp')
profiles = read('src/customisation_profiles.cpp')
record = read('src/records.cpp')

for name in ('AreWaterReflectionsVisible', 'SetWaterReflectionsVisible',
             'AreWaterReflectedWallsVisible', 'SetWaterReflectedWallsVisible'):
    assert f'{name}(' in api and f'{name}(' in water

# Reflection switch is not the existing directional-shadow or tint switch.
assert 'WaterUpdateReflections(player);' in water
assert 'WaterGetVisible(it->component, current)' in water
assert 'WaterSetVisible(it->component, true)' in water
assert 'WaterIsA(candidate, L"PlanarReflectionComponent")' in water
assert 'WaterIsA(owner, L"PlanarReflection")' in water
assert 'WaterReflectionTargetValid(*it, objects)' in water
assert 'serial_number != target.serial' in water
assert 'ColourWorkBudget budget(450)' in water
assert 'kReflectionScanBatch = 768' in water
assert 's_reflection_scan_complete' in water
assert 's_reflection_targets.empty() && s_reflection_scan_complete' in water
assert 's_reflection_next_verify = now + 1000' in water
assert 'WaterSetVisible(it->component, true)' in water
assert 's_reflection_restore_pending' in water
assert 's_reflection_targets.back().modified = true;' in water
assert 'if (s_reflection_player != player)' in water
assert 'return WaterObjectName(owner).find(L"Default__") != 0' in water

# Save file persists with existing visual WaterColours, defaulting to original ON.
assert 'L"WaterColours", L"ReflectionsVisible", 1' in water
assert 'write(ReflectionsVisible, L"ReflectionsVisible"' in water
assert 'QueueWaterConfigSave(ReflectionsVisible)' in water

page = menu.split('case Page::WaterReflections:', 1)[1].split('case Page::', 1)[0]
assert 'L"All water reflections: "' in page
assert 'L"Diagnose water reflected shadows (log)"' not in page
assert 'L"Water reflected shadows: "' not in page
assert 'case Page::WorldWaterColours:' in menu
assert 'case 0: SetWaterReflectionsVisible(!AreWaterReflectionsVisible()); return true;' in menu
assert 'if (!ToggleWaterReflectionRow(i)) GoBack();' in menu
assert 'ToggleWaterReflectionRow(g_selection);' in menu
assert 'SetPage(Page::WaterObjectReflections);' not in menu
assert 'SetPage(Page::WaterBeamReflections);' not in menu
assert 'case Page::WaterObjectReflections:' in menu
assert 'case Page::WaterBeamReflections:' in menu
assert 'WaterReflectionHideMask()' in water
assert 'g_selection == 0' in menu and 'SetWaterReflectionsVisible(!AreWaterReflectionsVisible());' in menu
assert 'if (!AreWaterReflectionsVisible()) SetWaterReflectionsVisible(true);' in menu

# Profiles created before this feature migrate safely rather than being rejected.
assert 'add("WaterReflectionsVisible", Flag(AreWaterReflectionsVisible()))' in profiles
assert 'values.contains("WaterReflectionsVisible")' in profiles
assert 'values.emplace("WaterReflectionsVisible", 1)' in profiles
assert 'SetWaterReflectionsVisible(value("WaterReflectionsVisible") != 0)' in profiles
assert 'Values expected = Capture();' in profiles

# No cheat/recording interaction introduced.
new = water.split('// Apply only to live, map-owned native PlanarReflection components.', 1)[1].split('// Only identify effect components',1)[0]
for prohibited in ('g_run_pb_eligible', 'g_recording_tainted', 'MarkCheatAction',
                   'SetCastShadows', 'r.SSR', 'SetScalarParameterValue', 'WriteProcessMemory',
                   'SetWorldLocation', 'GetAsyncKeyState'):
    assert prohibited not in new
assert 'WaterReflectionsVisible' not in record
assert 'WaterReflectionShadowsVisible' not in water
assert 'WaterSetCaptureDynamicShadows' not in water
assert 'WaterDiagnoseCaptureAPI' not in water
print('PASS: water reflection source integration, safety, profiles and logging')

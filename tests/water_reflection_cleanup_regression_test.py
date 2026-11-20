"""Regression for removal of temporary water diagnostics and ineffective capture shadows.

Source integration only; actual Win32 x86 build and gameplay validation remain separate.
"""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
menu = (root / 'src/menu.cpp').read_text(encoding='utf-8')
water = (root / 'src/water.cpp').read_text(encoding='utf-8')
profiles = (root / 'src/customisation_profiles.cpp').read_text(encoding='utf-8')
water_api = (root / 'include/refunct/features/water.hpp').read_text(encoding='utf-8')

# The effective water controls are exposed on both input paths without off-by-one
# errors when removing the obsolete rows.
rows = menu.split('case Page::WaterReflections:\n            return {', 1)[1].split('\n            };', 1)[0]
assert rows.count('L"All water reflections: "') == 1
assert rows.count('L"Water Object Reflections  >"') == 0
assert rows.count('L"Water Beam Reflections  >"') == 0
assert 'reflected shadows' not in rows.lower()
assert 'diagnos' not in rows.lower()
assert 'case 0: SetWaterReflectionsVisible(!AreWaterReflectionsVisible()); return true;' in menu
assert 'if (!ToggleWaterReflectionRow(i)) GoBack();' in menu
assert 'ToggleWaterReflectionRow(g_selection);' in menu
assert 'SetPage(Page::WaterObjectReflections);' not in menu
assert 'case Page::WaterObjectReflections:' in menu
assert 'case Page::WaterBeamReflections:' in menu
assert 'SetWaterReflectedWallsVisible(!AreWaterReflectedWallsVisible());' in menu
assert 'if (!AreWaterReflectedWallsVisible()) SetWaterReflectedWallsVisible(true);' in menu

for obsolete in ('DiagnoseWaterReflections', 'WaterDiagnoseCaptureAPI',
                 'WaterDiagnoseCaptureFlagEntries', 'WaterSetCaptureDynamicShadows',
                 'WaterDiagnoseOceanMaterial', 'AreWaterReflectedShadowsVisible',
                 'SetWaterReflectedShadowsVisible', 's_reflection_diagnostic',
                 'shadow_mode_applied', 'reflection_shadows_visible'):
    assert obsolete not in water, obsolete
    assert obsolete not in water_api, obsolete
    assert obsolete not in menu, obsolete

# The old saved setting is ignored. Profiles containing its old field are still
# accepted and their next save writes the new, supported schema.
assert 'values.erase("WaterReflectionShadowsVisible");' in profiles
assert 'add("WaterReflectionShadowsVisible"' not in profiles
assert 'SetWaterReflectedShadowsVisible(' not in profiles
assert 'add("WaterReflectionGeometryVisible", Flag(AreWaterReflectedWallsVisible()))' in profiles
assert 'SetWaterReflectedWallsVisible(value("WaterReflectionGeometryVisible") != 0)' in profiles

# Working reflection and capture-only geometry exclusion remain intact.
for required in ('WaterUpdateGeometryReflections(', 'WaterRestoreGeometryReflections(',
                 'WaterGeometryHiddenListMatches(', 'WaterReadCaptureHiddenList(',
                 'FindReflectedFunction(s_geometry_capture, L"HideComponent")',
                 'ColourWorkBudget apply_budget(450)',
                 'WaterReflectionCategoryChanged(ReflectionGeometryVisible',
                 'QueueWaterConfigSave(ReflectionsVisible)',
                 'WaterWakePlanarReflection('):
    assert required in water, required

print('PASS: reflection diagnostics and unused shadow control removed; working geometry preserved')

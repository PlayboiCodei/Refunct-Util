"""Old reflection experiments stay removed; new water-only capture controls stay isolated."""
from pathlib import Path
root = Path(__file__).resolve().parents[1]
menu = (root / 'src/menu.cpp').read_text()
shadow = (root / 'src/shadows.cpp').read_text()
water = (root / 'src/water.cpp').read_text()
header = (root / 'include/refunct/features/shadows.hpp').read_text()
water_header = (root / 'include/refunct/features/water.hpp').read_text()
records = (root / 'src/records.cpp').read_text()

# The newer water-only planar capture toggle is intentional. It must not
# reintroduce the old global shadow/SSR hacks into unrelated modules.
for source in (shadow, header, records):
    assert 'WaterReflections' not in source
    assert 'water_reflections' not in source
    assert 'Water reflections visible' not in source
    assert 'screen-space reflection' not in source
assert 'SetWaterReflectionsVisible' in menu
assert 'WaterUpdateReflections' in water
for source in (menu, water, water_header):
    assert 'WaterMaterialDiagnostic' not in source
    assert 'water material diagnostic' not in source.lower()
    assert 'Diagnose water materials' not in source
assert 'WaterUpdateMaterialDiagnostic' not in water
assert 'RequestWaterMaterialDiagnostic' not in menu + water + water_header

entries = menu.split('case Page::CustomisationShadows:\n            return {', 1)[1].split('};', 1)[0]
for entry in ('Shadows visible:', 'Shadow ambient colour:', 'ShadowAmbientMethodLabel()', 'Restore default shadows', 'Back'):
    assert entry in entries
assert 'Water reflections visible' not in entries
assert 'Diagnose water materials' not in entries
activation = menu.split('case Page::CustomisationShadows:\n            if (i == 0)', 1)[1].split('break;', 1)[0]
assert 'else if (i == 1) SetShadowAmbientColourEnabled(!IsShadowAmbientColourEnabled());' in activation
assert 'else if (i == 2) OpenShadowAmbientMethod();' in activation
assert 'else if (i == 3) ResetShadowCustomisation();' in activation
assert 'SetShadowsVisible(!AreShadowsVisible());' in activation
assert 'g_selection == 1) {\n                SetShadowAmbientColourEnabled(' in menu
assert 'g_selection == 2) {\n                s_shadow_use_windows_picker' in menu
assert 'SetShadowAmbientColourHex(hex)' in menu
assert 'UpdateShadowCustomisation(player);' in (root/'src/game_tick.cpp').read_text()
assert 'CastShadows' in shadow and 'AmbientCubemapTint' in shadow
assert 'WaterGetMaterial' in water and 'base_color' in water
assert 'add(IsShadowAmbientColourEnabled(), L"Shadow ambient colour")' in records
assert 'add(!AreShadowsVisible(), L"Directional shadows hidden")' in records
print('Water reflection and diagnostic cleanup validated; core features preserved')

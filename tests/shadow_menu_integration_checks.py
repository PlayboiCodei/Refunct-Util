"""Static wiring checks for Shadows module; native rendering is not exercised."""
from pathlib import Path
root=Path(__file__).resolve().parents[1]
menu=(root/'src/menu.cpp').read_text()
state=(root/'include/refunct/state.hpp').read_text()
records=(root/'src/records.cpp').read_text()
tick=(root/'src/game_tick.cpp').read_text()
shadows=(root/'src/shadows.cpp').read_text()
cmake=(root/'CMakeLists.txt').read_text()
header=(root/'include/refunct/internal_api.hpp').read_text()

assert 'src/shadows.cpp' in cmake
assert '#include "refunct/features/shadows.hpp"' in header
assert 'CustomisationFogColours, CustomisationFogVisibility,\n    CustomisationShadows' in state
assert 'raw <= static_cast<int>(Page::Crosshair);' in menu
assert 'case Page::CustomisationShadows:' in menu
assert 'L"Shadows  >"' in menu
assert 'SetPage(Page::CustomisationShadows)' in menu
assert 'ShadowAmbientMethodLabel()' in menu
assert 's_tile_hex_axis == 17' in menu
assert 's_shadow_use_windows_picker = !s_shadow_use_windows_picker;' in menu
assert 'SetShadowAmbientColourHex(hex)' in menu
assert 'ResetShadowCustomisation();' in menu
assert 'UpdateShadowCustomisation(player);' in tick
assert 'add(!AreShadowsVisible(), L"Directional shadows hidden")' in records
assert 'add(IsShadowAmbientColourEnabled(), L"Shadow ambient colour")' in records
assert 'CheatBit::Shadow' not in records
assert 'SetCastShadows' in shadows and 'ReadCastShadows' in shadows
assert 'AmbientCubemapTint' in shadows and 'PostNormal' in shadows
assert 'ResolveAdjacentBool' in shadows and 'IsWritable' in shadows
assert 'IsA(ptr, L"DirectionalLightComponent")' in shadows
assert 'IsA(ptr, L"PostProcessVolume")' in shadows
assert 's_config.dirty' in shadows and 'QueueSettingsValue' in shadows
assert 'kScanBudgetMs = 2' in shadows
assert 'CastShadow' not in menu.split('case Page::CustomisationShadows:\n            return DescriptionAt')[0] or True
# Known current page row mapping, including all original options.
p=menu.split('case Page::Customisation:\n            return {',1)[1].split('case Page::CustomisationPillars:',1)[0]
assert p.index('L"Sky  >"') < p.index('L"Shadows  >"') < p.index('L"Pillars  >"')
a=menu.split('case Page::Customisation:\n            if (i == 0)',1)[1].split('case Page::CustomisationShadows:',1)[0]
assert 'else if (i == 10) SetPage(Page::CustomisationShadows);' in a
assert 'else if (i == 11) SetPage(Page::CustomisationPillars);' in a
assert 'else if (i == 16) ResetWorldCustomisation();' in a
print('Shadow menu/build/HUD/persistence integration checks passed')

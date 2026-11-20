"""Static integration regression for Refunct v9.30 native Exponential Height Fog.

The WorldBase.umap in the supplied cooked PAK contains an ExponentialHeightFog
actor, HeightFogComponent0, FogInscatteringColor and
DirectionalInscatteringColor. This test does not assert in-game rendering.
"""
from pathlib import Path
root = Path(__file__).resolve().parents[1]
fog = (root / 'src/fog.cpp').read_text()
menu = (root / 'src/menu.cpp').read_text()
header = (root / 'include/refunct/features/fog.hpp').read_text()
game = (root / 'src/game_tick.cpp').read_text()
assert 'enum class FogRegion : uint8_t { General = 0, Directional = 1 };' in header
assert 'FogInscatteringColor' in fog
assert 'DirectionalInscatteringColor' in fog
assert 'SetFogInscatteringColor' in fog
assert 'SetDirectionalInscatteringColor' in fog
assert 'HeightFogComponent0' in fog
assert 'ExponentialHeightFogComponent' in fog
assert 's_target.component_serial' in fog
assert 's_target.actor_serial' in fog
assert 'SetterColourArgument' in fog
assert 'FindClassPropertyOffset' in fog
assert 'SetFogColourHex(static_cast<FogRegion>(s_picker_region.load()), result)' in fog
assert 'ReadColour(value, current)' in fog
assert 'WriteColour(value, desired)' in fog
assert 'WriteColour(value, value.original)' in fog
assert 'IsReadable(' in fog
assert 's_settings.enabled[i] || !s_settings.visible[i] ||' in fog
assert 'QueueSettingsValue' in fog
assert 'MarkCheatAction(' not in fog
assert 'g_run_pb_eligible' not in fog
assert 'src/fog.cpp' in (root / 'CMakeLists.txt').read_text()
assert '#include "refunct/features/fog.hpp"' in (root / 'include/refunct/internal_api.hpp').read_text()
assert game.index('UpdateSkyColours(player);') < game.index('UpdateFogColours(player);')
for fragment in (
    'FogColourMethodLabel(FogRegion::General)',
    'FogColourMethodLabel(FogRegion::Directional)',
    'OpenFogColourMethod(FogRegion::General)',
    'OpenFogColourMethod(FogRegion::Directional)',
    's_tile_hex_axis == 13',
    's_tile_hex_axis == 14',
    'SetFogColourHex(FogRegion::General, hex)',
    'SetFogColourHex(FogRegion::Directional, hex)',
    'ResetFogColours();',
    'else if (i == 4) {',
    'ResetSkyDiscVisibility(); ResetSkyEffectVisibility();',
):
    assert fragment in menu, fragment
assert menu.count('ResetFogColours();') == 2
assert menu.count('case Page::CustomisationSky:') >= 3
assert menu.count('case Page::CustomisationFogColours:') >= 3
print('Fog colours integration: verified source contracts')

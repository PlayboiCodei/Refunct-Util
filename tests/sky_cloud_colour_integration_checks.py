"""Cloud colour is a distinct native sky MID vector, not global sky tint.

Compiled smoke tests are separate; this checks menu selection, input and persistence
routings all agree with the same SkyRegion enum. Cooked parameter verified from
M_Sky_Panning_Clouds2.uasset in the supplied PAK (do not ship extracted assets).
"""
from pathlib import Path
r=Path(__file__).resolve().parents[1]
sky=(r/'src/sky.cpp').read_text()
menu=(r/'src/menu.cpp').read_text()
records=(r/'src/records.cpp').read_text()
header=(r/'include/refunct/features/sky.hpp').read_text()
assert 'enum class SkyRegion : uint8_t { Zenith = 0, Horizon = 1, Sun = 2, Clouds = 3 };' in header
for part in (
    'constexpr size_t kSkyRegionCount = 4;',
    'L"Zenith Color", L"Horizon color", L"Sun color", L"Cloud color"',
    'L"ZenithEnabled", L"HorizonEnabled", L"SunEnabled", L"CloudEnabled"',
    'L"ZenithHex", L"HorizonHex", L"SunHex", L"CloudHex"',
    'SetSkyColourEnabled(SkyRegion::Clouds, false);',
    'SetSkyColourHex(static_cast<SkyRegion>(s_picker_region.load()), picked);',
    'ResolveName(value, s_target.mid, kParameterNames[i], i)',
    'SetMIDColor(s_target.mid, value.name, desired)',
    'SetMIDColor(s_target.mid, value.name, value.original)',
    'QueueSettingsValue(kSection, kEnabledKeys[i]',
): assert part in sky,part
for part in (
    'SkyColourMethodLabel(SkyRegion::Clouds)',
    'OpenSkyColourMethod(SkyRegion::Clouds)',
    'SetSkyColourHex(SkyRegion::Clouds, hex)',
    's_tile_hex_axis == 16',
    's_sky_use_windows_picker[3] = !s_sky_use_windows_picker[3];',
    'g_selection == 7)', 'g_selection == 8)',
    'SkyHexInputAxis(region)',
    'Cloud colour:', 'Clouds visible:', 'Stars visible:',
): assert part in menu,part
assert menu.index('L"Cloud colour: "') < menu.index('L"Clouds visible: "')
assert 'add(IsSkyColourEnabled(SkyRegion::Clouds), L"Sky cloud colour")' in records
assert 'Stars colour' not in records and 'SkyRegion::Stars' not in header
assert 'MarkCheatAction(' not in sky
print('Cloud colour settings/menu/HUD/visibility independence checks passed')

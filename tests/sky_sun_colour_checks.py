"""Static regression checks for the Sun colour integration.

The source was mapped against the supplied Refunct cooked PAK:
M_Sky_Panning_Clouds2 includes vector parameter "Sun color", and
BP_DynamicSky references C_SunColor. These checks cannot prove in-game output.
"""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
menu = (root / 'src/menu.cpp').read_text()
sky = (root / 'src/sky.cpp').read_text()
header = (root / 'include/refunct/features/sky.hpp').read_text()

assert 'enum class SkyRegion : uint8_t { Zenith = 0, Horizon = 1, Sun = 2, Clouds = 3 };' in header
assert 'L"Zenith Color", L"Horizon color", L"Sun color"' in sky
assert 'constexpr size_t kSkyRegionCount = 4;' in sky
assert 'L"ZenithEnabled", L"HorizonEnabled", L"SunEnabled"' in sky
assert 'L"ZenithHex", L"HorizonHex", L"SunHex"' in sky
assert 's_target.values[i].modified' in sky
assert 'SetSkyColourEnabled(SkyRegion::Sun, false);' in sky
assert 'SetSkyColourHex(static_cast<SkyRegion>(s_picker_region.load()), picked);' in sky
assert 'ResolveName(value, s_target.mid, kParameterNames[i], i)' in sky
assert 'SetMIDColor(s_target.mid, value.name, desired)' in sky
assert 'SetMIDColor(s_target.mid, value.name, value.original)' in sky
assert 'SkyColourMethodLabel(SkyRegion::Sun)' in menu
assert 'OpenSkyColourMethod(SkyRegion::Sun)' in menu
assert 'SetSkyColourHex(SkyRegion::Sun, hex)' in menu
assert 's_tile_hex_axis == 12' in menu
assert 's_sky_use_windows_picker[2] = !s_sky_use_windows_picker[2];' in menu
assert 'case Page::CustomisationSkyColours:' in menu
assert 'else if (i == 4) {' in menu  # Sky / Restore all defaults
assert 'Sky sun:' in menu
assert 'not world lighting or shadows' in menu
assert 'MarkCheatAction(' not in sky
print('Sun colour controls, saved settings, material parameter and reset checks passed')

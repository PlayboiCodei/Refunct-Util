"""Sky material integration checks; Windows x86 and actual gameplay remain unverified."""
from pathlib import Path
import re
root=Path(__file__).resolve().parents[1]
menu=(root/'src/menu.cpp').read_text()
code=(root/'src/sky.cpp').read_text()
state=(root/'include/refunct/state.hpp').read_text()
assert 'CustomisationSky' in state
for fn in ('src/sky.cpp', 'include/refunct/features/sky.hpp'):
    assert (root/fn).exists()
assert 'src/sky.cpp' in (root/'CMakeLists.txt').read_text()
assert '#include "refunct/features/sky.hpp"' in (root/'include/refunct/internal_api.hpp').read_text()
assert 'UpdateSkyColours(player);' in (root/'src/game_tick.cpp').read_text()
assert 'else if (i == 9) SetPage(Page::CustomisationSky);' in menu
assert 'else if (i == 16) ResetWorldCustomisation();' in menu
assert 'case Page::CustomisationSky:' in menu
assert 's_tile_hex_axis = SkyHexInputAxis(region);' in menu
for tag in ('SkyColourMethodLabel(SkyRegion::Zenith)', 'SkyColourMethodLabel(SkyRegion::Horizon)',
            'OpenSkyColourMethod(SkyRegion::Zenith)', 'OpenSkyColourMethod(SkyRegion::Horizon)',
            'SetSkyColourHex(SkyRegion::Zenith, hex)', 'SetSkyColourHex(SkyRegion::Horizon, hex)',
            'ResetSkyColours();'):
    assert tag in menu,tag
for tag in ('BP_DynamicSky_C', 'SkyMaterial', 'M_Sky_Panning_Clouds2',
            'Zenith Color', 'Horizon color', 'MaterialInstanceDynamic',
            'Conv_StringToName', 'K2_GetVectorParameterValue',
            'SetVectorParameterValue', 's_target.mid_serial',
            'SkyColours', 'ZenithEnabled', 'HorizonEnabled',
            'ZenithHex', 'HorizonHex', 'QueueSettingsValue',
            'SkyIsA', 'TargetLive', 's_picker_region', 'ResetSkyColours'):
    assert tag in code,tag
for forbidden in ('g_recording_tainted =', 'g_run_pb_eligible =', 'MarkCheatAction(',
                  'WaterSetMaterial(', 'SetMovementMode('):
    assert forbidden not in code,forbidden
assert 'UpdateWaterColours(player);' in (root/'src/game_tick.cpp').read_text()
assert 's_sky_use_windows_picker[0] = !s_sky_use_windows_picker[0];' in menu
assert 's_sky_use_windows_picker[1] = !s_sky_use_windows_picker[1];' in menu
assert 's_settings.enabled[i] = true;' in code
assert 'SameColour(current, value.last_written)' in code
assert 'SetMIDColor(s_target.mid, value.name, value.original)' in code
assert 'IsReadable(' in code
assert 'kScanBudgetMs = 2' in code
print('Sky colour integration checks passed')

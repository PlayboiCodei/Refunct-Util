"""Integration guard for moving-platform (BP_Lift/BP_Mover) customisation.
This checks source structure; a Windows Win32 build and Refunct runtime test remain required.
"""
from pathlib import Path
root = Path(__file__).resolve().parents[1]
menu = (root / 'src/menu.cpp').read_text()
impl = (root / 'src/platforms.cpp').read_text()
header = (root / 'include/refunct/features/platforms.hpp').read_text()
state = (root / 'include/refunct/state.hpp').read_text()
records = (root / 'src/records.cpp').read_text()
tick = (root / 'src/game_tick.cpp').read_text()
cmake = (root / 'CMakeLists.txt').read_text()

for name in ('WorldPlatforms', 'WorldPlatformColours', 'WorldPlatformSpeed'):
    assert f'case Page::{name}:' in menu
    assert name in state
assert 'L"Platform Speed  >"' in menu and 'L"Platform Colours  >"' in menu
assert 'else if (i == 6) SetPage(Page::WorldPlatformSpeed)' in menu
assert 'else if (i == 5) SetPage(Page::WorldPlatformColours)' in menu
assert 'g_page == Page::WorldPlatformSpeed && g_selection == 0' in menu
assert 's_tile_hex_axis == 5' in menu
assert 'SetPlatformCustomColourHex(hex)' in menu
assert 's_platform_use_windows_picker = !s_platform_use_windows_picker' in menu

# Restrict all actor mutations to precisely named moving platforms.
assert 'name == L"BP_Lift_C" || name == L"BP_Mover_C"' in impl
assert 'ReadReflectedObject(actor, L"LiftMesh")' in impl
assert 'ReadReflectedObject(actor, L"LiftMover")' in impl
assert 'ObjectName(asset) != L"SM_Lift"' in impl
assert 'IsReadable' in impl and 'serial_number == entry.serial' in impl
assert 'BP_LevelRoot_C' in impl and 'UpdateNativeSectionSpeed' not in impl

# Reversible, dynamic MID colour; original asset isn't modified.
assert 'VectorParameterValues' in impl
assert 'M_Gold_Rough_Yellow' in impl and 'L"Color"' in impl
assert 'CreateDynamicMaterialInstance' in impl
assert 'SetVectorParameterValue' in impl
assert 'SetMaterial(target.mesh, target.source)' in impl
assert 'settings.custom_colour || target.material_instance' in impl

# Timeline playback rate follows the native actor baseline and restores it.
assert 'GetPlayRate' in impl and 'SetPlayRate' in impl
assert 'target.native_rate = observed' in impl
assert 'WriteRate(target, target.native_rate)' in impl
assert 'target.written_rate = desired' in impl
assert 'GetPlatformSpeedPercent() != 100' in impl
assert 'current <= (std::numeric_limits<int>::max)() - step' in impl

# Bounded work to prevent previous per-frame/button-press lag regression.
assert 'kScanBatch = 2048' in impl and 'kApplyBatch = 12' in impl
assert 'ColourWorkBudget budget(700)' in impl and 's_pending_colour' in impl
assert 'kUpdateMs = 50' in impl
assert 'Config().speed_percent != 100 ? 5000 : 37000' in impl
assert 's_discovery_watermark.Resume' in impl
assert 'src/platforms.cpp' in cmake
assert 'UpdatePlatforms(player);' in tick
assert 'PlatformColour, PlatformSpeed' in records
assert 'set(CheatBit::PlatformColour, IsPlatformCustomColourEnabled());' in records
assert 'set(CheatBit::PlatformSpeed, IsPlatformSpeedModified());' in records
for func in ('IsPlatformCustomColourEnabled', 'SetPlatformCustomColourEnabled',
             'GetPlatformCustomColourHex', 'SetPlatformCustomColourHex',
             'OpenPlatformColourPicker', 'GetPlatformSpeedPercent',
             'AdjustPlatformSpeed', 'ResetPlatformSpeed', 'UpdatePlatforms'):
    assert func+'(' in header and func+'(' in impl
print('PASS: platform menus, targeting, colour, speed, cheat tracking and bounded updates')

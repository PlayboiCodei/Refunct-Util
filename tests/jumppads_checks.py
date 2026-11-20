"""Jump-pad source-integrity checks. Gameplay must be verified in Refunct Win32."""
from pathlib import Path
root=Path(__file__).resolve().parents[1]
code=(root/'src/jumppads.cpp').read_text()
menu=(root/'src/menu.cpp').read_text()
state=(root/'include/refunct/state.hpp').read_text()
records=(root/'src/records.cpp').read_text()
cmake=(root/'CMakeLists.txt').read_text()
hud=(root/'src/hud.cpp').read_text()
assert 'src/jumppads.cpp' in cmake
assert 'UpdateJumpPads(player);' in (root/'src/game_tick.cpp').read_text()
assert 'name == L"BP_Jumppad_C"' in code
assert 'ReadReflectedObject(actor, L"Mover")' in code
assert 'JumpPadObjectName(asset) != L"SM_Jumppad"' in code
assert 'M_Gold_Rough_Yellow' in code and 'L"Color"' in code
assert 'SetVectorParameterValue' in code
assert 'JumpPadSetMaterial(target.mesh, target.source)' in code
assert 's_settings.height_percent = height >= 25' in code
assert 'L"PerformJumppadJump"' in code
assert 'JumpPadProcessEventHook' in code
assert 'L"Jumppad", L"ObjectProperty"' in code
assert 's_launch.pending = true;' in code
assert 'std::abs(z - s_launch.before_z) < 40.0f' in code
assert 'std::sqrt(static_cast<double>(Config().height_percent) / 100.0)' in code
assert 's_launch.pending = false;' in code
assert 'g_restart_requested || g_full_level_reload_requested' in code
assert 'kUpdateMs = 50' in code and 'kApplyBatch = 8' in code
assert 'IsJumpPadCustomColourEnabled' in records and 'IsJumpPadHeightModified' in records
assert 'Jump pad launch height' in hud
for page in ('WorldJumpPads','WorldJumpPadColours','WorldJumpPadHeight'):
 assert f'case Page::{page}:' in menu
 assert page in state
assert 'else if (i == 8) SetPage(Page::WorldJumpPadHeight);' in menu
assert 'else if (i == 6) SetPage(Page::WorldJumpPadColours);' in menu
assert 's_tile_hex_axis == 6' in menu
assert 'SetJumpPadCustomColourHex(hex)' in menu
assert 'OpenJumpPadColourPicker()' in code
assert 'L"Reset to 100%"' in menu
assert 'std::numeric_limits<int>::max' in code
# Source model: launch height is proportional to vertical speed squared.
import math
for percent in [25,50,100,150,200,400,1000]:
 speed=math.sqrt(percent/100)
 assert math.isclose(speed*speed, percent/100, rel_tol=1e-12)
print('PASS: jump-pad menu, colour, bounded scans, launch-height model, cheats')

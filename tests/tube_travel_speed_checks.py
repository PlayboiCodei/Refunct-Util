"""Static regression checks for safe tube-timeline speed control.

Does not replace a Win32 build / game test. The tube blueprint is known to
contain a TravelMover timeline and SetPlayRate from the supplied game PAK.
"""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
src = (root / "src/tubes.cpp").read_text(encoding="utf-8")
menu = (root / "src/menu.cpp").read_text(encoding="utf-8")
state = (root / "include/refunct/state.hpp").read_text(encoding="utf-8")
header = (root / "include/refunct/features/tubes.hpp").read_text(encoding="utf-8")
game_tick = (root / "src/game_tick.cpp").read_text(encoding="utf-8")
records = (root / "src/records.cpp").read_text(encoding="utf-8")

for item in (
    'GetPrivateProfileIntW(L"TubeTravel", L"SpeedPercent"',
    'QueueSettingsValue(L"TubeTravel", L"SpeedPercent"',
    'percent = 100', 'read >= 25 && read % 25 == 0',
    'current <= (std::numeric_limits<int>::max)() - kStep',
    'SetTravelPercent(current + kStep)',
    'ReadReflectedObject(player, L"TravelMover")',
    'L"TimelineComponent"',
    'static_cast<const UObjectHeader*>(object)->outer != owner',
    'FindReflectedFunction(timeline, L"GetPlayRate")',
    'FindReflectedFunction(timeline, L"SetPlayRate")',
    'Param(s_travel.get_rate, L"ReturnValue", L"FloatProperty", 96)',
    'Param(s_travel.set_rate, L"NewRate", L"FloatProperty", 96)',
    'const bool still_ours = s_travel.modified',
    's_travel.native_rate = observed',
    's_travel.native_rate * (static_cast<float>(speed) / 100.0f)',
    'const float desired = std::min(requested, 1000.0f)',
    'WriteTravelRate(s_travel.native_rate)',
    's_travel.next_check = now + 50',
    'g_recording_tainted = true',
    'if (!g_pb_cheats_allowed_for_run) g_run_pb_eligible = false',
):
    assert item in src, item

for item in (
    'WorldTubeSpeed', 'L"Tube Speed  >"',
    'L"Reset to 100%"',
    'case Page::WorldTubeSpeed:',
    'AdjustTubeTravelSpeed(key == VK_RIGHT ? +1 : -1)',
    'ResetTubeTravelSpeed()',
):
    assert item in menu, item

assert '25% to 400%' not in menu
assert 'read <= 400' not in src
assert 'std::clamp(current' not in src
assert 'WorldTubeSpeed' in state
assert 'IsTubeTravelSpeedModified()' in records
assert 'UpdateTubeTravelSpeed(player)' in game_tick
assert 'void UpdateTubeTravelSpeed(MyCharacter* player);' in header
assert 'UpdateTubeColours(player);' in game_tick  # existing tube colour functionality kept

# No direct player-velocity or shared-material writes in the speed updater.
speed = src.split('void UpdateTubeTravelSpeed(MyCharacter* player) {', 1)[1].split('bool IsTubeCustomColourEnabled()', 1)[0]
assert 'player->movement->velocity' not in speed
assert 'SetMaterial(' not in speed
assert 'WriteTravelRate(desired)' in speed
print('Tube travel speed integration and safety checks passed')

"""Climb speed integration checks for Refunct's actual ClimbSpeed property.

These verify source wiring and regression-sensitive navigation indices.
Windows/x86 gameplay testing must confirm the game's runtime behavior.
"""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
read = lambda file: (root / file).read_text(encoding="utf-8")
movement = read("src/movement.cpp")
menu = read("src/menu.cpp")
state = read("src/state.cpp")
header = read("include/refunct/state.hpp")
api = read("include/refunct/features/movement.hpp")
storage = read("src/storage.cpp")
records = read("src/records.cpp")

# Native Refunct character property, NOT generic walk/fly/climb velocity guesses.
assert 'ResolveTunedFloat(player, L"ClimbSpeed", 0.01f, 20000.0f)' in movement
assert 'ApplyTunedFloat(g_tuning_climb_speed,' in movement
assert 'static_cast<float>(g_climb_speed_percent) / 100.0f' in movement
assert 'g_tuning_climb_speed = {};' in movement  # Reset on new character
assert 'extern TunedFloatProperty g_tuning_climb_speed;' in header
assert 'TunedFloatProperty g_tuning_climb_speed{};' in state
assert 'g_climb_speed_percent = 100;' in state
assert 'extern int g_climb_speed_percent;' in header
assert 'void AdjustClimbSpeedPercent(int direction);' in api
assert 'g_climb_speed_percent = 100;' in movement  # Reset movement defaults

# Preference is saved, loaded, and included in the assisted-run policy.
assert 'write(L"Movement", L"ClimbSpeedPercent", std::to_wstring(g_climb_speed_percent));' in storage
assert 'g_climb_speed_percent = ReadUncappedPercentSetting(L"ClimbSpeedPercent", kMovementMinimumPercent);' in storage
assert 'set(CheatBit::Climb, g_climb_speed_percent != 100);' in records
assert 'InvalidateAssistedTiming();' in movement.split('void AdjustClimbSpeedPercent(int direction) {', 1)[1].split('}\n', 1)[0]

# All menu actions/description rows must remain aligned after insertion.
render = menu.split('case Page::MovementPhysics:', 1)[1].split('case Page::World:', 1)[0]
assert 'L"Climb speed: " + std::to_wstring(g_climb_speed_percent)' in render
action = menu.split('void ActivateSelected(MyCharacter* player) {', 1)[1].split('namespace {\n// Left/Right settings', 1)[0]
physics = action.split('case Page::MovementPhysics:', 1)[1].split('case Page::World:', 1)[0]
assert 'else if (i == 3) AdjustClimbSpeedPercent(+1);' in physics
assert 'else if (i == 8) AdjustGameSpeedPercent(+1);' in physics
arrow = menu.split('void HandleHorizontalMovement(int key, MyCharacter* player) {', 1)[1].split('} // namespace\n\nvoid ProcessKey(int key, MyCharacter* player) {', 1)[0]
assert 'g_page == Page::MovementPhysics && g_selection == 3' in arrow
assert 'AdjustClimbSpeedPercent(key == VK_RIGHT ? +1 : -1)' in arrow
assert 'g_page == Page::MovementPhysics && g_selection == 8' in arrow
assert 'L"Adjust ledge climbing speed without altering jumps or walking."' in menu

print("Climb speed integration checks passed")

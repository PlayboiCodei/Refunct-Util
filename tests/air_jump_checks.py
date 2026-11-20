"""Air-jump integration checks: new input edges, real jump strength and cheat gate."""
from pathlib import Path
root = Path(__file__).resolve().parents[1]
def load(path): return (root / path).read_text(encoding='utf-8')
menu = load('src/menu.cpp')
movement = load('src/movement.cpp')
game_tick = load('src/game_tick.cpp')
state = load('include/refunct/state.hpp')
storage = load('src/storage.cpp')
records = load('src/records.cpp')
hud = load('src/hud.cpp')
assert 'extern bool g_air_jump_enabled;' in state
assert 'bool g_air_jump_enabled = false;' in load('src/state.cpp')
assert 'AirJumpEnabled' in storage and 'write(L"Movement", L"AirJumpEnabled"' in storage
assert 'ToggleAirJump()' in menu
assert 'case Page::MovementJump:' in menu and 'g_selection == 2' in menu
assert 'g_air_jump_enabled = false;' in movement.split('bool ResetMovementConfiguration()',1)[1]
assert 'ResetAirJumpInputState();' in movement
assert 's_air_jump.initialized' in movement and '!s_air_jump.previous_space_down' in movement
assert 's_air_jump.pending = g_air_jump_enabled && input_allowed && airborne' in movement
assert 'g_scroll_to_jump.load(std::memory_order_relaxed) && wheel_requests > 0' in movement
assert 'player->movement->movement_mode != 3' in movement
assert 'player->movement->velocity.z = launch_speed;' in movement
assert 'g_tuning_jump_velocity.address' in movement and 'IsReadable(velocity_property, sizeof(float))' in movement
assert 'ApplyPendingAirJump(player);' in game_tick
assert game_tick.index('ApplyMovementTuning(player);') < game_tick.index('ApplyPendingAirJump(player);')
assert 'set(CheatBit::AirJump, g_air_jump_enabled);' in records
assert 'add(CheatBit::AirJump, L"Air jump");' in records
assert 'if (g_air_jump_enabled) modules.emplace_back(L"Air jump");' in hud
assert 'L"Allow an additional jump input' in menu
print('Air-jump integration checks passed')

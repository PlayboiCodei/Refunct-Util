"""Compile and execute the exact post-native air-jump function with portable ABI doubles.

This does not substitute for the Win32 MSVC build or in-game verification.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
src = (root / "src/movement.cpp").read_text(encoding="utf-8")
function = src.split("void ApplyPendingAirJump(MyCharacter* player) {", 1)[1].split(
    "\n}\n\n} // namespace refunct", 1
)[0]
function = "void ApplyPendingAirJump(MyCharacter* player) {" + function + "\n}"
harness = r'''
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <cassert>
namespace refunct {
struct Vec3 { float x, y, z; };
struct MovementComponent { Vec3 velocity{}; uint8_t movement_mode = 3; };
struct MyCharacter { MovementComponent* movement; };
struct TunedFloatProperty { float* address; };
struct AirJumpInputState {
    MyCharacter* character = nullptr;
    bool initialized = false;
    bool previous_space_down = false;
    bool pending = false;
    bool warned_missing_velocity = false;
};
bool g_air_jump_enabled = true;
bool g_flying = false;
bool g_noclip = false;
TunedFloatProperty g_tuning_jump_velocity{};
AirJumpInputState s_air_jump{};
bool IsReadable(const void* p, std::size_t) { return p != nullptr; }
void Log(const wchar_t*) {}
void Notify(const std::wstring&) {}
'''
tests = r'''
int main() {
    MovementComponent movement{};
    movement.velocity = {5.0f, -10.0f, -100.0f};
    MyCharacter player{&movement};
    float strength = 420.0f;
    g_tuning_jump_velocity.address = &strength;
    s_air_jump.character = &player;
    s_air_jump.pending = true;
    ApplyPendingAirJump(&player);
    assert(movement.velocity.x == 5.0f && movement.velocity.y == -10.0f);
    assert(movement.velocity.z == 420.0f);
    assert(!s_air_jump.pending);
    // No repeated jump from the same press.
    movement.velocity.z = -20.0f;
    ApplyPendingAirJump(&player);
    assert(movement.velocity.z == -20.0f);
    // Disallow ground, flying, invalid launch, or a stale character.
    s_air_jump.pending = true;
    movement.movement_mode = 1;
    ApplyPendingAirJump(&player);
    assert(movement.velocity.z == -20.0f);
    movement.movement_mode = 3;
    g_flying = true;
    s_air_jump.pending = true;
    ApplyPendingAirJump(&player);
    assert(movement.velocity.z == -20.0f);
    g_flying = false;
    strength = -1.0f;
    s_air_jump.pending = true;
    ApplyPendingAirJump(&player);
    assert(movement.velocity.z == -20.0f);
    strength = 520.0f;
    s_air_jump.pending = true;
    ApplyPendingAirJump(&player);
    assert(movement.velocity.z == 520.0f);
    return 0;
}
}
int main() { return refunct::main(); }
'''
with tempfile.TemporaryDirectory() as tmp:
    source = Path(tmp) / "airjump.cpp"
    binary = Path(tmp) / "airjump"
    source.write_text(harness + function + tests, encoding="utf-8")
    subprocess.run(["g++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-O2", str(source), "-o", str(binary)], check=True, timeout=25)
    subprocess.run([str(binary)], check=True, timeout=10)
print("Air-jump post-native implementation compiles and passes simulated movement cases")

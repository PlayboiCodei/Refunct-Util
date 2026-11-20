"""Portable C++ mock for the World persistence lifecycle policies.

Compiles the *actual* functions extracted from movement.cpp/menu.cpp with
minimal stand-ins for UE4 objects. This does not validate Windows x86 linking.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
menu = (root / "src/menu.cpp").read_text(encoding="utf-8")
movement = (root / "src/movement.cpp").read_text(encoding="utf-8")


def extract(source: str, signature: str) -> str:
    first = source.index(signature)
    opening = source.index("{", first)
    depth = 0
    for i in range(opening, len(source)):
        if source[i] == "{":
            depth += 1
        elif source[i] == "}":
            depth -= 1
            if depth == 0:
                return source[first : i + 1]
    raise AssertionError(f"Unclosed function: {signature}")


functions = "\n".join(
    [extract(menu, "bool WorldGameplayOptionsModified()"),
     extract(menu, "void ResetWorldGameplayForPersistence()"),
     extract(movement, "void ResetWorldOnNormalRestart()"),
     extract(movement, "void ApplyWorldGameLaunchPersistence()"),
     extract(movement, "void ToggleWorldPersistence(bool game_launch)")]
)

preamble = r"""
#include <string>
#include <initializer_list>
#include <cassert>
#include <cstdint>
int g_player_fov_degrees = 0;
bool g_animate_section_changes = true;
bool g_buttons_sync_with_sections = false;
bool g_keep_world_on_game_launch = false;
bool g_keep_world_on_map_restart = false;
int saved_count = 0;
int baseline_count = 0;
void NoteWorldProfilePersistenceReset() { ++baseline_count; }
std::wstring notification;
int fake_section_offsets = 0;
int fake_object_removed = 0;
enum class WorldObjectKind { Cubes, Buttons, Tiles, Tubes, Platforms, JumpPads };
enum class SectionPlacement { Auto, Raised };
constexpr int kPracticeClusterCount = 31;
bool IsSectionSpeedModified() { return false; }
bool IsTubeTravelSpeedModified() { return false; }
bool IsPlatformSpeedModified() { return false; }
bool IsJumpPadHeightModified() { return false; }
bool IsWaterRemoved() { return false; }
bool IsAllWaterRemoved() { return false; }
bool IsWaterSwimmingDisabled() { return false; }
int GetWaterHeightOffset() { return 0; }
bool IsButtonTileSelectionDisabled() { return false; }
bool IsTileSelectionOnContactDisabled() { return false; }
bool IsWorldCubeOverrideActive() { return false; }
bool IsWorldObjectRemoved(WorldObjectKind) { return fake_object_removed != 0; }
SectionPlacement GetSectionPlacement(int) { return SectionPlacement::Auto; }
float GetSectionPositionOffset(int, int) { return static_cast<float>(fake_section_offsets); }
void ResetJumpPadHeight() {}
void ResetPlatformSpeed() {}
void ResetTubeTravelSpeed() {}
void ResetSectionSpeed() {}
void ResetWorldObjectRemovals() { fake_object_removed = 0; }
void SetAllWaterRemoved(bool) {}
void SetWaterRemoved(bool) {}
void SetWaterSwimmingDisabled(bool) {}
void SetWaterHeightOffset(int) {}
bool SetButtonsSyncWithSections(bool state) { g_buttons_sync_with_sections = state; return true; }
void ResetAllSectionPlacements() {}
bool ResetAllSectionPositionOffsets() { fake_section_offsets = 0; return true; }
void ToggleButtonTileSelection() {}
void ToggleTileSelectionOnContact() {}
enum class CubeWorldAction { RestoreDefault };
void* g_player = nullptr;
bool RequestCubeWorldAction(CubeWorldAction, void*) { return true; }
void Log(const wchar_t*) {}
void SaveSettings() { ++saved_count; }
void Notify(const std::wstring& value) { notification = value; }
"""

main = r"""
int main() {
    assert(!WorldGameplayOptionsModified());
    ResetWorldOnNormalRestart();
    assert(saved_count == 0 && baseline_count == 0); // No SaveSettings for vanilla world.

    // FOV is a persistent customisation; it does not trigger World resets.
    g_player_fov_degrees = 115;
    assert(!WorldGameplayOptionsModified());
    ResetWorldOnNormalRestart();
    assert(saved_count == 0 && g_player_fov_degrees == 115);

    // A genuine World gameplay setting resets, but FOV survives.
    fake_object_removed = 1;
    ResetWorldOnNormalRestart();
    assert(saved_count == 1 && fake_object_removed == 0 && g_player_fov_degrees == 115);

    // Map restart: ON preserves gameplay World settings too.
    ToggleWorldPersistence(false);
    assert(g_keep_world_on_map_restart && saved_count == 2);
    g_player_fov_degrees = 125;
    fake_object_removed = 1;
    ResetWorldOnNormalRestart();
    assert(saved_count == 2 && baseline_count == 1 && fake_object_removed == 1 && g_player_fov_degrees == 125);
    ToggleWorldPersistence(false);
    assert(!g_keep_world_on_map_restart);
    fake_object_removed = 0;

    // All object flags and section position overrides trigger reset checks.
    g_player_fov_degrees = 0;
    fake_object_removed = 1;
    assert(WorldGameplayOptionsModified());
    fake_object_removed = 0;
    fake_section_offsets = 1;
    assert(WorldGameplayOptionsModified());
    fake_section_offsets = 0;

    // Game launch: ON retains; OFF resets once when the first valid tick runs.
    g_keep_world_on_game_launch = true;
    g_player_fov_degrees = 135;
    ApplyWorldGameLaunchPersistence();
    assert(saved_count == 3 && g_player_fov_degrees == 135);
    g_keep_world_on_game_launch = false;
    ApplyWorldGameLaunchPersistence();
    assert(saved_count == 3); // Never runs twice in the same process.
    ToggleWorldPersistence(true);
    assert(g_keep_world_on_game_launch && saved_count == 4);
    return 0;
}
"""

# Separate process verifies the launch-OFF path: the production helper runs
# once per game process by design, so it cannot be retested in the same binary.
launch_off_main = r"""
int main() {
    assert(!g_keep_world_on_game_launch);
    g_player_fov_degrees = 117;
    fake_object_removed = 1;
    ApplyWorldGameLaunchPersistence();
    assert(saved_count == 1 && baseline_count == 1 && fake_object_removed == 0 && g_player_fov_degrees == 117);
    g_player_fov_degrees = 130;
    ApplyWorldGameLaunchPersistence();
    assert(saved_count == 1 && g_player_fov_degrees == 130);
    return 0;
}
"""
compiler = shutil.which("g++")
if not compiler:
    raise RuntimeError("Portable C++ compiler unavailable")
with tempfile.TemporaryDirectory(prefix="refunct-world-persist-") as temp:
    src = Path(temp) / "test.cpp"
    exe = Path(temp) / "test"
    for case, code in (("launch_on", main), ("launch_off", launch_off_main)):
        src.write_text(preamble + "\n" + functions + "\n" + code,
                       encoding="utf-8")
        subprocess.run([compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                        "-O2", str(src), "-o", str(exe)], check=True, timeout=60)
        subprocess.run([str(exe)], check=True, timeout=20)
print("PASS: compiled World persistence lifecycle/reset and settings tests")

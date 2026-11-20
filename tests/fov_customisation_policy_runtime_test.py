"""FOV customisation: compiled bitmask policy and source-level menu/persistence wiring.

Win32 game integration and rendered camera behaviour must still be tested in Refunct.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
records = (root / "src/records.cpp").read_text(encoding="utf-8")
menu = (root / "src/menu.cpp").read_text(encoding="utf-8")
sections = (root / "src/sections.cpp").read_text(encoding="utf-8")
storage = (root / "src/storage.cpp").read_text(encoding="utf-8")

# Keep the historical Page ID and [World] PlayerFOV save key, but change the
# navigation parent and ensure a World reset doesn't touch this setting.
def case_block(source, name):
    return source.split(f"        case Page::{name}:", 1)[1].split("        case Page::", 1)[0]

build = menu.split("std::vector<std::wstring> BuildEntriesForPage(Page page) {", 1)[1]
clicks = menu.split("void ActivateSelected(MyCharacter* player) {", 1)[1]
back = menu.split("void GoBack() {", 1)[1].split("void ToggleProgressRiseAnimation", 1)[0]
world_rows = case_block(build, "World")
custom_rows = case_block(build, "Customisation")
world_clicks = case_block(clicks, "World")
custom_clicks = case_block(clicks, "Customisation")

assert 'L"Player & Camera  >"' not in world_rows
assert 'L"Camera FOV  >"' in custom_rows
assert "SetPage(Page::WorldPlayer)" not in world_clicks
assert "else if (i == 12) SetPage(Page::WorldPlayer);" in custom_clicks
assert "case Page::WorldPlayer: case Page::WorldButtonColours:" in back
assert 'case Page::WorldPlayer: return L"Customisation / Camera FOV";' in menu
assert "AdjustPlayerFovDegrees(key == VK_RIGHT ? +1 : -1);" in menu
assert 'write(L"World", L"PlayerFOV"' in storage  # backwards-compatible settings
assert 'GetPrivateProfileStringW(L"World", L"PlayerFOV"' in storage

world_reset = menu.split("void ResetWorldGameplayOptions() {", 1)[1].split(
    "void ResetWorldCustomisation() {", 1
)[0]
world_persistence = menu.split("bool WorldGameplayOptionsModified() {", 1)[1].split(
    "// These are defined in refunct", 1
)[0]
custom_reset = menu.split("void ResetWorldCustomisation() {", 1)[1].split(
    "// Page through large map folders", 1
)[0]
world_monitor = sections.split("bool AnyWorldGameplayOptionActive() {", 1)[1].split(
    "// Runs on the game thread, throttled.", 1
)[0]
assert "g_player_fov_degrees" not in world_reset
assert "g_player_fov_degrees" not in world_persistence
assert "g_player_fov_degrees" not in world_monitor
assert "g_player_fov_degrees = 0;" in custom_reset
assert "SaveSettings();" in custom_reset
assert "set(CheatBit::CameraFov, g_player_fov_degrees != 0);" in records

# Compile the actual cheat-bit registry and mask declarations from records.cpp.
# This tests the classification used for PB eligibility, notification and HUD.
start = records.index("enum class CheatBit : unsigned char {")
end = records.index("uint64_t AllSettingMask() {", start)
mask_definitions = records[start:end]
compiler = shutil.which("g++") or shutil.which("clang++")
if compiler:
    cpp = r"""
#include <cassert>
#include <cstdint>
""" + mask_definitions + r"""
int main() {
    const uint64_t fov_setting = CheatMask(CheatBit::CameraFov);
    const uint64_t gameplay_speed = CheatMask(CheatBit::TubeSpeed);
    // A camera tweak is in customisations, not the gameplay-cheat gate.
    assert((fov_setting & kCustomisationMask) == fov_setting);
    assert((fov_setting & ~kCustomisationMask) == 0);
    // A genuine gameplay modifier must still invalidate normal records.
    assert((gameplay_speed & kCustomisationMask) == 0);
    assert((gameplay_speed & ~kCustomisationMask) == gameplay_speed);
}
"""
    with tempfile.TemporaryDirectory(prefix="refunct-fov-mask-") as work:
        source = Path(work) / "fov_mask.cpp"
        executable = Path(work) / "fov_mask"
        source.write_text(cpp, encoding="utf-8")
        subprocess.run([compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                        str(source), "-o", str(executable)], check=True, timeout=45)
        subprocess.run([str(executable)], check=True, timeout=15)
    print("PASS: compiled FOV customisation/cheat bitmask policy")
else:
    print("SKIP: compiled FOV mask test (no portable compiler)")
print("PASS: FOV menu, save compatibility, World reset and monitoring isolation")

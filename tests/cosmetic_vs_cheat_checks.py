"""Visual settings must not taint PBs; game-changing controls still must.
These source-level checks are not an in-game Win32 validation.
"""
from pathlib import Path
import re
root = Path(__file__).resolve().parents[1]
read = lambda name: (root/name).read_text(encoding="utf-8")
records = read("src/records.cpp")
hud = read("src/hud.cpp")
menu = read("src/menu.cpp")
state = read("include/refunct/state.hpp")
storage = read("src/storage.cpp")
platforms = read("src/platforms.cpp")
jumppads = read("src/jumppads.cpp")
settings = records.split("uint64_t AllSettingMask() {", 1)[1].split("uint64_t SettingCheatMask()", 1)[0]
cosmetic_bits = records.split("constexpr uint64_t kCustomisationMask =", 1)[1].split(";", 1)[0]
cosmetics = (
    "CameraFov", "TileSelectedColour", "TileUnselectedColour", "TileRandomColour",
    "WallSelectedColour", "WallUnselectedColour", "ButtonSelectedColour",
    "ButtonPressedColour", "BorderSelectedColour", "BorderPressedColour",
    "BeaconHidden", "BeaconColour", "TubeColour", "PlatformColour", "JumpPadColour", "WaterColour",
)
for name in cosmetics:
    assert f"CheatMask(CheatBit::{name})" in cosmetic_bits, name
    assert f"set(CheatBit::{name}," in settings, name
for name in ("TubeSpeed", "PlatformSpeed", "JumpPadHeight", "SectionSpeed", "InstantRise",
             "Walk", "Jump", "GameSpeed", "ButtonSync",
             "ButtonTileGuard", "TileTouchGuard"):
    assert f"CheatMask(CheatBit::{name})" not in cosmetic_bits, name
    assert f"set(CheatBit::{name}," in settings, name
assert "set(CheatBit::ButtonRepair, IsButtonFixEnabled())" not in settings
assert "return AllSettingMask() & ~kCustomisationMask;" in records
assert "return AllSettingMask() & kCustomisationMask;" in records
# New visual-only settings may be listed without extending the 64-bit
# gameplay registry; the existing PB/cheat masks must remain unchanged.
assert 'LabelsForMask(ActiveCustomisationMask() &' in records
assert '~CheatMask(CheatBit::WaterColour)' in records
assert 'return names;' in records.split('std::vector<std::wstring> ActiveCustomisationLabels()', 1)[1].split('void MarkCheatAction', 1)[0]
assert "uint64_t ActiveCheatMask()" in records
assert "uint64_t active = SettingCheatMask() | s_used_actions;" in records
assert "!g_recording_tainted && !CheatsCurrentlyActive()" in records
assert "g_recording_tainted = true;" in records.split("void UpdateCheatNotifications()",1)[1].split("bool RecordsAllowedNow()",1)[0]
assert "ActiveCustomisationLabels()" in read("include/refunct/features/records.hpp")
assert "const auto customisations = ActiveCustomisationLabels();" in hud
assert "Customisations Active (" in hud
assert "HudElement::ActiveCustomisations" in hud
assert "g_show_active_customisations" in hud
assert 'L"Customisations  >"' in menu
assert "g_show_active_customisations = !g_show_active_customisations;" in menu
assert "g_show_active_customisations = false;" in menu
assert "ActiveCustomisations," in state
assert "GameTime, LocalTime, ButtonNumber, KeyDisplay, Count" in state
assert 'L"ActiveCustomisationsPosition"' in state
assert 'L"Active visual settings"' in state
assert 'L"ShowActiveCustomisations"' in storage
assert "g_show_active_customisations = GetPrivateProfileIntW" in storage
assert 'write(L"Display", L"ShowActiveCustomisations"' in storage
# Colour controls must not call the shared gameplay-taint handler.
for mod, change, colour_method, speed_method in (
        (platforms, "Changed", "SetPlatformCustomColourEnabled", "AdjustPlatformSpeed"),
        (jumppads, "JumpPadChanged", "SetJumpPadCustomColourEnabled", "AdjustJumpPadHeight")):
    assert f"void {change}(const std::wstring& message, bool affects_gameplay)" in mod
    colour = mod.split(f"void {colour_method}(",1)[1].split("void ",1)[0]
    assert f"{change}(" in colour and "false);" in colour
    speed = mod.split(f"void {speed_method}(",1)[1].split("void ",1)[0]
    assert f"{change}(" in speed and "true);" in speed
    assert "if (affects_gameplay) {" in mod.split(f"void {change}(",1)[1].split("DWORD WINAPI",1)[0]
    assert "(visual only)" in mod
assert "UpdateCheatNotifications();" in read("src/game_tick.cpp")
# The status HUD continues checking only gameplay cheats, not customisations.
assert "(CheatsCurrentlyActive() || g_recording_tainted ||" in hud
print("PASS: visual customisations are independent of gameplay cheats and PB eligibility")

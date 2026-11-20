"""Regression checks: game-affecting state, one-shot actions, and one shared HUD/record policy.

This is static validation; a Windows/x86 build and in-game tests remain needed.
"""
from pathlib import Path
import re
r = Path(__file__).resolve().parents[1]
read = lambda name: (r / name).read_text(encoding="utf-8")
records = read("src/records.cpp")
hud = read("src/hud.cpp")
tick = read("src/game_tick.cpp")
header = read("include/refunct/features/records.hpp")
movement = read("src/movement.cpp")
sections = read("src/sections.cpp")
tiles = read("src/tiles.cpp")
practice = read("src/practice.cpp")
# The single tracked mask determines record eligibility and the HUD list.
assert "uint64_t ActiveCheatMask()" in records
assert "bool CheatsCurrentlyActive() { return ActiveCheatMask() != 0; }" in records
assert "LabelsForMask(ActiveCheatMask() & ~CheatMask(CheatBit::WorldObjectRemoval))" in records
assert "const auto cheats = ActiveCheatLabels();" in hud
assert "g_recording_tainted = true;" in records.split("void UpdateCheatNotifications()", 1)[1].split("bool RecordsAllowedNow()", 1)[0]
# New activation must be recorded before a PB update, not after it.
assert tick.index("UpdateCheatNotifications();") < tick.index("UpdateButtonSplits();")
assert "g_cheat_popup_text" in hud and "CHEAT ACTIVE:" in records
assert "CHEAT USED:" in records
for label in (
    "Climb speed", "Tube speed", "Section speed", "Section XYZ offsets",
    "Automatic button repair", "Button beacons hidden", "Tube colours",
    "Selected wall colours", "Random tile colours", "Camera FOV",
    "Selected tile colours", "Button tile selection blocked", "Instant section rises",
    "Used this run: teleport/restore position", "Used this run: button modifications",
    "Used this run: tile modifications", "Used this run: section modifications",
    "Used this run: checkpoint load", "Preloaded level restart",
    "Restart with progress", "Section/checkpoint practice",
):
    assert label in records, label
for state_flag in ("g_walk_speed_percent", "g_swim_speed_percent", "g_climb_speed_percent",
    "g_slide_speed_percent", "g_slide_distance_percent", "g_jump_height_percent",
    "g_acceleration_percent", "g_game_speed_percent", "g_preserve_momentum",
    "g_scroll_to_jump", "g_hold_space_to_jump", "g_buttons_sync_with_sections",
    "g_flying", "g_noclip", "g_restore_flying_pending", "g_restore_noclip_pending"):
    assert state_flag in records.split("uint64_t AllSettingMask()",1)[1], state_flag
assert "MarkCheatAction(CheatAction::Position);" in movement
assert "MarkCheatAction(CheatAction::Buttons);" in sections
assert "MarkCheatAction(CheatAction::Sections);" in sections
assert tiles.count("MarkCheatAction(CheatAction::Tiles);") == 2
assert "MarkCheatAction(CheatAction::Checkpoint);" in practice
# HUD-only controls and record-save preferences are not counted as cheats.
mask = records.split("uint64_t AllSettingMask()", 1)[1].split("uint64_t SettingCheatMask()", 1)[0]
for exempt in ("g_show_timer", "g_show_splits", "g_show_fps",
               "g_show_active_cheats", "g_show_menu_descriptions",
               "g_cheat_record_mode", "g_save_personal_bests"):
    assert exempt not in mask, exempt
assert "s_used_actions = 0;" in records.split("void ClearTaintOnFreshRun(int buttons)",1)[1].split("std::wstring ButtonLabel",1)[0]
# New API doesn't add more runtime hooks or modify the game binary.
assert "enum class CheatAction" in header
assert "ActiveCheatLabels()" in header
print("PASS: comprehensive shared cheat tracking")

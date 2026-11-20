"""Integration checks for manual and automatic ghost-button recovery.

Source guards only: actual character contact must be verified in-game on Win32.
"""
from pathlib import Path
root = Path(__file__).resolve().parents[1]
sections = (root/'src/sections.cpp').read_text(encoding='utf-8')
menu = (root/'src/menu.cpp').read_text(encoding='utf-8')
api = (root/'include/refunct/features/practice.hpp').read_text(encoding='utf-8')
water = (root/'src/water.cpp').read_bytes()
assert b'prearmed DefaultPhysicsVolume as water BEFORE old WaterVolume exit' in water
assert 'bool RepairGhostButtonsNow();' in api
assert 'L"Repair ghost buttons now"' in menu
assert 'else if (i == 7) RepairGhostButtonsNow();' in menu
assert 'else if (i == 8) ToggleButtonTileSelection();' in menu
assert 'g_page == Page::WorldButtonActions && g_selection == 8' in menu
assert 'ToggleButtonFix' not in menu
assert 's_button_fix_manual_pending = true;' in sections
assert 'EnableButtonFixAll' not in sections
assert 's_button_fix_enabled' not in sections
assert 'if (s_button_fix_faulted)' in sections
assert 's_button_fix_retry_after_ms = now + 10000;' in sections
assert 'if (attempted >= 4) continue;' in sections
assert 's_button_fix_manual_pending && remaining == attempted' in sections
assert 'g_full_level_reload_requested ||' in sections
assert 's_sections.press_batch.active' in sections
assert 'const bool explicitly_disabled = button.visual_applied && !button.visual_active;' in sections
assert 'const bool eligible = !explicitly_disabled &&' in sections
assert 'const bool native_enabled = !pressed && collision;' in sections
assert 'native_enabled) {' in sections
assert 'if (native_enabled) {' in sections
assert 'now - s_button_fix_near_since[i] < 2000' in sections
assert 'now - s_button_fix_last_repair_ms[i] < 12000' in sections
assert 'RefreshButtonContactComponents(button, index + 1)' in sections
assert 'SetButtonCollision(button, true) &&' in sections
assert 'SetButtonPressed(button, false)' in sections
assert 'post_refresh->buttons != before.buttons' in sections
assert 'g_recording_tainted = true;' in sections
assert 'g_run_pb_eligible = false;' in sections
assert 'AUTO native-gate fault on button ' in sections
assert 'MarkCheatAction(CheatAction::Buttons);' in sections
assert 'MarkAutomaticButtonRepair(index + 1);' in sections
assert 'SetGenerateOverlapEvents' not in sections  # no speculative overlap changes
print('PASS: manual/auto ghost button menu, throttling, collision safety, and progression guards')

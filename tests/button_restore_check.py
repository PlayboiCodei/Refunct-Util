"""Static guards for V4 World > Buttons native one-shot actions.

These checks validate plumbing only. UE ProcessEvent behaviour needs a live
Refunct v9.30 Win32 test; never equate a source scan with gameplay validation.
"""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
menu = (root / "src/menu.cpp").read_text(encoding="utf-8")
sections = (root / "src/sections.cpp").read_text(encoding="utf-8")
api = (root / "include/refunct/features/practice.hpp").read_text(encoding="utf-8")
state = (root / "include/refunct/state.hpp").read_text(encoding="utf-8")

assert "WorldButtons" in state and "WorldButtonActions" in state and "WorldButtonColours" in state
assert 'L"Button Controls  >"' in menu and 'L"Button Colours  >"' in menu
labels = (
    'L"Restore game-default button states"',
    'L"Enable buttons (above water)"',
    'L"Enable all buttons (including underwater)"',
    'L"Disable buttons (underwater)"',
    'L"Disable buttons (above water)"',
    'L"Disable all buttons"',
    'L"Press all available buttons"',
)
for label in labels:
    assert menu.count(label) == 1, label
assert "else if (i == 2) SetPage(Page::WorldButtonActions);" in menu
assert "case Page::WorldButtonActions:" in menu and "case Page::WorldButtonColours:" in menu

# Keep index-to-action contracts aligned with the current BuildEntries order.
activation = menu.split("void ActivateSelected(MyCharacter* player) {", 1)[1]
start = activation.index("case Page::WorldButtonActions:")
actions = activation[start:activation.index("case Page::WorldButtonColours:", start)]
expected = (
    "if (i == 0) RestoreDefaultButtons();",
    "else if (i == 1) RestoreButtons(false);",
    "else if (i == 2) RestoreButtons(true);",
    "else if (i == 3) DisableUnderwaterButtons();",
    "else if (i == 4) DisableAboveWaterButtons();",
    "else if (i == 5) DisableAllButtons();",
    "else if (i == 6) PressAllAvailableButtons();",
    "else if (i == 7) RepairGhostButtonsNow();",
)
for item in expected:
    assert item in actions, item
for name in ("RestoreButtons", "RestoreDefaultButtons", "DisableUnderwaterButtons",
             "DisableAboveWaterButtons", "DisableAllButtons", "PressAllAvailableButtons",
             "RepairGhostButtonsNow"):
    assert f"{name}(" in api, name

restore = sections.split("bool RestoreButtonsImpl(bool include_underwater, bool restore_default) {", 1)[1].split("bool RestoreButtons(bool include_underwater)", 1)[0]
for text in (
    "wanted_active[i] = restore_default",
    "(i == completed && raised[button.section])",
    "(include_underwater || raised[button.section])",
    "if (i < completed) ++previously_pressed_restored;",
    "SetButtonVisualActive(button, active, i + 1)",
    "SetButtonCollision(button, true)", "SetButtonPressed(button, false)",
    "SetButtonCollision(button, false)", "SetButtonPressed(button, true)",
    "g_recording_tainted = true;", "g_buttons_sync_with_sections = false;",
    "rollback incomplete (restart map)",
):
    assert text in restore, text
assert "ActivatePracticeButton(" not in restore

disable = sections.split("bool DisableButtonsByScope(ButtonDisableScope scope) {", 1)[1].split("bool DisableUnderwaterButtons()", 1)[0]
for text in (
    "scope == ButtonDisableScope::All", "scope == ButtonDisableScope::AboveWater",
    "!IsSectionPhysicallyRaised(section, raised[section])",
    "SetButtonVisualActive(button, false, index + 1)",
    "SetButtonPressed(button, true)", "SetButtonCollision(button, false)",
    "g_recording_tainted = true;", "g_run_pb_eligible = false;",
    "g_buttons_sync_with_sections = false;",
    "rollback incomplete; restart map",
):
    assert text in disable, text
assert "ActivatePracticeButton(" not in disable

press = sections.split("bool PressAllAvailableButtons() {", 1)[1].split("std::wstring SectionButtonSyncStatus()", 1)[0]
for text in (
    "batch.indices[batch.count++] = i;", "if (!pressed && collision)",
    "for (int i = state->buttons; i < kRefunctButtonCount; ++i)",
    "g_recording_tainted = true;", "g_run_pb_eligible = false;",
    "ActivatePracticeButton(index, g_player)",
    "state->buttons == batch.progress_before_press + 1",
    "native button did not activate", "state->resets != batch.resets_at_start",
    "batch.player = g_player;", "g_player != batch.player",
    "g_full_level_reload_requested || g_restart_requested",
    "batch = {};",
):
    assert text in press, text
assert "UpdatePressAllAvailableButtons();" in sections
print("PASS: V4 button controls, native gates, rollback and pending restart safety")

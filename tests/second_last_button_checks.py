"""Static invariants for the B/N two-contact restoration feature."""
from pathlib import Path
r=Path(__file__).resolve().parents[1]
state=(r/'include/refunct/state.hpp').read_text()
api=(r/'include/refunct/features/practice.hpp').read_text()
menu=(r/'src/menu.cpp').read_text()
practice=(r/'src/practice.cpp').read_text()
assert 'LoadSecondLastButton, FullLevelReload, // Existing action IDs never move.' in state
assert '{L"Load second last button", L"LoadSecondLastButton", \'N\'}' in state
assert 'void LoadSecondLastButtonCheckpoint();' in api
assert 'int SecondLastButtonNumber();' in api
assert 'case Action::LoadSecondLastButton:' in menu
assert 'LoadSecondLastButtonCheckpoint();' in menu
assert 'L"Load second last button  ["' in menu
assert 'L"Second last button: "' in menu
# The action moved from the Save & Load root to Button Checkpoints, index 1.
assert 'else if (i == 1) ExecuteAction(Action::LoadSecondLastButton, player);' in menu
assert 'else if (i == 3) SetPage(Page::ButtonCheckpoints);' in menu
assert 'struct ButtonContact' in practice
assert 's_second_last_contact = s_last_contact;' in practice
assert 's_second_last_contact = {};' in practice
assert 's_selected_contact = contact;' in practice
assert 'origin == PracticeOrigin::LastButton && s_selected_contact.stance_valid' in practice
assert 'origin == PracticeOrigin::LastButton && g_keep_last_button_momentum &&' in practice
assert 's_selected_contact.velocity_valid' in practice
assert 'if (!reset && state->buttons == g_observed_buttons + 1 &&' in practice
assert '(!g_practice_active || g_practice_phase == PracticePhase::Ready)' in practice
assert 's_second_last_contact = {};' in practice
assert 'g_last_button_checkpoint_valid = false;' in practice
assert 'Vec3 spawn = PracticeSpawnForButton(button);' in practice
assert 'constexpr float kLastButtonSpawnHeightCorrection = 2.0f;' in practice
assert 'spawn.z -= kLastButtonSpawnHeightCorrection;' in practice
assert 'RequestPreparedPractice(button - 1, spawn,' in practice
assert 'RequestPreparedPractice(target_button - 1, PracticeSpawnForButton(target_button),' in practice
assert 'PracticeOrigin::LastButton, button);' in practice
print('PASS: two-button history, custom N binding, menu indices, independent pose/velocity replay')

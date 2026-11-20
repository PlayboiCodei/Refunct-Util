"""Checks that menu indices, config migration, and loaded-counter wiring match."""
from pathlib import Path
root = Path(__file__).resolve().parents[1]
menu = (root/'src/menu.cpp').read_text()
storage = (root/'src/storage.cpp').read_text()
practice = (root/'src/practice.cpp').read_text()
hud = (root/'src/hud.cpp').read_text()
state = (root/'include/refunct/state.hpp').read_text()
assert 'L"Section Practice  >"' in menu
# World > Run & Practice owns restart, practice and button checkpoints.
assert 'case Page::Practice: case Page::SavePositions:' in menu
assert 'case Page::SavedCheckpoints: case Page::ButtonCheckpoints:' in menu
assert 'SetPage(Page::SaveStates); break;' in menu
activation = menu.split('void ActivateSelected(MyCharacter* player) {', 1)[1].split('namespace {\n// Left/Right settings', 1)[0]
save_states = activation.split('case Page::SaveStates:', 1)[1].split('case Page::SavePositions:', 1)[0]
assert 'else if (i == 3) SetPage(Page::ButtonCheckpoints);' in save_states
assert 'else if (i == 4) SetPage(Page::Practice);' in save_states
button_checkpoints = activation.split('case Page::ButtonCheckpoints:', 1)[1].split('case Page::Practice:', 1)[0]
assert 'else if (i == 1) ExecuteAction(Action::LoadSecondLastButton, player);' in button_checkpoints
assert 'else if (i == 2) {' in button_checkpoints
assert 'TeleportToLastPressedButton(player)' not in button_checkpoints
assert 'g_keep_last_button_momentum = !g_keep_last_button_momentum;' in button_checkpoints
assert 'else if (i == 5) GoBack();' in button_checkpoints
assert 'Keep momentum on last button:' in menu
assert 'L"Back"' in menu
assert 'L"Button Checkpoints  >"' in menu
assert 'Button time vs fastest:' in menu
assert 'KeepLastButtonMomentum' in storage
assert 's_last_button_momentum_pending' in practice
assert 's_selected_contact.velocity_valid' in practice
assert 'RestoreLastButtonMomentum(player)' in practice
assert 'CubeDisplayMode' in storage and 'TileDisplayMode' in storage
assert 'CubeProgress' in storage and 'TileProgress' in storage  # migration preserved
assert 'ProgressDisplayMode::Loaded' in hud
assert 'g_progress_counters.loaded_valid' in hud
assert 'Collectibles' in practice and 'FertileLands' in practice
assert 'LevelIndex' in practice
assert 'total_cubes != kTotalCubes' in practice
assert 'enum class ProgressDisplayMode' in state
print('PASS: menu, migration, and live loaded-counter source wiring')

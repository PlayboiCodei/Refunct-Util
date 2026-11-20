"""Native button-triggered tile selection: static integration and safety checks.

The actual BP_Button_C -> BP_IslandChunk_C callback order can only be tested
inside Refunct v9.30 on Windows/x86.
"""
from pathlib import Path
root = Path(__file__).resolve().parents[1]
menu = (root/'src/menu.cpp').read_text(encoding='utf-8')
tiles = (root/'src/tiles.cpp').read_text(encoding='utf-8')
api = (root/'include/refunct/features/tiles.hpp').read_text(encoding='utf-8')
sections = (root/'src/sections.cpp').read_text(encoding='utf-8')
tick = (root/'src/game_tick.cpp').read_text(encoding='utf-8')

assert 'Prevent button tile selection:' in menu
assert 'IsButtonTileSelectionDisabled() ? L"ON" : L"OFF"' in menu
assert 'else if (i == 8) ToggleButtonTileSelection();' in menu
assert 'g_page == Page::WorldButtonActions && g_selection == 8' in menu
assert 'Prevent button presses from selecting tiles' in menu

for fn in ('IsButtonTileSelectionDisabled', 'ToggleButtonTileSelection',
           'BeginButtonTileSelectionGuard', 'EndButtonTileSelectionGuard',
           'UpdateButtonTileSelectionGuard'):
    assert fn in api and fn in tiles, fn
assert 'L"DisableButtonTileSelection"' in tiles
assert 'GetPrivateProfileIntW(' in tiles and 'QueueSettingsValue(' in tiles
assert 'g_section_button_contact_hook_available.load' in tiles

# Preserve the original button callback and block only native tile actors.
assert 'const bool guard_button_tiles = BeginButtonTileSelectionGuard(actor, player);' in sections
assert sections.count('if (guard_button_tiles) EndButtonTileSelectionGuard();') == 3
assert 'g_original_add_based_character(actor, player);' in sections
assert 'TileActorValid(actor, nullptr)' in tiles
assert 'ShouldBlockPhysicalTileContact(actor, player)' in sections
assert 's_button_tile_guard.button_callback_depth > 0' in tiles
assert 'g_practice_button_actors.end(), actor)' in tiles
assert 'IsSyntheticPracticeButtonContact()' in tiles

# Post-press snapshots preserve real gameplay state and never fabricate a tile
# count: rollback changes material, native Color, and the corresponding counter.
assert 'candidate.count != kTotalColourableTiles' in tiles
assert 'ResolveTileDeselectAbi(candidate)' in tiles
assert 'if (selected != state->platforms) return false;' in tiles
assert 'guard.old_colours[i] == 0' in tiles
assert 's_recent_tile_touches' in tiles and 'kSameContactWindowMs = 100' in tiles
assert 'guard.old_colours[index] = 0;' in tiles
assert 'if (selected_total != state->platforms)' in tiles
assert 'DeselectOneTile(actor, guard.tiles, before)' in tiles
assert 'RecordRandomTileTransition(actor, false);' in tiles
assert 'state->platforms = before.platforms - 1;' in tiles

# Cancel pending work on map replacement / restart / player change. Defer any
# reversal until button progress really advances, then wait one frame.
assert 'state->resets != guard.resets' in tiles
assert 'player != guard.player' in tiles
assert 'g_restart_requested || g_full_level_reload_requested' in tiles
assert 'state->buttons == guard.before_buttons' in tiles
assert 'if (!guard.button_progress_observed)' in tiles
assert 'now - guard.started_at > 750' in tiles
assert 'UpdateButtonTileSelectionGuard(player);' in tick
assert tick.index('UpdateButtonTileSelectionGuard(player);') < tick.index('UpdateTileColourPresets(player);')
print('PASS: button tile selection toggle, callback, safety and HUD descriptions')

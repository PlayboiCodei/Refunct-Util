"""Guard immediate post-button visual refresh without touching gameplay state.

This checks source wiring and bounded work; live engine frame times require
an in-game build. Nothing here simulates Unreal rendering.
"""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
code = (root / 'src/tiles.cpp').read_text(encoding='utf-8')
header = (root / 'include/refunct/features/tiles.hpp').read_text(encoding='utf-8')

# Use the already-verified game progress counter: a button can activate tiles
# through Blueprint without invoking the tile-contact callback.
update = code.split('void UpdateTileColourPresets(MyCharacter* player) {', 1)[1].split('\n}', 1)[0]
for token in (
    'state->buttons > s_tile_palette.observed_button_count',
    's_tile_palette.button_refresh_remaining = s_tile_palette.count;',
    's_tile_palette.button_refresh_passes = 3;',
    's_tile_palette.button_refresh_remaining == 0) return;',
    'RefreshTilesAfterButtonProgress();',
    'ProcessRecentNativeTileContacts(selected, unselected);',
):
    assert token in update, token
assert update.index('RefreshTilesAfterButtonProgress();') < update.index(
    'if (now < next_palette_update_ms && s_tile_colour_dirty_remaining == 0) return;')
assert update.index('ProcessRecentNativeTileContacts(selected, unselected);') < update.index(
    'RefreshTilesAfterButtonProgress();')

# No pre-existing change is treated as a new button press on cache rebuild.
assert 'candidate.observed_button_count = state->buttons;' in code
assert 's_tile_palette.button_refresh_remaining = 0;' in update
assert 's_tile_palette.button_refresh_passes = 0;' in update

# Scans are cached and CPU-budgeted. No engine object-table walk or MIDs are
# created here; existing contact queue applies at most one material per tick.
scan = code.split('void RefreshTilesAfterButtonProgress() {', 1)[1].split('\n}', 1)[0]
assert 'constexpr int kMaxChecksPerTick = 96;' in scan
assert 'ColourWorkBudget budget(750);' in scan
assert 'if (checked > 0 && budget.expired()) break;' in scan
assert 'ReadTileColor(actor, 0x02C4, native_colour,' in scan
assert 'NotifyNativeTileContactCompleted(actor, s_tile_palette.player);' in scan
assert 'random_colours[index].was_selected != (native_colour != 0)' in scan
assert 'last_applied >= 0 && last_applied != native_colour' in scan
assert 's_tile_palette.button_refresh_passes > 1' in scan
for forbidden in (
    'g_process_event(', 'CreateTileMID(', 'SetMeshMaterialVerified(',
    'ApplyTilePaletteVisual(', 'FindReflectedFunction(', 'WritePrivateProfileStringW(',
    'g_recording_tainted', 'g_run_pb_eligible', 'Log(',
):
    assert forbidden not in scan, forbidden

assert 'void NotifyNativeTileContactCompleted(void* actor, MyCharacter* player);' in header
assert 'constexpr int kRandomChecksPerUpdate = 32;' in code
assert 'ColourWorkBudget palette_budget(1100);' in code
print('PASS: button-driven tile refresh is prompt, bounded, and visual-only')

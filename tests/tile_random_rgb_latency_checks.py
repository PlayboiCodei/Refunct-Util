"""Source-level safeguards for low-latency, hitch-resistant tile colouring.

These checks verify integration and bounded game-thread work. They do not
measure Refunct frame times or validate the Windows/x86 runtime.
"""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
tiles = (root / 'src/tiles.cpp').read_text(encoding='utf-8')
sections = (root / 'src/sections.cpp').read_text(encoding='utf-8')
header = (root / 'include/refunct/features/tiles.hpp').read_text(encoding='utf-8')

assert 'void NotifyNativeTileContactCompleted(void* actor, MyCharacter* player)' in header
assert 'void NotifyNativeTileContactCompleted(void* actor, MyCharacter* player)' in tiles
contact = sections.split('const auto invoke_native = [&]() {', 1)[1].split('    };', 1)[0]
assert contact.index('g_original_add_based_character(actor, player);') < contact.index(
    'NotifyNativeTileContactCompleted(actor, player);')

native = tiles.split('void NotifyNativeTileContactCompleted(', 1)[1].split('\n}', 1)[0]
for text in ('s_tile_palette.cached', 's_tile_palette.tile_class',
             's_recent_native_tile_count', 'visual_active'):
    assert text in native, text
for forbidden in ('CreateTileMID(', 'SetMidColour(', 'g_process_event(',
                  'Log(', 'WritePrivateProfileStringW(', 'std::vector'):
    assert forbidden not in native, forbidden

process = tiles.split('void ProcessRecentNativeTileContacts(', 1)[1].split('\n}', 1)[0]
assert 'ReadTileColor(' in process and 'RecordRandomTileTransition(' in process
assert 'pending.attempts < 3' in process
assert 'ApplyTilePaletteVisual(' in process

update = tiles.split('void UpdateTileColourPresets(', 1)[1].split('\n}', 1)[0]
assert update.index('ProcessRecentNativeTileContacts(selected, unselected);') < update.index(
    'if (now < next_palette_update_ms && s_tile_colour_dirty_remaining == 0) return;')
assert 'constexpr int kPerTick = 20;' in update
assert 'ColourWorkBudget palette_budget(1100);' in update
assert 'applied_native_colours[index] == native_colour' in update
assert 'applied_revisions[index] == s_tile_visual_revision' in update
assert 'if (now < next_palette_update_ms && s_recent_native_tile_count == 0 &&' in update
assert 'if (now >= next_random_fallback_ms)' in update

fallback = tiles.split('void DetectRandomTileSelections(', 1)[1].split('\n}', 1)[0]
assert 'constexpr int kRandomChecksPerUpdate = 32;' in fallback
assert 'constexpr int kMaxImmediateApplies = 1;' in fallback

# The validated UE reflection is cached by class; no unvalidated fixed RVAs.
assert 'struct GetMaterialCache' in tiles and 'struct SetMaterialCache' in tiles
assert 'struct TintCallCache' in tiles
assert 'struct FactoryCallCache' in tiles
assert 'static std::array<FactoryCallCache, 3> calls{};' in tiles
assert 'PropertyMatches(function,' in tiles and 'PropertyMatches(fn,' in tiles
assert 'InvalidateTileVisuals();' in tiles
toggle = tiles.split('void ToggleRandomTileRGB()', 1)[1].split('\n}', 1)[0]
assert 's_tile_palette.player == g_player' in toggle
assert 's_tile_palette.resets == state->resets' in toggle
assert 'SnapshotRandomTileBaseline(s_tile_palette)' in toggle
assert 'else s_tile_palette.cached = false;' in toggle

print('PASS: event-driven RGB, bounded fallback, material-call caches, and dirty checks')

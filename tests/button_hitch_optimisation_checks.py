"""Source-level regression checks for button-press latency optimisations.

Not a substitute for Windows x86 compilation or an in-game frametime trace.
"""
from pathlib import Path
root = Path(__file__).resolve().parents[1]
records = (root / 'src/records.cpp').read_text(encoding='utf-8')
sections = (root / 'src/sections.cpp').read_text(encoding='utf-8')
tiles = (root / 'src/tiles.cpp').read_text(encoding='utf-8')

# New golds are committed to memory and queued; the worker owns slow disk I/O.
start = records.index('GoldUpdate UpdateBestSegment(')
end = records.index('void FinishPersonalBestIfNeeded()', start)
gold = records[start:end]
assert 'QueueGoldFileWrite(next)' in gold
assert 'FlushFileBuffers(' not in gold
assert 'CopyFileW(' not in gold
assert 'Log((L"Fastest segment saved' not in gold
worker = records[records.index('DWORD WINAPI GoldFileWriterMain('):
                 records.index('void StartGoldFileWriter(')]
assert 'WritePersonalBestSnapshot(path, snapshot)' in worker
assert 'writer->pending = false;' in worker
assert 'SetEvent(writer->idle)' in worker
assert 'Sleep(100)' in worker
assert 'return DrainGoldFileWriter() && WritePersonalBestSnapshot(g_pb_path, best);' in records
assert 'if (!DrainGoldFileWriter() || !DrainFastestSegmentGhostWrites()) return false;' in records
assert 'StartGoldFileWriter();' in records[records.index('void LoadPersonalBest()'):]

# Reflected colouring work is spread across frames rather than done 37-at-once.
colours = sections[sections.index('void UpdateButtonColours()'):]
assert 'constexpr int kButtonsPerBatch = 6;' in colours
assert 's_button_tint_next_tick = now + (urgent ? 0 : 200);' in colours
assert 'ColourWorkBudget tint_budget(1000);' in colours
assert 's_button_colour_dirty_remaining' in colours
assert 's_button_tint_cursor = (s_button_tint_cursor + 1) % kRefunctButtonCount;' in colours

# Button tile guards retain the verified map, but inspect fresh native colours
# each button contact. Layout is invalidated across resets/player/map changes.
assert 'bool PrepareButtonTileLayout(MyCharacter* player)' in tiles
assert 's_button_tile_layout.resets == state->resets' in tiles
assert 's_button_tile_layout.level_roots[i] != g_practice_level_actors[i]' in tiles
snapshot = tiles[tiles.index('bool SnapshotButtonTiles('):tiles.index('bool SetMeshMaterialVerified(')]
assert 'const TileBatch& candidate = s_button_tile_layout;' in snapshot
assert 'ReadTileColor(candidate.actors[i], candidate.color_offset, colour,' in snapshot
assert 'candidate.tile_class)' in snapshot
assert 'FindFertileLandsOffset' not in snapshot

# Random tile RGB scans are now bounded each refresh.
random = tiles[tiles.index('void DetectRandomTileSelections('):tiles.index('} // namespace', tiles.index('void DetectRandomTileSelections('))]
assert 'constexpr int kRandomChecksPerUpdate = 32;' in random
assert 'random_scan_cursor' in random
print('PASS: asynchronous gold saves, bounded colour work, and tile layout cache')

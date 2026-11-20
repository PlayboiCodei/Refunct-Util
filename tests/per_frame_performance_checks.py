"""Timing-source guards; Windows x86 and live frametimes remain unverified."""
from pathlib import Path
root = Path(__file__).resolve().parents[1]
tubes = (root / 'src/tubes.cpp').read_text(encoding='utf-8')
tiles = (root / 'src/tiles.cpp').read_text(encoding='utf-8')
platforms = (root / 'src/platforms.cpp').read_text(encoding='utf-8')
jumppads = (root / 'src/jumppads.cpp').read_text(encoding='utf-8')
budget = (root / 'include/refunct/colour_work_budget.hpp').read_text(encoding='utf-8')
assert 'if (!setting.enabled && !s_restore_pending) return;' in tubes
assert 's_pending_apply == 0 && s_scan_cursor < 0 && now < s_next_colour_work' in tubes
assert 'QueueTubeColourRefresh(); // No global UObject rescan for a known tube.' in tubes
assert 'ColourWorkBudget scan_budget(800)' in tubes
assert 'ColourWorkBudget apply_budget(1000)' in tubes
assert 's_next_colour_work = GetTickCount64() + (s_pending_apply ? 0 : 650);' in tubes
assert 'if (now < next_palette_update_ms && s_tile_colour_dirty_remaining == 0) return;' in tiles
assert tiles.index('ProcessRecentNativeTileContacts(selected, unselected);') < tiles.index(
    'if (now < next_palette_update_ms && s_tile_colour_dirty_remaining == 0) return;')
assert 'constexpr int kPerTick = 20;' in tiles
assert 'ColourWorkBudget palette_budget(1100)' in tiles
for file in (platforms, jumppads):
    assert 'kScanBatch = 2048' in file
    assert 'ColourWorkBudget budget(700)' in file
    assert 's_pending_colour' in file
    assert 'ColourWorkBudget apply_budget(1100)' in file
assert 'QueryPerformanceCounter' in budget
assert 'QueryPerformanceFrequency' in budget
print('PASS: bounded faster scans, cached updates and immediate dirty paths')

"""Static wiring checks for per-button ghost saves. Runtime behaviour is covered
by ghost_fastest_runtime_test.py and portable CTest ghost_data_tests.cpp.
"""
from pathlib import Path
root=Path(__file__).resolve().parents[1]
read=lambda name:(root/name).read_text(encoding='utf-8')
records=read('src/records.cpp')
ghost=read('src/ghost.cpp')
header=read('include/refunct/features/ghost.hpp')
menu=read('src/menu.cpp')
assert 'QueueFastestSegmentGhost(button, observed_elapsed - segment,' in records
assert records.index('result.new_gold = true;') < records.index('QueueFastestSegmentGhost(') < records.index('void FinishPersonalBestIfNeeded()')
assert 'UpdatePBGhostRecording(player);' in read('src/game_tick.cpp')
assert 'ghost_data::ExtractSegment(' in ghost
assert 's_fastest[index] = std::move(track);' in ghost
assert 'StartFastestGhostWriter();' in ghost
assert 'CopyFastestSegmentGhosts(destination);' in records
assert 'DrainFastestSegmentGhostWrites()' in header
assert 's_fastest[static_cast<std::size_t>(button)]' in ghost
assert 'if (s_playback.empty()) return false;' in ghost # fallback only
assert 'Ghost recordings:' not in menu
assert 'No ghost yet: reach Button 1 on a clean run' not in menu
assert read('src/ghost.cpp').index('for (int button = 1; button <= kRefunctButtonCount; ++button)') < read('src/ghost.cpp').index('const std::uint32_t pb_ms = Milliseconds(g_pb.total_seconds);')
print('PASS: fastest segments captured after golds, saved asynchronously, loaded without PB and copied into snapshots')

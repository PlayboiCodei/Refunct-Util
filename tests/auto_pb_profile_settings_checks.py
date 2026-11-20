"""Source integration assertions for opt-in automatic PB profile snapshots."""
from pathlib import Path
root = Path(__file__).resolve().parents[1]
read = lambda file: (root / file).read_text(encoding='utf-8')
state = read('include/refunct/state.hpp')
definitions = read('src/state.cpp')
settings = read('src/storage.cpp')
menu = read('src/menu.cpp')
records = read('src/records.cpp')
tick = read('src/game_tick.cpp')
cmake = read('CMakeLists.txt')
for name in ('g_auto_create_pb_profile', 'g_auto_pb_copy_best_segments'):
    assert 'extern bool ' + name + ';' in state
    assert name in definitions and name in settings and name in menu and name in records
assert 'bool g_auto_create_pb_profile = false;' in definitions
assert 'bool g_auto_pb_copy_best_segments = true;' in definitions
assert 'L"AutoCreatePBProfile", 0,' in settings
assert 'L"CopyBestSegmentsToPBProfile", 1,' in settings
assert 'write(L"Recording", L"AutoCreatePBProfile",' in settings
assert 'write(L"Recording", L"CopyBestSegmentsToPBProfile",' in settings
assert 'Create profile on new PB:' in menu
assert 'Copy fastest segments to new profile:' in menu
assert 'g_selection == 5' in menu
assert 'g_selection == 6' in menu
assert 'else if (i == 5) {' in menu and 'else if (i == 6) {' in menu
assert 'SaveAutomaticPBProfile(next);' in records
assert records.index('CommitPBGhostOnNewPB(next.total_seconds);') < records.index('SaveAutomaticPBProfile(next);')
assert records.index('if (!SavePersonalBestFile(next))',records.index('void FinishPersonalBestIfNeeded()')) < records.index('SaveAutomaticPBProfile(next);')
assert 'WritePersonalBestSnapshot(destination, snapshot, false)' in records
assert 'MOVEFILE_REPLACE_EXISTING : 0' in records
assert 'snapshot.best_segment_sources.fill(SegmentSource::Unknown);' in records
assert 'ScanProfiles(false);' in records
assert 'UpdatePBGhostRecording(player);' in tick
assert 'src/ghost.cpp' in cmake
assert '#include "refunct/features/ghost.hpp"' in read('include/refunct/internal_api.hpp')
print('PASS: auto PB settings, menu rows, transactional snapshot, Ghost integration')

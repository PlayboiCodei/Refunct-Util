"""Exercise production ghost save guards in an inert portable C++ harness.

C++ mock permissions represent records.cpp's existing cheat mode gates.
No Win32 file writes or in-game claims are made by this test.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
compiler = shutil.which('g++') or shutil.which('clang++')
assert compiler, 'portable C++ compiler required'
source = (root / 'src/ghost.cpp').read_text(encoding='utf-8')
preamble = (root / 'tests/ghost_native_mock_preamble.hpp').read_text(encoding='utf-8')
source = 'namespace refunct {' + source.split('namespace refunct {', 1)[1]

main = r'''
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
int main() {
    using namespace refunct;
    s_loaded_settings = true;
    g_save_pb_ghosts = true;
    g_save_fastest_ghosts = true;
    g_mock_pb_record_allowed = true;
    g_mock_segment_record_allowed = true;
    g_pb.best_segment_seconds[0] = 1.0;
    s_capture.started = true;
    s_capture.usable = true;
    s_capture.samples = {{0,0,0,88,0}, {1000,0,0,88,0}};

    // Segment recording permitted: pending sample should be queued.
    QueueFastestSegmentGhost(1,0.0,1.0,true);
    assert(s_pending_fastest[0].ready);
    // If disabled before committing, the pending write must be cancelled.
    g_save_fastest_ghosts = false;
    CommitPendingFastestGhosts();
    assert(!s_pending_fastest[0].ready);
    // OFF prevents queuing a new gold ghost even with cheat permission.
    QueueFastestSegmentGhost(1,0.0,1.0,true);
    assert(!s_pending_fastest[0].ready);
    g_save_fastest_ghosts = true;
    g_mock_segment_record_allowed = false; // Save with cheats excludes segments.
    QueueFastestSegmentGhost(1,0.0,1.0,true);
    assert(!s_pending_fastest[0].ready);
    // Save with cheats: Fastest segments only or Both => allowed.
    g_mock_segment_record_allowed = true;
    QueueFastestSegmentGhost(1,0.0,1.0,true);
    assert(s_pending_fastest[0].ready);
    s_pending_fastest[0] = {};
    // Time-only segments and synthesized runs can never produce ghost data.
    QueueFastestSegmentGhost(1,0.0,1.0,false);
    assert(!s_pending_fastest[0].ready);
    // PB OFF clears previous in-memory playback and never claims a fresh save.
    s_playback = {{0,0,0,88,0}, {1000,0,0,88,0}};
    g_save_pb_ghosts = false;
    CommitPBGhostOnNewPB(1.0);
    assert(s_playback.empty() && !PBGhostSavedForLatestPB());
    // PB cheat permission OFF also blocks new PB ghosts independently of toggle.
    g_save_pb_ghosts = true;
    g_mock_pb_record_allowed = false;
    CommitPBGhostOnNewPB(1.0);
    assert(!PBGhostSavedForLatestPB());
    // OFF PB ghost toggle prevents furthest-run recordings too.
    g_last_button_count = 1;
    g_run_started_at_zero = true;
    g_save_pb_ghosts = false;
    s_furthest_checked_button = 0;
    CommitFurthestGhostAtButton();
    assert(s_furthest_checked_button == 1);
    // When Save with cheats excludes PBs, furthest ghosts are also blocked.
    g_save_pb_ghosts = true;
    g_mock_pb_record_allowed = false;
    s_furthest_checked_button = 0;
    CommitFurthestGhostAtButton();
    assert(s_furthest_checked_button == 1);
    // Never count these switches as gameplay cheats or modify replay data.
    assert(g_pb.best_segment_seconds[0] == 1.0);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='refunct_ghost_record_permissions_') as d:
    path = Path(d) / 'test.cpp'
    exe = Path(d) / 'test'
    path.write_text(preamble + source + main, encoding='utf-8')
    subprocess.run([compiler,'-std=c++20','-O2','-Wall','-Wextra','-Werror',
                    '-I',str(root/'include'),str(path),'-o',str(exe)],check=True,timeout=50)
    subprocess.run([str(exe)],check=True,timeout=10)
print('PASS: live PB/furthest/gold ghost save toggles, record cheat permissions, no synthetic ghost')

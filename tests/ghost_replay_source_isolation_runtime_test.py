"""Compile production replay playback with live-data mocks and verify PB vs gold.

No furthest-run tracks may be used by either Replay Viewer source. Existing
non-replay ghost fallback should remain unchanged.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
source=(root/'src/ghost.cpp').read_text()
menu=(root/'src/menu.cpp').read_text()
header=(root/'include/refunct/features/ghost.hpp').read_text()
prelude=(root/'tests/ghost_native_mock_preamble.hpp').read_text()
assert 'void ToggleGhostReplaySource();' in header
assert 'L"Source: " + GhostReplaySourceLabel() + L"  < >"' in menu
assert 'else if (i == 2) ToggleGhostReplaySource();' in menu
assert 'else if (g_selection == 2) ToggleGhostReplaySource();' in menu
assert 'bool allow_furthest = true' in source
assert 'false, false)) return false;' in source
assert 's_replay_best_segments = GetPrivateProfileIntW' in source
main=r'''
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
using namespace refunct;
int main(){
    s_loaded_settings=true;
    s_loaded_profile=g_pb_path;
    s_replay_best_segments=false;
    s_use_fastest_sections=true; // The normal ghost's choice must not affect replay.
    g_pb.total_seconds=1.0;
    for(int i=0;i<kRefunctButtonCount;++i)
        g_pb.button_seconds[i]=(i+1)/37.0;
    s_playback={{0,0,0,0,0},{1000,100,100,100,0}};
    s_furthest.reached=2;
    s_furthest.samples={{0,777,0,0,0},{1000,888,0,0,0}};
    s_furthest.button_ms[0]=500;
    s_furthest.button_ms[1]=1000;
    g_mock_level_state.buttons=0;
    assert(GhostReplaySourceLabel()==L"PB");
    assert(StartGhostReplayViewer());
    assert(!s_replay.fastest);
    GhostPlayback playback{};
    assert(SelectGhostPlayback(playback) && playback.track==&s_playback);
    // Switching source terminates existing replay safely, cancels pending restart.
    assert(g_restart_requested);
    ToggleGhostReplaySource();
    assert(!IsGhostReplayRequested());
    assert(!g_restart_requested);
    assert(GhostReplaySourceLabel()==L"Best segments");
    // Gold segment 1, PB segment 2. Do NOT use the available furthest run.
    s_fastest[0]={{0,4,4,4,0},{800,5,5,5,0}};
    g_pb.best_segment_seconds[0]=0.8;
    assert(StartGhostReplayViewer());
    assert(s_replay.fastest);
    assert(s_replay.duration_ms>800);
    assert(SelectGhostPlayback(playback) && playback.track==&s_fastest[0]);
    s_replay.elapsed_ms=815;
    assert(SelectGhostPlayback(playback));
    assert(playback.track==&s_playback && !playback.furthest);
    assert(!playback.fastest);
    StopGhostReplayViewer();
    // Completed PB is strictly required for PB source; never use furthest.
    ToggleGhostReplaySource();
    assert(GhostReplaySourceLabel()==L"PB");
    s_playback.clear();
    assert(!StartGhostReplayViewer());
    // Best segments also cannot use furthest to fill a missing section.
    ToggleGhostReplaySource();
    s_fastest[0].clear();
    assert(!StartGhostReplayViewer());
    assert(!IsGhostReplayRequested());
    assert(s_furthest.samples.size()==2); // Existing saved furthest data unchanged.
    return 0;
}
'''
compiler=shutil.which('g++') or shutil.which('clang++')
assert compiler
with tempfile.TemporaryDirectory(prefix='refunct_replay_src_') as td:
    cpp=Path(td)/'test.cpp';exe=Path(td)/'test'
    cpp.write_text(prelude + 'namespace refunct {' + source.split('namespace refunct {',1)[1] + main)
    subprocess.run([compiler,'-std=c++20','-O2','-Wall','-Wextra','-Werror',
                    '-I',str(root/'include'),str(cpp),'-o',str(exe)],check=True,timeout=100)
    subprocess.run([str(exe)],check=True,timeout=12)
print('PASS: explicit PB, separate saved source, gold+PB mixing, no furthest fallback, safe live switch')

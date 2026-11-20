"""Native mock of saved-ghost spectator playback and safe stop controls.

Camera spawning and SetViewTargetWithBlend require Refunct's Windows x86 UE4.14
runtime; the C++ mock validates isolated playback clocks and source selection.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
compiler = shutil.which('g++') or shutil.which('clang++')
assert compiler
source = (root / 'src/ghost.cpp').read_text(encoding='utf-8')
prelude = (root / 'tests/ghost_native_mock_preamble.hpp').read_text(encoding='utf-8')
assert 'SetViewTargetWithBlend' in source
assert 'GetViewTarget' in source
assert 'StopGhostReplayInternal()' in source
assert 'ReplayCameraLive()' in source
assert 'if (s_replay.requested || s_replay.active)' in source
assert 'UpdateGhostReplayViewer(player);' in (root/'src/game_tick.cpp').read_text(encoding='utf-8')
assert 'case Page::GhostReplay:' in (root/'src/menu.cpp').read_text(encoding='utf-8')
assert 'case Page::GhostReplay: SetPage(Page::Main);' in (root/'src/menu.cpp').read_text(encoding='utf-8')
main=r'''
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
int main() {
    using namespace refunct;
    s_loaded_profile = g_pb_path;
    s_playback = {{0,1,2,3,90},{1000,10,20,30,100}};
    s_show=false; s_pass_through=false; s_use_fastest_sections=false;
    g_pb.total_seconds = 1.0;
    for(int i=0;i<kRefunctButtonCount;++i)
        g_pb.button_seconds[i] = (i+1)/37.0;
    g_mock_level_state.buttons = 0;

    g_engine_timer_valid = false; // Replay bypasses live run-clock gating.
    g_timer_complete = true;
    assert(StartGhostReplayViewer());
    assert(IsGhostReplayRequested());
    assert(!s_replay.active && s_replay.requested);
    assert(s_replay.duration_ms == 1000);
    GhostPlayback selection{};
    assert(SelectGhostPlayback(selection));
    assert(selection.track == &s_playback && selection.elapsed_ms == 0);
    SeekGhostReplaySeconds(9999);
    assert(s_replay.elapsed_ms == 1000);
    SeekGhostReplaySeconds(-9999);
    assert(s_replay.elapsed_ms == 0);
    assert(GhostReplaySpeedLabel() == L"1x");
    CycleGhostReplaySpeed(-5);
    assert(GhostReplaySpeedLabel() == L"0.5x");
    CycleGhostReplaySpeed(+5);
    assert(GhostReplaySpeedLabel() == L"2x");
    RestartGhostReplay();
    assert(s_replay.elapsed_ms == 0);
    // Start is also the stop command and must not touch saved samples.
    assert(StartGhostReplayViewer());
    assert(!IsGhostReplayRequested());
    assert(s_playback.size() == 2 && s_playback.back().time_ms == 1000);
    assert(!SelectGhostPlayback(selection)); // live clock remains invalid

    // Replay selection is independent from the ordinary HUD ghost source.
    s_use_fastest_sections=true;
    assert(GhostReplaySourceLabel()==L"PB");
    ToggleGhostReplaySource();
    assert(GhostReplaySourceLabel()==L"Best segments");
    g_pb.best_segment_seconds[0] = 0.8;
    s_fastest[0] = {{0,3,3,3,45},{800,6,6,6,55}};
    assert(StartGhostReplayViewer());
    assert(s_replay.fastest && s_replay.duration_ms >= 800);
    assert(SelectGhostPlayback(selection));
    assert(selection.track == &s_fastest[0] && selection.fastest);
    SeekGhostReplaySeconds(1);
    assert(SelectGhostPlayback(selection));
    assert(selection.track && selection.elapsed_ms <= selection.track->back().time_ms);
    StopGhostReplayViewer();
    assert(!IsGhostReplayRequested());
    // No source: graceful refusal, no changes to existing PB samples.
    s_playback.clear(); s_fastest[0].clear(); s_furthest = {};
    assert(!StartGhostReplayViewer());
    assert(!IsGhostReplayRequested());
    // Completed real level: replay auto-queues R restart and waits until
    // the new native LevelState confirms button zero before spawning camera.
    s_use_fastest_sections=false;
    s_playback={{0,1,2,3,90},{1000,10,20,30,100}};
    g_mock_level_state.buttons=37;
    g_mock_level_state.resets=3;
    assert(StartGhostReplayViewer());
    assert(g_restart_requested && s_replay.waiting_reset);
    assert(s_replay.button_timeline.count==37);
    SeekGhostReplaySeconds(1);
    assert(s_replay.resume_at_ms==1000);
    RestartGhostReplay();
    assert(s_replay.resume_at_ms==0 && s_replay.waiting_reset);
    PlayerController controller{};
    MyCharacter new_player{};
    new_player.controller=&controller;
    g_restart_requested=false;
    g_mock_level_state.buttons=0;
    g_mock_level_state.resets=4;
    UpdateGhostReplayViewer(&new_player);
    assert(!s_replay.waiting_reset && s_replay.requested);
    StopGhostReplayViewer();
    assert(!IsGhostReplayRequested());
    return 0;
}
'''
source = 'namespace refunct {'+source.split('namespace refunct {',1)[1]
with tempfile.TemporaryDirectory(prefix='ghost_replay_viewer_') as td:
    src=Path(td)/'ghost_replay.cpp'
    exe=Path(td)/'ghost_replay'
    src.write_text(prelude+source+main,encoding='utf-8')
    subprocess.run([compiler,'-std=c++20','-O2','-Wall','-Wextra','-Werror',
                    '-I',str(root/'include'),str(src),'-o',str(exe)],check=True, timeout=100)
    subprocess.run([str(exe)],check=True,timeout=10)
print('PASS: replay source selection, independent clock, seek/speed, stop, no-source safety')

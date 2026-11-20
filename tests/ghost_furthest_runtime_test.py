"""Compile production Ghost code with fake game ABI: no PB and no new golds."""
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
compiler = shutil.which('g++') or shutil.which('clang++')
if not compiler:
    print('SKIP: C++20 compiler unavailable')
    raise SystemExit(0)
source = (root/'src/ghost.cpp').read_text()
source = 'namespace refunct {' + source.split('namespace refunct {',1)[1]
preamble = (root/'tests/ghost_native_mock_preamble.hpp').read_text()
main = r'''
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <string>
int main() {
    using namespace refunct;
    SceneComponent root{{0.f,0.f,0.f}};
    PlayerController controller{{0,0,0}};
    MyCharacter player{&root,&controller};
    s_loaded_settings = true;
    s_loaded_profile = g_pb_path;
    s_show = true;
    s_button_sync = true;
    s_playback.clear();
    g_pb.total_seconds = -1;
    g_pb.best_segment_seconds[0] = 1.2; // Existing golds cannot be beaten.
    g_pb.best_segment_seconds[1] = 1.3;
    g_pb.best_segment_seconds[2] = 1.4;
    FastestGhostWriter worker{};
    worker.wake = reinterpret_cast<HANDLE>(1);
    worker.idle = reinterpret_cast<HANDLE>(1);
    s_fastest_writer = &worker;
    g_last_run_start = 100;
    g_last_button_count = 0;
    g_elapsed_seconds = 0;
    UpdatePBGhostRecording(&player);
    for (int step=1; step<=80; ++step) {
        g_elapsed_seconds = step*.05;
        root.location.x = step;
        if (step == 40 || step == 80) {
            g_last_button_count++;
            g_button_splits.push_back({g_last_button_count, g_elapsed_seconds});
        }
        UpdatePBGhostRecording(&player);
    }
    assert(s_furthest.reached==2);
    assert(PBGhostRecordingStatus()==L"FURTHEST BUTTON 2");
    assert(worker.queue.size()==2);
    assert(worker.queue.back().path==ghost_data_paths::PB(g_storage_dir,g_pb_path));
    ghost_furthest_data::Run<kRefunctButtonCount> decoded;
    assert(worker.queue.back().bytes.empty()); // Immutable raw snapshot queued.
    assert(EncodeFastestGhostWrite(worker.queue.back()));
    assert(ghost_furthest_data::Deserialize(worker.queue.back().bytes, decoded));
    assert(decoded.reached==2 && decoded.button_ms[0]==2000 && decoded.button_ms[1]==4000);

    // Restart. Run two reaches Button 3 but does not beat a single gold.
    ResetPBGhostCapture();
    g_button_splits.clear();
    g_last_button_count=0;
    g_last_run_start=200;
    g_elapsed_seconds=0;
    UpdatePBGhostRecording(&player);
    for (int step=1; step<=150; ++step) {
        g_elapsed_seconds=step*.05;
        root.location.x=step+500;
        if (step==50 || step==100 || step==150) {
            g_last_button_count++;
            g_button_splits.push_back({g_last_button_count,g_elapsed_seconds});
        }
        UpdatePBGhostRecording(&player);
    }
    assert(s_furthest.reached==3);
    assert(s_furthest.button_ms[2]==7500);
    assert(PBGhostRecordingStatus()==L"FURTHEST BUTTON 3");
    assert(worker.queue.size()==3); // 2 earlier + only the improved Button 3
    assert(EncodeFastestGhostWrite(worker.queue.back()));
    assert(ghost_furthest_data::Deserialize(worker.queue.back().bytes, decoded));
    assert(decoded.reached==3 && decoded.button_ms[2]==7500);
    // Source can be reloaded from the saved bytes with no completed PB.
    s_furthest={};
    assert(!HasPBGhostForActiveProfile());
    assert(EncodeFastestGhostWrite(worker.queue.back()));
    assert(ghost_furthest_data::Deserialize(worker.queue.back().bytes,s_furthest));
    assert(HasPBGhostForActiveProfile());
    const std::vector<ghost_data::Sample>* chosen=nullptr;
    std::uint32_t when=0;
    bool gold=false, furthest=false;
    g_last_button_count=2;
    g_button_splits={{1,2.5},{2,5.0}};
    g_elapsed_seconds=6;
    assert(ButtonSynchronizedPlaybackMs(when,chosen,gold,furthest));
    assert(chosen==&s_furthest.samples && furthest && !gold && when==6000);
    // Next, a run that reaches only Button 2 must never replace Button 3.
    ResetPBGhostCapture();
    g_button_splits.clear();
    g_last_button_count=0;
    g_last_run_start=300;
    g_elapsed_seconds=0;
    UpdatePBGhostRecording(&player);
    const auto old_size=worker.queue.size();
    for (int step=1; step<=60; ++step) {
        g_elapsed_seconds=step*.05;
        root.location.x=step+900;
        if (step==20 || step==60) {
            g_last_button_count++;
            g_button_splits.push_back({g_last_button_count,g_elapsed_seconds});
        }
        UpdatePBGhostRecording(&player);
    }
    assert(worker.queue.size()==old_size);
    assert(s_furthest.reached==3);
    // A valid PB is authoritative: button contacts must not overwrite it.
    s_primary_pb_protected = true;
    ResetPBGhostCapture();
    g_button_splits.clear();
    g_last_button_count=0;
    g_last_run_start=400;
    g_elapsed_seconds=0;
    UpdatePBGhostRecording(&player);
    const auto protected_writes=worker.queue.size();
    for (int step=1;step<=80;++step) {
        g_elapsed_seconds=step*.05;
        root.location.x=step+1100;
        if(step==40||step==80){++g_last_button_count;g_button_splits.push_back({g_last_button_count,g_elapsed_seconds});}
        UpdatePBGhostRecording(&player);
    }
    assert(worker.queue.size()==protected_writes);
    // A delayed first sample is still a genuine partial replay, not a
    // reason for NO SAVED GHOST and not grounds to fabricate t=0 movement.
    std::vector<ghost_data::Sample> late={{250,1,2,3,0}, {300,2,3,4,0},
                                          {500,3,4,5,0}};
    std::vector<ghost_data::Sample> prefix;
    assert(ghost_furthest_data::ExtractObservedPrefix(late,500,prefix));
    assert(prefix.size()==3 && prefix.front().time_ms==250);
    // Deliberately corrupt metadata: corrupted file must not be accepted.
    auto corrupted=worker.queue.back().bytes;
    corrupted[28]^=0xF;
    assert(!ghost_furthest_data::Deserialize(corrupted,decoded));
    return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    cpp = Path(directory)/'furthest.cpp'
    cpp.write_text(preamble + source + main)
    binary = Path(directory)/'furthest'
    subprocess.run([compiler,'-std=c++20','-Wall','-Wextra','-Werror','-O2',
                    '-I',str(root/'include'),str(cpp),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
print('PASS: no-gold partial runs, furthest priority, reload, button-synced fallback, corruption rejection')

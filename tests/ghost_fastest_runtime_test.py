"""Compile real ghost.cpp with inert Win32/UE mock and validate partial-run segment capture.
No Windows process or filesystem side effects.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
compiler=shutil.which('g++') or shutil.which('clang++')
if not compiler:
    print('SKIP: C++20 compiler unavailable')
    raise SystemExit(0)
source=(root/'src/ghost.cpp').read_text()
source='namespace refunct {'+source.split('namespace refunct {',1)[1]
preamble=(root/'tests/ghost_native_mock_preamble.hpp').read_text()
main=r'''
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
int main(){
    using namespace refunct;
    SceneComponent root{{0.f,0.f,0.f}};
    PlayerController controller{{0,0,0}};
    MyCharacter player{&root,&controller};
    s_loaded_settings=true;
    s_loaded_profile=g_pb_path;
    s_show=true;
    s_button_sync=true;
    s_playback.clear();
    g_pb.total_seconds=-1; // Never finished a full run.
    g_last_run_start=100;
    g_last_button_count=0;
    g_elapsed_seconds=0;
    UpdatePBGhostRecording(&player);
    assert(s_capture.usable);
    FastestGhostWriter mock_writer{};
    mock_writer.wake=reinterpret_cast<HANDLE>(1);
    mock_writer.idle=reinterpret_cast<HANDLE>(1);
    s_fastest_writer=&mock_writer;
    for(int step=1;step<=40;step++){
        g_elapsed_seconds=step*.05;
        root.location.x=step*2.f;
        if(step==40){
            g_last_button_count=1;
            g_button_splits.push_back({1,2.0});
            g_pb.best_segment_seconds[0]=2.0;
            QueueFastestSegmentGhost(1,0.0,2.0,true);
        }
        UpdatePBGhostRecording(&player);
    }
    assert(s_furthest.reached==1);
    assert(s_fastest[0].size()>30);
    assert(s_fastest[0].front().time_ms==0 && s_fastest[0].back().time_ms==2000);
    assert(HasPBGhostForActiveProfile());
    assert(mock_writer.queue.size()==2);
    assert(mock_writer.queue[0].path==ghost_data_paths::Gold(g_storage_dir,g_pb_path,1));
    std::vector<ghost_data::Sample> decoded;
    assert(mock_writer.queue[0].bytes.empty()); // Encoding has not run on the tick.
    assert(EncodeFastestGhostWrite(mock_writer.queue[0]));
    assert(ghost_data::Deserialize(mock_writer.queue[0].bytes,2000,decoded));
    assert(decoded.back().x==80.f);
    // Repeat on the same unfinished run, with no PB timestamp in g_pb.
    for(int step=41;step<=100;step++){
        g_elapsed_seconds=step*.05;
        root.location.x=step*2.f;
        if(step==100){
            g_last_button_count=2;
            g_button_splits.push_back({2,5.0});
            g_pb.best_segment_seconds[1]=3.0;
            QueueFastestSegmentGhost(2,2.0,5.0,true);
        }
        UpdatePBGhostRecording(&player);
    }
    assert(s_furthest.reached==2);
    assert(s_fastest[1].size()>30);
    assert(s_fastest[1].front().time_ms==0 && s_fastest[1].back().time_ms==3000);
    assert(mock_writer.queue.size()==4);
    assert(EncodeFastestGhostWrite(mock_writer.queue[2]));
    assert(ghost_data::Deserialize(mock_writer.queue[2].bytes,3000,decoded));
    assert(std::abs(decoded.front().x-80.f)<.01f && std::abs(decoded.back().x-200.f)<.01f);

    // Playback selects these tracks even though no completed-PB ghost exists.
    const std::vector<ghost_data::Sample>* chosen=nullptr;
    std::uint32_t when=0;
    bool gold=false, farthest=false;
    g_elapsed_seconds=1;
    g_last_button_count=0;
    assert(ButtonSynchronizedPlaybackMs(when,chosen,gold,farthest));
    assert(chosen==&s_fastest[0] && gold && when==1000);
    g_last_button_count=1;
    g_button_splits={{1,2.5}};
    g_elapsed_seconds=3.5;
    assert(ButtonSynchronizedPlaybackMs(when,chosen,gold,farthest));
    assert(chosen==&s_fastest[1] && when==1000);
    g_elapsed_seconds=99;
    assert(ButtonSynchronizedPlaybackMs(when,chosen,gold,farthest) && when==3000);

    // A new gold from practice invalidates the old movement track; timing
    // cannot be used to fabricate a ghost, even in the current session.
    g_pb.best_segment_seconds[1]=2.8;
    QueueFastestSegmentGhost(2,2.5,5.3,false);
    assert(s_fastest[1].empty());
    assert(ButtonSynchronizedPlaybackMs(when,chosen,gold,farthest));
    assert(chosen==&s_furthest.samples && farthest);
    // Recorded golds survive capture resets and remain playable.
    ResetPBGhostCapture();
    assert(!s_fastest[0].empty());
    return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    src=Path(directory)/'test.cpp'
    src.write_text(preamble+source+main)
    binary=Path(directory)/'test'
    subprocess.run([compiler,'-std=c++20','-Wall','-Wextra','-Werror','-O2',
                    '-I',str(root/'include'),str(src),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
print('PASS: 2 fastest ghosts captured and queued from incomplete run, no PB required')

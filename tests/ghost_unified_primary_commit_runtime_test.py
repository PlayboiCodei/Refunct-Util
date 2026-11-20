"""Compile the production PB commit function against mock files and a queued partial write."""
from pathlib import Path
import shutil
import subprocess
import tempfile

root=Path(__file__).resolve().parents[1]
source=(root/'src/ghost.cpp').read_text('utf-8')
start=source.index('void CommitPBGhostOnNewPB(double run_seconds) {')
depth=0
end=None
for i in range(source.index('{',start),len(source)):
    if source[i]=='{': depth+=1
    elif source[i]=='}':
        depth-=1
        if depth==0:
            end=i+1
            break
assert end
fn=source[start:end]
assert fn.index('DrainFastestSegmentGhostWrites()') < fn.index('WriteGhostFile(GhostPath(')
compiler=shutil.which('g++') or shutil.which('clang++')
if not compiler:
    print('SKIP: no C++20 compiler')
    raise SystemExit(0)

code = r'''
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
#include "refunct/ghost_data.hpp"
#include "refunct/ghost_furthest_data.hpp"
#include "refunct/ghost_data_paths.hpp"
namespace refunct {
constexpr int kRefunctButtonCount=37;
struct Capture {
    bool started=true, usable=true, complete=true;
    std::vector<ghost_data::Sample> samples={{0,0,0,0,0},{200,1,2,3,45}};
};
Capture s_capture;
ghost_furthest_data::Run<kRefunctButtonCount> s_furthest;
std::vector<ghost_data::Sample> s_playback;
bool s_pb_ghost_saved_for_latest_pb=false;
bool s_primary_pb_protected=false,s_primary_ghost_invalid=false;
bool g_save_pb_ghosts=true,g_practice_active=false,g_preloaded_restart_active=false,g_progress_restart_active=false;
std::wstring g_storage_dir=L"RefunctUtility";
std::wstring g_pb_path=L"RefunctUtility\\Profiles\\Any%\\Splits\\records.ini";
std::wstring g_active_profile=L"Any%";
std::vector<std::uint8_t> disk, pending;
std::vector<std::wstring> logs;
bool drain_failure=false,write_failure=false,allow_records=true;
int drain_calls=0, write_calls=0;
struct MockCache {int value=0;};
MockCache s_draw_cache;
void Log(const wchar_t* value){logs.emplace_back(value);}
bool RecordsAllowedForPB(){return allow_records;}
bool CustomMapActive(){return false;}
bool CustomMapApplying(){return false;}
std::uint32_t Milliseconds(double seconds){return std::uint32_t(std::llround(seconds*1000.0));}
std::wstring GhostPath(const std::wstring& ini){return ghost_data_paths::PB(g_storage_dir,ini);}
bool DrainFastestSegmentGhostWrites(){
    ++drain_calls;
    if(drain_failure) return false;
    if(!pending.empty()){disk=pending;pending.clear();}
    return true;
}
bool WriteGhostFile(const std::wstring&, const std::vector<std::uint8_t>& bytes){
    ++write_calls;
    if(write_failure)return false;
    disk=bytes;
    return true;
}
'''
main = r'''
int main(){
    // The background writer has queued an older incomplete recording.
    s_furthest.reached=1;
    s_furthest.button_ms[0]=200;
    s_furthest.samples={{0,0,0,0,0},{200,0,0,0,0}};
    assert(ghost_furthest_data::Serialize(s_furthest,pending));
    CommitPBGhostOnNewPB(.2);
    assert(drain_calls==1 && write_calls==1);
    assert(pending.empty());
    assert(s_pb_ghost_saved_for_latest_pb && s_primary_pb_protected);
    assert(s_furthest.samples.empty());
    std::vector<ghost_data::Sample> decoded;
    assert(ghost_data::Deserialize(disk,200,decoded));
    assert(decoded.size()==2);
    // Failure to flush the partial worker must stop the PB commit.
    pending=disk;
    drain_failure=true;
    s_primary_pb_protected=false;
    s_pb_ghost_saved_for_latest_pb=true;
    CommitPBGhostOnNewPB(.2);
    assert(!s_pb_ghost_saved_for_latest_pb);
    assert(!s_primary_pb_protected);
    assert(write_calls==1);
    drain_failure=false;pending.clear();
    // An I/O error must not destroy the existing file.
    const auto before=disk;
    write_failure=true;
    CommitPBGhostOnNewPB(.2);
    assert(disk==before);
    assert(!s_pb_ghost_saved_for_latest_pb);
    return 0;
}
} // namespace refunct
int main(){return refunct::main();}
'''
with tempfile.TemporaryDirectory(prefix='ghost_commit_') as td:
    cpp=Path(td)/'test.cpp'
    cpp.write_text(code+fn+'\n'+main,encoding='utf-8')
    bin=Path(td)/'test'
    subprocess.run([compiler,'-std=c++20','-O2','-Wall','-Wextra','-Werror','-I',
                    str(root/'include'),str(cpp),'-o',str(bin)],check=True,timeout=35)
    subprocess.run([str(bin)],check=True,timeout=15)
print('PASS: queued partial drained before PB write; PB protection and I/O failure safety')

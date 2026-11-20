"""Exercise the production old-furthest -> single primary ghost migration with C++20 mocks."""
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/ghost.cpp').read_text('utf-8')
start = source.index('bool IsCompletedGhostFormat(')
end = source.index('// A new gold must not synchronously open/flush a file on the game tick.', start)
functions = source[start:end]
assert 'ConsolidateLegacyFurthestGhost' in functions
assert 'ValidCompletedGhostBytes' in functions
compiler = shutil.which('g++') or shutil.which('clang++')
if not compiler:
    print('SKIP: C++20 compiler not found')
    raise SystemExit(0)

cpp = r'''
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <map>
#include <vector>
#include <string>
#include <cstring>
#include "refunct/ghost_data.hpp"
#include "refunct/ghost_furthest_data.hpp"
#include "refunct/ghost_data_paths.hpp"
namespace refunct {
constexpr int kRefunctButtonCount = 37;
std::wstring g_storage_dir=L"RefunctUtility";
std::map<std::wstring,std::vector<std::uint8_t>> files;
std::vector<std::wstring> logs;
bool fail_move=false,fail_write=false;
void Log(const wchar_t* s){logs.emplace_back(s);}
bool IsRegularFile(const std::wstring& path){return files.contains(path);}
std::wstring GhostPath(const std::wstring& pb){return ghost_data_paths::PB(g_storage_dir,pb);}
std::wstring FurthestGhostPath(const std::wstring& pb){return ghost_data_paths::Furthest(g_storage_dir,pb);}
bool ReadGhostFile(const std::wstring& path,std::vector<std::uint8_t>& bytes,std::size_t=0){
    auto it=files.find(path); if(it==files.end())return false;bytes=it->second;return true;
}
bool WriteGhostFile(const std::wstring& path,const std::vector<std::uint8_t>& bytes){
    if(fail_write) return false;
    files[path]=bytes;
    return true;
}
bool MoveFileW(const wchar_t* a,const wchar_t* b){
    if(fail_move||!files.contains(a)||files.contains(b))return false;
    files[b]=files.at(a);files.erase(a);return true;
}
bool DeleteFileW(const wchar_t* s){return files.erase(s)>0;}
'''
main = r'''
std::vector<std::uint8_t> MakePB(){
    std::vector<ghost_data::Sample> frames={{0,0,0,0,0},{200,1,2,3,0}};
    std::vector<std::uint8_t> bytes;
    assert(ghost_data::Serialize(frames,200,bytes)); return bytes;
}
std::vector<std::uint8_t> MakePartial(int reached){
    ghost_furthest_data::Run<kRefunctButtonCount> run;
    run.reached=reached;
    for (int i=0;i<reached;++i)run.button_ms[i]=(i+1)*200;
    run.samples={{0,0,0,0,0},{(std::uint32_t)(reached*200),1,2,3,0}};
    std::vector<std::uint8_t> bytes;
    assert(ghost_furthest_data::Serialize(run,bytes));return bytes;
}
int main(){
 const auto ini=ghost_data_paths::Records(g_storage_dir,L"Any%");
 const auto main=GhostPath(ini),old=FurthestGhostPath(ini);
 // The first incomplete recording is moved into the ordinary ghost file.
 files[old]=MakePartial(3);
 ConsolidateLegacyFurthestGhost(ini,200);
 assert(files.contains(main) && !files.contains(old));
 ghost_furthest_data::Run<kRefunctButtonCount> decoded;
 assert(ghost_furthest_data::Deserialize(files.at(main),decoded) && decoded.reached==3);
 // A better partial beats a shorter partial without a second saved file.
 files[old]=MakePartial(4);
 ConsolidateLegacyFurthestGhost(ini,200);
 assert(!files.contains(old));
 assert(ghost_furthest_data::Deserialize(files.at(main),decoded) && decoded.reached==4);
 // Worse partial must never replace the better one.
 files[old]=MakePartial(2);
 ConsolidateLegacyFurthestGhost(ini,200);
 assert(!files.contains(old));
 assert(ghost_furthest_data::Deserialize(files.at(main),decoded) && decoded.reached==4);
 // A valid completed PB is authoritative and never overwritten.
 const auto original=MakePB();files[main]=original;files[old]=MakePartial(5);
 ConsolidateLegacyFurthestGhost(ini,200);
 assert(files.at(main)==original && !files.contains(old));
 // A valid PB from a different time is stale for this split profile.
 files[main]=original;files[old]=MakePartial(6);
 ConsolidateLegacyFurthestGhost(ini,300);
 assert(files.at(main)==original && files.contains(old));
 // Invalid PB must be preserved for manual recovery, along with partial.
 files[main]=original;files[main].back()^=0xF;files[old]=MakePartial(6);
 auto invalid=files.at(main);
 ConsolidateLegacyFurthestGhost(ini,200);
 assert(files.at(main)==invalid && files.contains(old));
 // A blocked move leaves the only good recording intact.
 files.clear();files[old]=MakePartial(2);fail_move=true;
 ConsolidateLegacyFurthestGhost(ini,200);
 assert(!files.contains(main) && files.contains(old));fail_move=false;
 // A legacy PB that hasn't migrated must keep priority over partial data.
 files[ghost_data_paths::LegacyPB(g_storage_dir,ini)]=original;
 ConsolidateLegacyFurthestGhost(ini,200);
 assert(!files.contains(main) && files.contains(old));
 files.clear();files[main]=MakePartial(2);files[old]=MakePartial(3);
 fail_write=true;const auto earlier=files.at(main);
 ConsolidateLegacyFurthestGhost(ini,200);
 assert(files.at(main)==earlier && files.contains(old));
 return 0;
}
} // namespace refunct
int main(){return refunct::main();}
'''
with tempfile.TemporaryDirectory(prefix='ghost_unified_') as directory:
    path = Path(directory)/'unified.cpp'
    path.write_text(cpp + functions + main, encoding='utf-8')
    binary=Path(directory)/'unified'
    subprocess.run([compiler,'-std=c++20','-O2','-Wall','-Wextra','-Werror',
                    '-I',str(root/'include'),str(path),'-o',str(binary)],
                   check=True,timeout=35)
    subprocess.run([str(binary)],check=True,timeout=15)
print('PASS: single-file migration, PB priority, conflict safety, failed move/write')

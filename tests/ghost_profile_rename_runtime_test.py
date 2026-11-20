"""Exercise the production legacy-ghost rename recovery after folder rename."""
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root/'src/ghost.cpp').read_text('utf-8')
compiler = shutil.which('g++') or shutil.which('clang++')
assert compiler

start=source.index('void RenamePBGhostProfile(')
brace=source.index('{',start)
depth=0
for i in range(brace,len(source)):
    if source[i]=='{':depth+=1
    elif source[i]=='}':
        depth-=1
        if depth==0:break
function=source[start:i+1]
assert 'DrainFastestSegmentGhostWrites()' in function
assert 'LegacyPB' in function and 'LegacyFurthest' in function and 'LegacyGold' in function
assert 'LegacyInline' in function

cpp=r'''
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <string>
#include <vector>
#include <set>
#include "refunct/ghost_data_paths.hpp"
namespace refunct {
constexpr int kRefunctButtonCount=37;
std::wstring g_storage_dir=L"RefunctUtility",s_loaded_profile;
std::set<std::wstring> files;
std::vector<std::wstring> warnings;
void Log(const wchar_t* s){warnings.emplace_back(s);}
bool DrainFastestSegmentGhostWrites(){return true;}
bool IsRegularFile(const std::wstring& s){return files.contains(s);}
bool EnsureGhostSectionDirectory(const std::wstring&){return true;}
bool MoveFileW(const wchar_t* src,const wchar_t* dst) {
  const std::wstring from=src,to=dst;
  if(!files.contains(from)||files.contains(to))return false;
  files.erase(from);files.insert(to);return true;
}
std::wstring GhostPath(const std::wstring& p){return ghost_data_paths::PB(g_storage_dir,p);}
std::wstring FurthestGhostPath(const std::wstring& p){return ghost_data_paths::Furthest(g_storage_dir,p);}
std::wstring FastestGhostPath(const std::wstring& p,int b){return ghost_data_paths::Gold(g_storage_dir,p,b);}
'''
main=r'''
int main(){using namespace refunct;
  const auto a=ghost_data_paths::Records(g_storage_dir,L"Any%");
  const auto b=ghost_data_paths::Records(g_storage_dir,L"100%");
  const auto c=ghost_data_paths::Records(g_storage_dir,L"Renamed");
  files.insert(GhostPath(c)); // Moved automatically with entire profile folder.
  files.insert(FastestGhostPath(c,12));
  files.insert(GhostPath(b));
  files.insert(FastestGhostPath(b,12));
  files.insert(ghost_data_paths::LegacyPB(g_storage_dir,a));
  files.insert(ghost_data_paths::LegacyFurthest(g_storage_dir,a));
  files.insert(ghost_data_paths::LegacyGold(g_storage_dir,a,12));
  files.insert(ghost_data_paths::LegacyGold(g_storage_dir,a,28));
  files.insert(ghost_data_paths::LegacyInline(g_storage_dir,a,L".gold35.ghost"));
  RenamePBGhostProfile(a,c);
  assert(files.contains(GhostPath(c)));
  assert(files.contains(ghost_data_paths::LegacyPB(g_storage_dir,a))); // Conflict kept.
  assert(files.contains(FurthestGhostPath(c)));
  assert(files.contains(FastestGhostPath(c,12)));
  assert(files.contains(ghost_data_paths::LegacyGold(g_storage_dir,a,12))); // Conflict kept.
  assert(files.contains(FastestGhostPath(c,28)));
  assert(files.contains(FastestGhostPath(c,35)));
  assert(files.contains(GhostPath(b)) && files.contains(FastestGhostPath(b,12)));
  assert(s_loaded_profile==c);
  assert(!warnings.empty());
  return 0;
}
} // namespace refunct
int main(){return refunct::main();}
'''
with tempfile.TemporaryDirectory(prefix='ghost_rename_nested_') as td:
    src=Path(td)/'test.cpp'; binary=Path(td)/'test'
    src.write_text(cpp+function+'\n'+main,'utf-8')
    subprocess.run([compiler,'-std=c++20','-O2','-Wall','-Wextra','-Werror',
                    '-I',str(root/'include'),str(src),'-o',str(binary)],check=True,timeout=35)
    subprocess.run([str(binary)],check=True,timeout=15)
print('PASS: rename recovers unmigrated ghosts; per-profile files and other profiles are preserved')

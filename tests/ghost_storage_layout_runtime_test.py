"""Compile and run the production profile migration with a fake Win32 filesystem."""
from pathlib import Path
import subprocess
import shutil
import tempfile

root = Path(__file__).resolve().parents[1]
storage = (root / 'src/storage.cpp').read_text('utf-8')
records = (root / 'src/records.cpp').read_text('utf-8')
ghost = (root / 'src/ghost.cpp').read_text('utf-8')
compiler = shutil.which('g++') or shutil.which('clang++')
assert compiler


def function(src, signature):
    start = src.index(signature)
    opened = src.index('{', start)
    depth = 0
    for end in range(opened, len(src)):
        if src[end] == '{':
            depth += 1
        elif src[end] == '}':
            depth -= 1
            if depth == 0:
                return src[start:end + 1]
    raise AssertionError('Unclosed ' + signature)


assert 'MigrateLegacyGhostFiles();' in function(storage, 'bool InitializeStorage()')
assert 'EnsureProfileDirectories(name)' in function(records, 'bool CreateProfile(')
assert 'ghost_data_paths::ProfilesRoot(g_storage_dir)' in function(records, 'void ScanProfiles(')
assert 'ghost_data_paths::LegacyPB' in ghost
assert 'ghost_data_paths::LegacyGold' in ghost
assert 's_loaded_profile != g_pb_path' in ghost
assert 'WritePersonalBestSnapshot(destination, snapshot, false)' in records

helpers_start = storage.index('namespace {\nstruct MigrationCount')
helpers_end = storage.index('} // namespace', helpers_start) + len('} // namespace')
helpers = storage[helpers_start:helpers_end]

preamble = r'''
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "refunct/ghost_data_paths.hpp"
using DWORD = unsigned long;
using HANDLE = void*;
int _wcsicmp(const wchar_t* a,const wchar_t* b) {
  while (*a && *b) {
    const auto aa=std::towlower(*a++), bb=std::towlower(*b++);
    if(aa!=bb)return aa<bb?-1:1;
  }
  return *a==*b?0:(*a?1:-1);
}
int _wcsnicmp(const wchar_t* a,const wchar_t* b,size_t n) {
  for(size_t i=0;i<n;++i){
    const auto aa=std::towlower(a[i]),bb=std::towlower(b[i]);
    if(aa!=bb)return aa<bb?-1:1;
    if(!aa)return 0;
  }
  return 0;
}
constexpr DWORD FILE_ATTRIBUTE_DIRECTORY=0x10;
constexpr DWORD INVALID_FILE_ATTRIBUTES=0xfffffffful;
#define INVALID_HANDLE_VALUE (reinterpret_cast<HANDLE>(-1))
struct WIN32_FIND_DATAW { DWORD dwFileAttributes=0; wchar_t cFileName[260]{}; };
namespace refunct {
constexpr int kRefunctButtonCount=37;
std::wstring g_storage_dir=L"RefunctUtility",g_splits_dir=L"RefunctUtility\\Splits";
std::map<std::wstring,std::wstring> files;
std::set<std::wstring> dirs,denied;
std::vector<std::wstring> messages;
std::vector<std::pair<std::wstring,DWORD>> results;
size_t cursor=0;
void Log(const wchar_t* msg) {messages.emplace_back(msg);}
bool ValidProfileName(const std::wstring& value) {
  return !value.empty() && value.size()<=32 && value.front()!=L' ' && value.back()!=L' ' &&
      value.find_first_not_of(L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_% -()") == std::wstring::npos;
}
bool IsRegularFile(const std::wstring& path) {return files.contains(path);}
bool EnsureDirectory(const std::wstring& dir) {
  if(denied.contains(dir))return false;
  dirs.insert(dir);return true;
}
DWORD GetFileAttributesW(const wchar_t* path) {
  if(dirs.contains(path))return FILE_ATTRIBUTE_DIRECTORY;
  return files.contains(path)?0:INVALID_FILE_ATTRIBUTES;
}
bool MoveFileW(const wchar_t* from, const wchar_t* to) {
  if(denied.contains(from) || files.contains(to) || !files.contains(from))return false;
  const std::wstring dest=to;
  const auto idx=dest.find_last_of(L"\\/");
  if(idx!=std::wstring::npos && !dirs.contains(dest.substr(0,idx)))return false;
  files[dest]=files.at(from);files.erase(from);return true;
}
bool RemoveDirectoryW(const wchar_t* path) {
  const std::wstring dir(path);
  for(const auto& [f,v]:files)if(f.starts_with(dir+L"\\"))return false;
  return dirs.erase(dir)>0;
}
static void update(WIN32_FIND_DATAW* data) {
  std::wmemset(data->cFileName,0,260);
  std::wcsncpy(data->cFileName,results[cursor].first.c_str(),259);
  data->dwFileAttributes=results[cursor].second;
}
HANDLE FindFirstFileW(const wchar_t* mask, WIN32_FIND_DATAW* data) {
  results.clear();cursor=0;
  std::wstring query(mask);
  const auto slash=query.find_last_of(L"\\/");
  const auto folder=query.substr(0,slash);
  const auto pattern=query.substr(slash+1);
  auto collect=[&](const std::wstring& path,DWORD attr){
    if(!path.starts_with(folder+L"\\"))return;
    const auto filename=path.substr(folder.size()+1);
    if(filename.find(L'\\')!=std::wstring::npos)return;
    if(pattern==L"*" ||
       (pattern==L"*.ini" && filename.size()>=4 && filename.ends_with(L".ini")) ||
       (pattern==L"*.ini.bak" && filename.size()>=8 && filename.ends_with(L".ini.bak")) ||
       (pattern==L"*.ghost" && filename.size()>=6 && filename.ends_with(L".ghost")) ||
       (pattern==L"gold*.ghost" && filename.starts_with(L"gold") && filename.ends_with(L".ghost")))
       results.emplace_back(filename,attr);
  };
  for(const auto& [path,value]:files)collect(path,0);
  for(const auto& path:dirs)collect(path,FILE_ATTRIBUTE_DIRECTORY);
  if(results.empty())return INVALID_HANDLE_VALUE;
  update(data);return reinterpret_cast<HANDLE>(1);
}
bool FindNextFileW(HANDLE,WIN32_FIND_DATAW* data) {
  if(++cursor>=results.size())return false;
  update(data);return true;
}
bool FindClose(HANDLE) {return true;}
'''

main = r'''
int main() {
  using namespace refunct;
  dirs.insert(g_storage_dir);
  dirs.insert(g_splits_dir);
  dirs.insert(ghost_data_paths::ProfilesRoot(g_storage_dir));
  const auto a=ghost_data_paths::Records(g_storage_dir,L"Any%");
  const auto b=ghost_data_paths::Records(g_storage_dir,L"100%");
  assert(ghost_data_paths::ProfileName(a)==L"Any%");
  assert(ghost_data_paths::ProfileName(b)==L"100%");
  assert(ghost_data_paths::ProfileName(L"RefunctUtility\\Splits\\records.ini")==L"records");
  assert(ghost_data_paths::ProfileName(L"foo/records.ini").empty());
  assert(ghost_data_paths::ProfileName(L"Splits\\Any%.ini")==L"Any%");
  assert(ghost_data_paths::Gold(g_storage_dir,a,28)==
      L"RefunctUtility\\Profiles\\Any%\\Ghosts\\BestSegments\\gold28.ghost");
  assert(ghost_data_paths::PB(g_storage_dir,a)!=ghost_data_paths::PB(g_storage_dir,b));
  assert(ghost_data_paths::Gold(g_storage_dir,a,0).empty());
  assert(ghost_data_paths::Gold(g_storage_dir,a,38).empty());
  files[L"RefunctUtility\\Splits\\Any%.ini"]=L"original_any";
  files[L"RefunctUtility\\Splits\\Any%.ini.bak"]=L"original_backup";
  files[L"RefunctUtility\\Splits\\100%.ini"]=L"original_100";
  files[L"RefunctUtility\\Ghosts\\Any%.ghost"]=L"global_any_ghost";
  files[L"RefunctUtility\\Ghosts\\Any%.furthest.ghost"]=L"global_furthest";
  files[L"RefunctUtility\\Splits\\Any%.ini.ghost"]=L"inline_any_ghost";
  files[L"RefunctUtility\\Splits\\100%.ini.furthest.ghost"]=L"locked_furthest";
  files[L"RefunctUtility\\Splits\\Any%.ini.gold35.ghost"]=L"inline_gold35";
  files[L"RefunctUtility\\Splits\\Any%.ini.gold00.ghost"]=L"invalid_gold";
  dirs.insert(L"RefunctUtility\\Ghosts\\Sections");
  dirs.insert(L"RefunctUtility\\Ghosts\\Sections\\Any%");
  files[L"RefunctUtility\\Ghosts\\Sections\\Any%\\gold28.ghost"]=L"global_gold28";
  denied.insert(L"RefunctUtility\\Splits\\100%.ini.furthest.ghost");
  // An existing modern PB ghost takes priority over either legacy copy.
  assert(EnsureProfileDirectories(L"Any%"));
  files[ghost_data_paths::PB(g_storage_dir,a)]=L"new_ghost";
  MigrateLegacyGhostFiles();
  assert(files.at(a)==L"original_any");
  assert(files.at(a+L".bak")==L"original_backup");
  assert(files.at(b)==L"original_100");
  assert(files.at(ghost_data_paths::PB(g_storage_dir,a))==L"new_ghost");
  assert(files.at(L"RefunctUtility\\Ghosts\\Any%.ghost")==L"global_any_ghost");
  assert(files.at(L"RefunctUtility\\Splits\\Any%.ini.ghost")==L"inline_any_ghost");
  assert(files.at(ghost_data_paths::Furthest(g_storage_dir,a))==L"global_furthest");
  assert(files.at(ghost_data_paths::Gold(g_storage_dir,a,28))==L"global_gold28");
  assert(files.at(ghost_data_paths::Gold(g_storage_dir,a,35))==L"inline_gold35");
  assert(files.at(L"RefunctUtility\\Splits\\100%.ini.furthest.ghost")==L"locked_furthest");
  assert(files.at(L"RefunctUtility\\Splits\\Any%.ini.gold00.ghost")==L"invalid_gold");
  assert(!messages.empty() && messages.back().find(L"conflicts=")!=std::wstring::npos);
  denied.clear();
  MigrateLegacyGhostFiles();
  assert(files.at(ghost_data_paths::Furthest(g_storage_dir,b))==L"locked_furthest");
  assert(files.at(ghost_data_paths::Gold(g_storage_dir,a,28))==L"global_gold28");
  // An unexpected legacy INI conflict must not attach its backup to a newer record.
  files[L"RefunctUtility\\Splits\\100%.ini"]=L"old_collision";
  files[L"RefunctUtility\\Splits\\100%.ini.bak"]=L"old_backup";
  MigrateLegacyGhostFiles();
  assert(files.at(b)==L"original_100");
  assert(files.at(L"RefunctUtility\\Splits\\100%.ini.bak")==L"old_backup");
  // A conflicting INI must also prevent merging its old ghost with another
  // profile's already-migrated best-segment records.
  files[L"RefunctUtility\\Ghosts\\100%.ghost"]=L"different_recording";
  MigrateLegacyGhostFiles();
  assert(files.at(L"RefunctUtility\\Ghosts\\100%.ghost")==L"different_recording");
  assert(!files.contains(ghost_data_paths::PB(g_storage_dir,b)));
  // After resolving the collision, a stranded backup and ghost can migrate
  // safely on the next startup without losing either recording.
  files.erase(L"RefunctUtility\\Splits\\100%.ini");
  MigrateLegacyGhostFiles();
  assert(files.at(b+L".bak")==L"old_backup");
  assert(files.at(ghost_data_paths::PB(g_storage_dir,b))==L"different_recording");
  return 0;
}
} // namespace refunct
int main(){return refunct::main();}
'''

source = '\n'.join([
    preamble,
    function(storage, 'std::wstring ProfilePath('),
    function(storage, 'bool EnsureProfileDirectories('),
    function(storage, 'bool EnsureGhostSectionDirectory('),
    helpers,
    function(storage, 'void MigrateLegacyGhostFiles()'),
    main,
])
with tempfile.TemporaryDirectory(prefix='nested_profile_migration_') as temp:
    cpp = Path(temp) / 'mock.cpp'
    binary = Path(temp) / 'mock'
    cpp.write_text(source,'utf-8')
    subprocess.run([compiler,'-std=c++20','-O2','-Wall','-Wextra','-Werror',
                    '-I',str(root/'include'),str(cpp),'-o',str(binary)],check=True,timeout=40)
    subprocess.run([str(binary)],check=True,timeout=15)
print('PASS: per-profile paths, legacy INI/backups, both ghost layouts, golds, conflicts and retry')

"""Compile the real profile path, profile scanning, and full-directory rename functions."""
from pathlib import Path
import subprocess
import shutil
import tempfile

root=Path(__file__).resolve().parents[1]
storage=(root/'src/storage.cpp').read_text('utf-8')
records=(root/'src/records.cpp').read_text('utf-8')
compiler=shutil.which('g++') or shutil.which('clang++')
assert compiler

def fn(s, signature):
    begin=s.index(signature); brace=s.index('{',begin); depth=0
    for at in range(brace,len(s)):
        if s[at]=='{':depth+=1
        if s[at]=='}':
            depth-=1
            if depth==0:return s[begin:at+1]
    raise AssertionError(signature)

assert 'MoveFileW(old_dir.c_str(), new_dir.c_str())' in fn(records,'bool RenameActiveProfile(')
assert 'RenamePBGhostProfile(from, new_record)' in fn(records,'bool RenameActiveProfile(')

header=r'''
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
using DWORD=unsigned long;
using HANDLE=void*;
constexpr DWORD INVALID_FILE_ATTRIBUTES=0xfffffffful;
constexpr DWORD FILE_ATTRIBUTE_DIRECTORY=0x10;
#define INVALID_HANDLE_VALUE (reinterpret_cast<HANDLE>(-1))
struct WIN32_FIND_DATAW {DWORD dwFileAttributes=0;wchar_t cFileName[260]{};};
int _wcsicmp(const wchar_t* a,const wchar_t* b) {
 while(*a&&*b){const auto aa=std::towlower(*a++),bb=std::towlower(*b++);
  if(aa!=bb)return aa<bb?-1:1;}
 return *a==*b?0:(*a?1:-1);
}
namespace refunct {
std::wstring g_storage_dir=L"RefunctUtility",g_splits_dir=L"RefunctUtility\\Splits";
std::wstring g_active_profile=L"Any%",g_pb_path;
std::vector<std::wstring> g_profiles;
std::map<std::wstring,std::wstring> files;
std::set<std::wstring> dirs;
std::vector<std::wstring> logs,notices;
int ghost_rename_calls=0,compare_reloads=0,save_settings_calls=0;
bool ValidProfileName(const std::wstring& s) {
 return !s.empty()&&s.size()<=32&&s.front()!=L' '&&s.back()!=L' '&&
 s.find_first_not_of(L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789%_- ()") == std::wstring::npos;
}
bool IsRegularFile(const std::wstring& s){return files.contains(s);}
bool EnsureDirectory(const std::wstring& s){dirs.insert(s);return true;}
DWORD GetFileAttributesW(const wchar_t* s) {
 if(dirs.contains(s))return FILE_ATTRIBUTE_DIRECTORY;
 return files.contains(s)?0:INVALID_FILE_ATTRIBUTES;
}
bool MoveFileW(const wchar_t* from,const wchar_t* to) {
 const std::wstring src=from,dst=to;
 if(files.contains(src)){
  if(files.contains(dst))return false;
  files[dst]=files.at(src);files.erase(src);return true;
 }
 if(!dirs.contains(src)||dirs.contains(dst)||files.contains(dst))return false;
 std::vector<std::pair<std::wstring,std::wstring>> moved_files;
 std::vector<std::pair<std::wstring,std::wstring>> moved_dirs;
 for(const auto& [f,v]:files)if(f.starts_with(src+L"\\"))moved_files.emplace_back(f,dst+f.substr(src.size()));
 for(const auto& d:dirs)if(d==src||d.starts_with(src+L"\\"))moved_dirs.emplace_back(d,dst+d.substr(src.size()));
 for(const auto& [a,b]:moved_files){files[b]=files.at(a);files.erase(a);}
 for(const auto& [a,b]:moved_dirs){dirs.erase(a);dirs.insert(b);}
 return true;
}
std::vector<std::wstring> listing;size_t index=0;
void update(WIN32_FIND_DATAW* data){
 std::wmemset(data->cFileName,0,260);
 std::wcsncpy(data->cFileName,listing[index].c_str(),259);
 data->dwFileAttributes=listing[index].ends_with(L".ini")?0:FILE_ATTRIBUTE_DIRECTORY;
}
HANDLE FindFirstFileW(const wchar_t* pattern,WIN32_FIND_DATAW* data){
 listing.clear();index=0;
 std::wstring mask(pattern);
 const auto slash=mask.find_last_of(L"\\/");
 const auto folder=mask.substr(0,slash),wild=mask.substr(slash+1);
 if(wild==L"*"){
  for(const auto& dir:dirs)if(dir.starts_with(folder+L"\\")&&
    dir.substr(folder.size()+1).find(L'\\')==std::wstring::npos)
    listing.push_back(dir.substr(folder.size()+1));
 } else if(wild==L"*.ini"){
  for(const auto& [path,v]:files)if(path.starts_with(folder+L"\\")&&path.ends_with(L".ini")&&
     path.substr(folder.size()+1).find(L'\\')==std::wstring::npos)
     listing.push_back(path.substr(folder.size()+1));
 }
 if(listing.empty())return INVALID_HANDLE_VALUE;
 update(data);return reinterpret_cast<HANDLE>(1);
}
bool FindNextFileW(HANDLE,WIN32_FIND_DATAW* data){if(++index>=listing.size())return false;update(data);return true;}
bool FindClose(HANDLE){return true;}
void Log(const wchar_t* s){logs.emplace_back(s);}
void Notify(const std::wstring& s){notices.push_back(s);}
bool DrainGoldFileWriter(){return true;}
bool DrainFastestSegmentGhostWrites(){return true;}
void RenamePBGhostProfile(const std::wstring&,const std::wstring&){++ghost_rename_calls;}
bool ReloadComparisonProfile(){++compare_reloads;return true;}
void SaveSettings(){++save_settings_calls;}
struct PersonalBest{};
bool SavePersonalBestFile(const PersonalBest&){return true;}
'''
main=r'''
int main(){using namespace refunct;
 dirs.insert(g_storage_dir);dirs.insert(ghost_data_paths::ProfilesRoot(g_storage_dir));
 dirs.insert(g_splits_dir);
 assert(EnsureProfileDirectories(L"Any%"));
 assert(EnsureProfileDirectories(L"100%"));
 const auto a=ghost_data_paths::Records(g_storage_dir,L"Any%");
 const auto b=ghost_data_paths::Records(g_storage_dir,L"100%");
 const auto c=ghost_data_paths::Records(g_storage_dir,L"Renamed");
 files[a]=L"best_splits_any";
 files[a+L".bak"]=L"backup_any";
 files[ghost_data_paths::PB(g_storage_dir,a)]=L"pb_any";
 files[ghost_data_paths::Gold(g_storage_dir,a,28)]=L"gold28_any";
 files[b]=L"best_splits_100";
 files[ghost_data_paths::Gold(g_storage_dir,b,28)]=L"gold28_100";
 g_pb_path=ProfilePath(L"Any%");
 ScanProfiles(false);
 assert(g_profiles.size()==2);
 assert(RenameActiveProfile(L"Renamed"));
 assert(g_active_profile==L"Renamed"&&g_pb_path==c);
 assert(files.at(c)==L"best_splits_any" && files.at(c+L".bak")==L"backup_any");
 assert(files.at(ghost_data_paths::PB(g_storage_dir,c))==L"pb_any");
 assert(files.at(ghost_data_paths::Gold(g_storage_dir,c,28))==L"gold28_any");
 assert(files.at(ghost_data_paths::Gold(g_storage_dir,b,28))==L"gold28_100");
 assert(ghost_rename_calls==1&&compare_reloads==1&&save_settings_calls==1);
 assert(g_profiles.size()==2);
 assert(!RenameActiveProfile(L"100%"));
 assert(files.at(c)==L"best_splits_any");
 // An unresolved old/new INI collision blocks folder rename rather than
 // reintroducing a similarly named legacy profile on the next profile scan.
 files[L"RefunctUtility\\Splits\\Renamed.ini"]=L"conflicting_record";
 assert(!RenameActiveProfile(L"YetAnother"));
 assert(files.at(c)==L"best_splits_any");
 assert(files.at(L"RefunctUtility\\Splits\\Renamed.ini")==L"conflicting_record");
 // Failed old migration: rename imports the outstanding record first.
 files[L"RefunctUtility\\Splits\\Legacy.ini"]=L"legacy_record";
 files[L"RefunctUtility\\Splits\\Legacy.ini.bak"]=L"legacy_backup";
 g_active_profile=L"Legacy";g_pb_path=ProfilePath(L"Legacy");
 assert(RenameActiveProfile(L"Imported"));
 const auto d=ghost_data_paths::Records(g_storage_dir,L"Imported");
 assert(files.at(d)==L"legacy_record");
 assert(files.at(d+L".bak")==L"legacy_backup");
 return 0;
}
} // namespace refunct
int main(){return refunct::main();}
'''
parts=[header,fn(storage,'std::wstring ProfilePath('),fn(storage,'bool EnsureProfileDirectories('),
       fn(records,'void ScanProfiles('),fn(records,'bool RenameActiveProfile('),main]
with tempfile.TemporaryDirectory(prefix='profile_folder_rename_') as td:
    cpp=Path(td)/'test.cpp';binary=Path(td)/'test';cpp.write_text('\n'.join(parts),'utf-8')
    subprocess.run([compiler,'-std=c++20','-O2','-Wall','-Wextra','-Werror',
                    '-I',str(root/'include'),str(cpp),'-o',str(binary)],check=True,timeout=35)
    subprocess.run([str(binary)],check=True,timeout=15)
print('PASS: actual profile enumeration, whole-folder rename, backups, ghosts, conflicts, legacy INI')

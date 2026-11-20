"""Compile the actual HUD profile module against a fake Win32 filesystem.
Exercise persistence, validation, all fields, menu wiring and isolation.
No game/Win32 ABI validation is implied.
"""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/hud_profiles.cpp').read_text()
state = (root / 'include/refunct/state.hpp').read_text()
menu = (root / 'src/menu.cpp').read_text()
storage = (root / 'src/storage.cpp').read_text()
records = (root / 'src/records.cpp').read_text()
assert 'src/hud_profiles.cpp' in (root/'CMakeLists.txt').read_text()
assert 'InitialiseHudProfiles();' in (root/'src/bootstrap.cpp').read_text().split('LoadSettings();',1)[1]
assert 'write(L"HUDProfiles", L"ActiveProfile", ActiveHudProfile());' in storage
assert 'g_show_key_display' in source and 'g_show_splits' in source
# Every modern Display setting is covered, except legacy aliases derived from
# the modern enum/boolean fields. Array styles are checked in the compiled test.
settings = storage.split('write(L"Display", L"Coordinates"',1)[1].split('write(L"Movement", L"KeepOnGameLaunch"',1)[0]
keys = {'Coordinates'} | set(re.findall(r'write\(L"Display", L"(\w+)"',settings))
legacy = {'CubeProgress','TileProgress','ComparePB','SplitSegmentTime','SplitSegmentLabel'}
assert keys - legacy <= set(re.findall(r'field\("(\w+)"',source))
assert 'L"HUD Profiles  >"' in menu
assert 's_hud_profiles_return_page = Page::Hud;\n                RefreshHudProfiles();\n                SetPage(Page::HudProfiles);' in menu
assert 'case Page::HudProfiles:\n            SetPage(s_hud_profiles_return_page); break;' in menu
assert 'NameEditMode::HudCreate' in records and 'NameEditMode::HudRename' in records
assert 'Page::ConfirmDeleteHudProfile || page == Page::Search' in menu
assert not any(x in source for x in ('g_active_profile', 'g_compare_profile', 'g_keys[', 'g_pb.', 'g_flying', 'SelectProfile('))
compiler = shutil.which('g++') or shutil.which('clang++')
assert compiler, 'C++ compiler needed for native mock'
with tempfile.TemporaryDirectory() as td:
    directory = Path(td)
    mock = directory / 'refunct'
    mock.mkdir()
    # Use the real HUD declarations and enum definitions, not invented types.
    declarations = state.split('extern bool g_show_coordinates;',1)[1].split('extern bool g_flying;',1)[0]
    declarations = 'extern bool g_show_coordinates;' + declarations
    definitions = '\n'.join(re.findall(r'^extern ([^;]+);',declarations,re.M))
    definitions = re.sub(r'(?m)^([^\n]+)$',r'\1{};',definitions)
    (mock/'internal_api.hpp').write_text('''#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include <iterator>
#include <cwchar>
#include <cwctype>
#include <map>
#include <cassert>
#include <cstring>
#include "refunct/pure.hpp"
#include "refunct/section_compare.hpp"
using DWORD = uint32_t;
using ULONGLONG = uint64_t;
struct File { std::wstring path; std::string contents; bool write = false; std::vector<std::wstring> entries; size_t i = 0; };
using HANDLE = File*;
constexpr HANDLE INVALID_HANDLE_VALUE = nullptr;
constexpr DWORD FILE_ATTRIBUTE_DIRECTORY = 16, FILE_ATTRIBUTE_NORMAL = 128;
constexpr DWORD GENERIC_READ = 0x80000000, GENERIC_WRITE = 0x40000000;
constexpr DWORD FILE_SHARE_READ = 1, OPEN_EXISTING = 3, CREATE_ALWAYS = 2;
constexpr DWORD MOVEFILE_REPLACE_EXISTING = 1, MOVEFILE_WRITE_THROUGH = 8;
constexpr DWORD INVALID_FILE_SIZE = 0xFFFFFFFF;
struct WIN32_FIND_DATAW { DWORD dwFileAttributes; wchar_t cFileName[260]; };
inline std::map<std::wstring,std::string> files;
inline std::wstring active;
inline bool fail_write = false;
inline int _wcsicmp(const wchar_t* a, const wchar_t* b) {
    for (; *a && *b; ++a,++b) if (towlower(*a)!=towlower(*b)) return towlower(*a)<towlower(*b)?-1:1;
    return *a ? 1 : *b ? -1 : 0;
}
inline HANDLE CreateFileW(const wchar_t* path,DWORD access,DWORD,void*,DWORD,DWORD,void*) {
    if (access==GENERIC_READ && !files.contains(path)) return nullptr;
    if (access==GENERIC_WRITE && fail_write) return nullptr;
    auto* f=new File{}; f->path=path; f->write=access==GENERIC_WRITE;
    if (!f->write) f->contents=files.at(path); return f;
}
inline DWORD GetFileSize(HANDLE f,void*) {return static_cast<DWORD>(f->contents.size());}
inline int ReadFile(HANDLE f,void* dst,DWORD len,DWORD* read,void*) {
    *read=std::min(len,GetFileSize(f,nullptr)); std::memcpy(dst,f->contents.data(),*read);return 1;
}
inline int WriteFile(HANDLE f,const void* src,DWORD len,DWORD* written,void*) {
    f->contents.assign(static_cast<const char*>(src),len); *written=len; return 1;
}
inline int FlushFileBuffers(HANDLE) {return 1;}
inline int CloseHandle(HANDLE f) {if(f->write) files[f->path]=f->contents; delete f; return 1;}
inline int DeleteFileW(const wchar_t* p) {return files.erase(p)!=0;}
inline int MoveFileExW(const wchar_t* a,const wchar_t* b,DWORD) {
    if(!files.contains(a)) return 0; files[b]=files.at(a);files.erase(a);return 1;
}
inline int MoveFileW(const wchar_t* a,const wchar_t* b) {return !files.contains(b)&&MoveFileExW(a,b,0);}
inline int FindNextFileW(HANDLE f,WIN32_FIND_DATAW* d) {
    if(f->i>=f->entries.size())return 0; d->dwFileAttributes=0;
    std::wcscpy(d->cFileName,f->entries[f->i++].c_str());return 1;
}
inline HANDLE FindFirstFileW(const wchar_t* pattern,WIN32_FIND_DATAW* d) {
    auto* f=new File{}; std::wstring dir(pattern); dir.resize(dir.size()-5);
    for(const auto& [p,v]:files) if(p.starts_with(dir)&&p.ends_with(L".ini")) f->entries.push_back(p.substr(dir.size()));
    if(!FindNextFileW(f,d)){delete f;return nullptr;}return f;
}
inline int FindClose(HANDLE f){delete f;return 1;}
inline DWORD GetPrivateProfileStringW(const wchar_t*,const wchar_t*,const wchar_t* fallback,wchar_t* dst,DWORD n,const wchar_t*) {
    auto v=active.empty()?std::wstring(fallback):active; std::wcsncpy(dst,v.c_str(),n-1);dst[n-1]=0;return static_cast<DWORD>(v.size());
}
namespace refunct {
inline std::wstring g_storage_dir=L"root",g_settings_path=L"settings.ini";
inline bool IsRegularFile(const std::wstring& p){return files.contains(p);}
inline bool EnsureDirectory(const std::wstring&){return true;}
void SaveSettings();
inline void Log(const wchar_t*){}
inline void Notify(const std::wstring&){}
''' + declarations + '\n}\n')
    harness = directory/'harness.cpp'
    harness.write_text('''#include "''' + str(root/'src/hud_profiles.cpp') + '''"
namespace refunct {
''' + definitions + '''
void SaveSettings(){ if(HudProfilesReady()) active=ActiveHudProfile(); }
}
int main(){
    using namespace refunct;
    // Initialise every field to its minimum, then import the existing HUD.
    VisitHudFields([](const auto&,auto& ref,int min,int,int){ref=static_cast<std::remove_reference_t<decltype(ref)>>(min);});
    g_show_key_display=true; g_show_splits=true;
    const auto initial=Capture();
    assert(Validate(initial)); assert(initial.size()==42+5*kHudElementCount);
    InitialiseHudProfiles(); assert(HudProfilesReady()); assert(active==L"Default");
    assert(HudProfileNames().size()==1 && Exists(L"Default"));
    assert(CreateHudProfile(L"Speedrun")); assert(active==L"Speedrun");
    assert(!CreateHudProfile(L"speedrun")); assert(!CreateHudProfile(L"../bad"));
    // Exercise all fields at their maximum, including every style slot.
    VisitHudFields([](const auto&,auto& ref,int,int max,int){ref=static_cast<std::remove_reference_t<decltype(ref)>>(max);});
    g_show_key_display=false; g_show_splits=false;
    auto changed=Capture(); assert(Validate(changed));
    assert(SelectHudProfile(L"Default")); assert(Capture()==initial);
    assert(SelectHudProfile(L"Speedrun")); assert(Capture()==changed);
    assert(RenameActiveHudProfile(L"Minimal")); assert(active==L"Minimal");
    assert(!Exists(L"Speedrun")); assert(Exists(L"Minimal"));
    assert(SaveActiveHudProfile());
    g_show_timer=!g_show_timer;
    auto live=Capture(); assert(live!=changed);
    // Simulate startup with saved live settings: preserve the active label,
    // current working settings, and the separate explicitly saved snapshot.
    s_ready=false; s_active=L"Default"; InitialiseHudProfiles();
    assert(active==L"Minimal" && Capture()==live);
    assert(ReloadActiveHudProfile() && Capture()==changed);
    // No partial application for malformed or incomplete target files.
    auto bad=changed; bad["Timer"]=2; assert(!Validate(bad));
    bad=changed; bad.erase("Timer"); assert(!Validate(bad));
    bad=changed; bad["KeyDisplayPositionSizePercent"]=55; assert(!Validate(bad));
    assert(WriteProfile(L"Broken",bad)); assert(!SelectHudProfile(L"Broken"));
    assert(Capture()==changed && active==L"Minimal");
    fail_write=true; assert(!SelectHudProfile(L"Default")); fail_write=false;
    assert(Capture()==changed && active==L"Minimal");
    assert(DeleteActiveHudProfile()); assert(active==L"Default" && Capture()==initial);
    assert(!DeleteActiveHudProfile() && !RenameActiveHudProfile(L"No"));
}
''')
    binary=directory/'hud_test'
    subprocess.run([compiler,'-std=c++20','-I',str(directory),'-I',str(root/'include'),str(harness),str(root/'src/pure.cpp'),'-o',str(binary)],check=True,timeout=30)
    subprocess.run([str(binary)],check=True,timeout=10)
print('PASS: complete HUD schema, native mock profile lifecycle, relaunch, validation and isolation')

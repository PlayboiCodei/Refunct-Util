"""Compile production Hotkey Profiles against fake Win32 files and gameplay setters."""
from pathlib import Path
import shutil
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
source = (root/'src/hotkey_profiles.cpp').read_text()
assert 'src/hotkey_profiles.cpp' in (root/'CMakeLists.txt').read_text()
assert 'InitialiseHotkeyProfiles();' in (root/'src/bootstrap.cpp').read_text()
assert 'NameEditMode::HotkeyCreate' in (root/'src/records.cpp').read_text()
assert 'NameEditMode::HotkeyRename' in (root/'src/records.cpp').read_text()
compiler = shutil.which('g++') or shutil.which('clang++')
assert compiler
with tempfile.TemporaryDirectory() as td:
    directory = Path(td)
    mock = directory/'refunct'
    mock.mkdir()
    mock_header = r'''#pragma once
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
#include <atomic>
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
inline void SaveSettings();
inline bool QueueSettingsValue(const wchar_t*,const wchar_t*,const wchar_t* v){active=v;return true;}
inline void Log(const wchar_t*){}
inline void Notify(const std::wstring&){}
} // namespace refunct
'''
    state = (root/'include/refunct/state.hpp').read_text()
    table = state.split('enum class Action : size_t {',1)[1].split('extern std::array<std::atomic_int, kActionCount> g_keys;',1)[0]
    core = (root/'src/core.cpp').read_text().split('bool IsBindable(int key) {',1)[1].split('\n}\n',1)[0]
    vk = dict(BACK=8,TAB=9,RETURN=13,SHIFT=16,CONTROL=17,MENU=18,ESCAPE=27,SPACE=32,PRIOR=33,NEXT=34,LEFT=37,UP=38,RIGHT=39,DOWN=40,LWIN=91,RWIN=92,LBUTTON=1,RBUTTON=2)
    vk.update({f'F{i}':111+i for i in range(1,13)})
    constants = '\n'.join(f'constexpr int VK_{key}={value};' for key,value in vk.items())
    extra = constants + '\nnamespace refunct {\n' + 'enum class Action : size_t {' + table + '\ninline std::array<std::atomic_int,kActionCount> g_keys{};\nbool IsBindable(int key) {' + core + '\n}\nconst std::wstring& ActiveHotkeyProfile();\ninline int saves=0;\ninline void SaveSettings(){++saves;active=ActiveHotkeyProfile();}\n}\n'
    (mock/'internal_api.hpp').write_text(mock_header + extra)
    harness=directory/'harness.cpp'
    harness.write_text('#include "' + str(root/'src/hotkey_profiles.cpp') + '"\n'+r'''

int main(){
    using namespace refunct;
    for(size_t i=0;i<kActionCount;++i) g_keys[i].store(kActions[i].default_key);
    const auto initial=Capture(); assert(initial.size()==61 && Validate(initial));
    InitialiseHotkeyProfiles(); assert(HotkeyProfilesReady() && active==L"Default");
    assert(CreateHotkeyProfile(L"Practice")); assert(active==L"Practice");
    assert(!CreateHotkeyProfile(L"practice") && !CreateHotkeyProfile(L"../bad"));
    auto desired=initial; desired["Menu"]='J'; desired["Restart"]=VK_F5; desired["SelectAllTiles"]='Q';
    assert(Validate(desired)); Apply(desired); assert(Capture()==desired);
    assert(SelectHotkeyProfile(L"Default") && Capture()==initial);
    assert(SelectHotkeyProfile(L"Practice") && Capture()==desired);
    assert(RenameActiveHotkeyProfile(L"Training") && active==L"Training");
    assert(SaveActiveHotkeyProfile()); Apply(initial);
    assert(ReloadActiveHotkeyProfile() && Capture()==desired);
    // Live settings are authoritative on restart, preserving individual edits.
    auto edited=desired; edited["SelectAllTiles"]='T'; Apply(edited);
    s_ready=false; InitialiseHotkeyProfiles(); assert(active==L"Training" && Capture()==edited);
    Values loaded; assert(ReadProfile(L"Training",loaded) && loaded==edited);
    auto bad=initial; bad.erase("Menu"); assert(!Validate(bad));
    bad=initial; bad["Restart"]=VK_F5; assert(!Validate(bad));
    bad=desired; bad["Menu"]=0; assert(!Validate(bad));
    bad=initial; bad["Menu"]=VK_ESCAPE; assert(!Validate(bad));
    bad=initial; bad["Menu"]=999; assert(!Validate(bad));
    bad=initial; bad["Menu"]=0; assert(Validate(bad));
    files[Path(L"Bad")]=files[Path(L"Default")]+"Menu=0\n";
    assert(!SelectHotkeyProfile(L"Bad") && Capture()==edited && active==L"Training");
    fail_write=true; assert(!SelectHotkeyProfile(L"Default") && Capture()==edited && active==L"Training"); fail_write=false;
    assert(DeleteActiveHotkeyProfile() && active==L"Default" && Capture()==initial);
    assert(!DeleteActiveHotkeyProfile() && !RenameActiveHotkeyProfile(L"No"));
    // Existing legacy shadowed F5 assignments are repaired on initialisation.
    Apply(desired); g_keys[static_cast<size_t>(Action::Menu)].store(0);
    s_ready=false; InitialiseHotkeyProfiles(); assert(Validate(Capture()));
    assert(Capture().at("Restart")==0 && Capture().at("Menu")==0);
}
''')
    binary=directory/'hotkey_test'
    subprocess.run([compiler,'-std=c++20','-I',str(directory),'-I',str(root/'include'),str(harness),str(root/'src/pure.cpp'),'-o',str(binary)],check=True,timeout=30)
    subprocess.run([str(binary)],check=True,timeout=10)
print('PASS: all 61 hotkeys, profile lifecycle, startup, invalid files, write failure and F5 recovery')

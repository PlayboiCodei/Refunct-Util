"""Compile production World Profiles against fake Win32 files and gameplay setters."""
from pathlib import Path
import shutil
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
source = (root/'src/world_profiles.cpp').read_text()
assert 'src/world_profiles.cpp' in (root/'CMakeLists.txt').read_text()
assert 'InitialiseWorldProfiles();' in (root/'src/bootstrap.cpp').read_text()
assert 'NameEditMode::WorldCreate' in (root/'src/records.cpp').read_text()
assert 'NameEditMode::WorldRename' in (root/'src/records.cpp').read_text()
assert all(x not in source for x in ('g_active_profile', 'g_show_', 'g_player_fov', 'PressAllAvailableButtons(', 'SelectWorldTiles(', 'RequestCubeWorldAction('))
compiler = shutil.which('g++') or shutil.which('clang++')
assert compiler
with tempfile.TemporaryDirectory() as td:
    directory = Path(td)
    mock = directory/'refunct'
    mock.mkdir()
    (mock/'internal_api.hpp').write_text(r'''#pragma once
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
inline void SaveSettings(){}
inline bool QueueSettingsValue(const wchar_t*,const wchar_t*,const wchar_t* v){active=v;return true;}
inline void Log(const wchar_t*){}
inline void Notify(const std::wstring&){}
inline bool g_animate_section_changes=true, g_buttons_sync_with_sections=false, g_recording_tainted=false, g_pb_cheats_allowed_for_run=false, g_run_pb_eligible=true;
inline std::atomic_bool g_section_button_contact_hook_available{true};
inline bool sync_failure=false, button_tiles=false, touch_tiles=false, remove_water=false, hide_ocean=false, swim_disabled=false;
inline int section_speed=100, tube_speed=100, platform_speed=100, jump_height=100, water_height=0;
enum class WorldObjectKind : uint8_t { Tubes, Buttons, JumpPads, Cubes, Platforms, Tiles, Count };
inline std::array<bool,6> removed{};
inline bool IsWorldObjectRemoved(WorldObjectKind k){return removed[static_cast<size_t>(k)];}
inline void SetWorldObjectRemovalProfile(const std::array<bool,6>& values){removed=values;}
inline bool SetButtonsSyncWithSections(bool v){if(sync_failure)return false;g_buttons_sync_with_sections=v;return true;}
inline bool ButtonTileSelectionDisabledPreference(){return button_tiles;}
inline bool TileSelectionOnContactDisabledPreference(){return touch_tiles;}
inline bool ToggleButtonTileSelection(){button_tiles=!button_tiles;return true;}
inline bool ToggleTileSelectionOnContact(){touch_tiles=!touch_tiles;return true;}
inline void FinishSectionPlacementAnimations(){}
inline int GetSectionSpeedPercent(){return section_speed;}
inline int GetTubeTravelSpeedPercent(){return tube_speed;}
inline int GetPlatformSpeedPercent(){return platform_speed;}
inline int GetJumpPadHeightPercent(){return jump_height;}
inline void SetSectionSpeedProfilePercent(int v){section_speed=v;}
inline void SetTubeTravelSpeedProfilePercent(int v){tube_speed=v;}
inline void SetPlatformSpeedProfilePercent(int v){platform_speed=v;}
inline void SetJumpPadHeightProfilePercent(int v){jump_height=v;}
inline bool IsAllWaterRemoved(){return remove_water;}
inline bool IsWaterRemoved(){return hide_ocean;}
inline bool IsWaterSwimmingDisabled(){return swim_disabled;}
inline int GetWaterHeightOffset(){return water_height;}
inline void SetAllWaterRemoved(bool v){remove_water=v;}
inline void SetWaterRemoved(bool v){hide_ocean=v;}
inline void SetWaterSwimmingDisabled(bool v){swim_disabled=v;}
inline void SetWaterHeightOffset(int v){water_height=v;}
} // namespace refunct
''')
    harness = directory/'harness.cpp'
    harness.write_text('#include "' + str(root/'src/world_profiles.cpp') + '"\n' + r'''
int main(){
    using namespace refunct;
    const auto initial=Capture(); assert(initial.size()==18 && Validate(initial));
    InitialiseWorldProfiles(); assert(s_ready && active==L"Default");
    assert(CreateWorldProfile(L"Practice")); assert(active==L"Practice");
    assert(!CreateWorldProfile(L"practice") && !CreateWorldProfile(L"../bad"));
    auto desired=initial;
    for(auto& [key,val]:desired) {
        if(key=="WaterHeight") val=-500;
        else if(key.ends_with("Speed") || key=="JumpPadHeight") val=123456;
        else val=!val;
    }
    assert(Validate(desired)); assert(Apply(desired)); assert(Capture()==desired);
    assert(g_recording_tainted && !g_run_pb_eligible);
    assert(SelectWorldProfile(L"Default") && Capture()==initial);
    assert(SelectWorldProfile(L"Practice") && Capture()==desired);
    assert(RenameActiveWorldProfile(L"Training") && active==L"Training");
    assert(SaveActiveWorldProfile());
    assert(Apply(initial)); NoteWorldProfilePersistenceReset();
    assert(SelectWorldProfile(L"Default"));
    assert(SelectWorldProfile(L"Training") && Capture()==desired); // reset did not overwrite snapshot
    assert(Apply(initial)); NoteWorldProfilePersistenceReset();
    s_ready=false; InitialiseWorldProfiles();
    assert(active==L"Training" && Capture()==initial);
    assert(ReloadActiveWorldProfile() && Capture()==desired);
    auto bad=initial; bad.erase("WaterHeight"); assert(WriteProfile(L"Bad",bad));
    assert(!SelectWorldProfile(L"Bad") && Capture()==desired && active==L"Training");
    bad=initial; bad["RemoveWater"]=2; assert(!Validate(bad));
    bad=initial; bad["JumpPadHeight"]=0; assert(!Validate(bad));
    bad=initial; bad["WaterHeight"]=100001; assert(!Validate(bad));
    g_section_button_contact_hook_available=false;
    assert(!SelectWorldProfile(L"Default") && Capture()==desired && active==L"Training");
    g_section_button_contact_hook_available=true;
    sync_failure=true; assert(!SelectWorldProfile(L"Default") && Capture()==desired); sync_failure=false;
    section_speed=123457; fail_write=true; assert(!SelectWorldProfile(L"Default") && active==L"Training"); fail_write=false;
    assert(DeleteActiveWorldProfile() && active==L"Default" && Capture()==initial);
    assert(!DeleteActiveWorldProfile() && !RenameActiveWorldProfile(L"No"));
}
''')
    binary = directory/'world_test'
    subprocess.run([compiler,'-std=c++20','-I',str(directory),'-I',str(root/'include'),str(harness),str(root/'src/pure.cpp'),'-o',str(binary)],check=True,timeout=30)
    subprocess.run([str(binary)],check=True,timeout=10)
print('PASS: World profile lifecycle, all 18 fields, persistence resets, startup, invalid files and hook preflight')

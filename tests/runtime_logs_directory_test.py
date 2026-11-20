#!/usr/bin/env python3
"""Compile runtime log paths, Win32 migration and early startup trace with filesystem mocks."""
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
bootstrap = (root / 'src/bootstrap.cpp').read_text()
trace = bootstrap[bootstrap.index('void StartupTrace('):bootstrap.index('\nint DiagnosticMode()')]
assert bootstrap.index('StartupTrace("01:') < bootstrap.index('const int diagnostic_mode = DiagnosticMode();')
assert 'runtime_logs::StorageDirectory(runtime_logs::ModulePath(nullptr)' in (root / 'src/storage.cpp').read_text()
assert r'\\Logs\\RefunctUtility.log' in (root / 'src/core.cpp').read_text()

mock = r'''
#pragma once
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <map>
#include <set>
#include <string>
#include <vector>
using DWORD = unsigned long;
using HANDLE = void*;
using HMODULE = void*;
#define MAX_PATH 260
#define INVALID_HANDLE_VALUE ((HANDLE)-1)
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#define FILE_ATTRIBUTE_DIRECTORY 16
#define FILE_ATTRIBUTE_REPARSE_POINT 1024
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_NO_MORE_FILES 18
#define FILE_ATTRIBUTE_NORMAL 128
#define ERROR_ALREADY_EXISTS 183
#define ERROR_FILE_EXISTS 80
#define FILE_APPEND_DATA 4
#define FILE_SHARE_READ 1
#define FILE_SHARE_WRITE 2
#define OPEN_ALWAYS 4
struct WIN32_FIND_DATAW { DWORD dwFileAttributes; wchar_t cFileName[MAX_PATH]; };
inline int _wcsicmp(const wchar_t* a, const wchar_t* b) {
    while (*a && *b && std::towlower(*a)==std::towlower(*b)) { ++a; ++b; }
    return int(std::towlower(*a))-int(std::towlower(*b));
}
inline std::map<std::wstring, std::string> files;
inline std::set<std::wstring> dirs, locked;
inline std::wstring exe, asi, opened;
inline DWORD error = 0;
inline int debug_messages = 0, enumerations = 0;
inline std::wstring local_app_data;
inline DWORD GetEnvironmentVariableW(const wchar_t*, wchar_t* out, DWORD cap) {
    if (local_app_data.empty()) return 0;
    if (local_app_data.size()>=cap) return cap;
    std::wcscpy(out,local_app_data.c_str()); return DWORD(local_app_data.size());
}
inline unsigned GetPrivateProfileIntW(const wchar_t*,const wchar_t* key,int fallback,const wchar_t* path) {
    auto it=files.find(path);
    if(it==files.end()) return fallback;
    const std::string name = std::wstring(key)==L"StartupLogEnabled" ? "StartupLogEnabled" : "UtilLogEnabled";
    if(it->second.find(name+"=0")!=std::string::npos) return 0;
    if(it->second.find(name+"=1")!=std::string::npos) return 1;
    return fallback;
}
inline std::wstring parent(const std::wstring& p) { return p.substr(0,p.find_last_of(L'\\')); }
inline DWORD GetModuleFileNameW(HMODULE module, wchar_t* out, DWORD cap) {
    const auto& p = module ? asi : exe;
    if (p.size()>=cap) return cap;
    std::wcscpy(out,p.c_str()); return DWORD(p.size());
}
inline DWORD GetFileAttributesW(const wchar_t* p) {
    if (dirs.count(p)) return FILE_ATTRIBUTE_DIRECTORY;
    if (files.count(p)) return FILE_ATTRIBUTE_NORMAL;
    return INVALID_FILE_ATTRIBUTES;
}
inline bool CreateDirectoryW(const wchar_t* p, void*) {
    if (files.count(p)||dirs.count(p)||!dirs.count(parent(p))) return false;
    dirs.insert(p); return true;
}
inline bool MoveFileW(const wchar_t* from, const wchar_t* to) {
    if (!files.count(from)||locked.count(from)) {error=5;return false;}
    if (GetFileAttributesW(to)!=INVALID_FILE_ATTRIBUTES) {error=ERROR_ALREADY_EXISTS;return false;}
    if (!dirs.count(parent(to))) {error=3;return false;}
    files[to]=files[from]; files.erase(from); return true;
}
inline bool DeleteFileW(const wchar_t* path) {
    if (locked.count(path)) {error=5;return false;}
    if (!files.erase(path)) {error=ERROR_FILE_NOT_FOUND;return false;}
    return true;
}
inline DWORD GetLastError() { return error; }
struct Search { std::vector<std::wstring> names; size_t index=0; std::wstring directory; };
inline bool read_item(Search* s, WIN32_FIND_DATAW* out) {
    if(s->index>=s->names.size()) {error=ERROR_NO_MORE_FILES;return false;}
    const auto& name=s->names[s->index++];
    std::wcscpy(out->cFileName,name.c_str());
    out->dwFileAttributes=GetFileAttributesW((s->directory+L"\\"+name).c_str());
    return true;
}
inline HANDLE FindFirstFileW(const wchar_t* pattern, WIN32_FIND_DATAW* out) {
    ++enumerations;
    auto* s=new Search;
    s->directory=parent(pattern);
    auto collect=[&](const std::wstring& p){if(parent(p)==s->directory) s->names.push_back(p.substr(s->directory.size()+1));};
    // Return all direct children intentionally; actual migration must filter.
    for(auto& [p,v]:files) collect(p);
    for(auto& p:dirs) if(p!=s->directory) collect(p);
    if(!read_item(s,out)){delete s; error=ERROR_FILE_NOT_FOUND; return INVALID_HANDLE_VALUE;}return s;
}
inline bool FindNextFileW(HANDLE h, WIN32_FIND_DATAW* out){return read_item(static_cast<Search*>(h),out);}
inline bool FindClose(HANDLE h){delete static_cast<Search*>(h);return true;}
inline HANDLE CreateFileW(const wchar_t* p,DWORD,DWORD,void*,DWORD,DWORD,void*) {
    if(!dirs.count(parent(p))) return INVALID_HANDLE_VALUE;
    opened=p; files[opened]; return reinterpret_cast<HANDLE>(1);
}
inline bool WriteFile(HANDLE,const void* data,DWORD length,DWORD* count,void*) {
    files[opened].append(static_cast<const char*>(data),length);*count=length;return true;
}
inline bool CloseHandle(HANDLE){return true;}
inline void OutputDebugStringA(const char*){++debug_messages;}
'''

main = r'''
int main(int argc, char** argv) {
    using namespace refunct;
    using namespace refunct::runtime_logs;
    bool standard=false;
    const std::wstring game=L"D:\\Steam Games\\Refunct";
    const std::wstring bin=game+L"\\Refunct\\Binaries\\Win32";
    const std::wstring storage=game+L"\\RefunctUtility";
    const std::wstring logs=storage+L"\\Logs";
    exe=bin+L"\\Refunct-Win32-Shipping.exe";
    asi=bin+L"\\RefunctUtility.asi";
    dirs={game,bin,storage,logs,storage+L"\\directory.log"};
    assert(StorageDirectory(exe,&standard)==storage && standard);
    assert(StorageDirectory(L"E:\\Games\\refunct\\binaries\\win32\\refunct-win32-shipping.exe",&standard)==L"E:\\Games\\RefunctUtility" && standard);
    assert(StorageDirectory(L"C:\\Portable\\Refunct-Win32-Shipping.exe",&standard)==L"C:\\Portable\\RefunctUtility" && !standard);
    assert(StorageDirectory(L"Refunct-Win32-Shipping.exe").empty());
    assert(StorageDirectory(L"").empty());
    files[storage+L"\\RefunctUtility.log"]="old log";
    files[logs+L"\\RefunctUtility.log"]="current log";
    files[logs+L"\\RefunctUtility.legacy-1.log"]="older archive";
    files[storage+L"\\RefunctUtility(20261117).log"]="dated";
    files[storage+L"\\button-sync-diagnostics.txt"]="diagnostic";
    files[bin+L"\\RefunctUtility.startup.txt"]="startup\r\n";
    files[bin+L"\\RefunctUtilityRestart.log"]="restart";
    files[bin+L"\\unrelated.log"]="game log";
    files[storage+L"\\settings.ini"]="settings";
    files[storage+L"\\notes.txt"]="notes";
    files[storage+L"\\not.logx"]="other";
    files[storage+L"\\locked.log"]="locked";
    locked.insert(storage+L"\\locked.log");
    // A separate process is required because the startup preference is cached.
    if (argc>1) {
        const std::string mode=argv[1];
        const bool legacy=mode.rfind("legacy-",0)==0;
        const bool startup_off=mode.find("startup-off")!=std::string::npos;
        const std::string prefs=startup_off ? "[Settings]\nUtilLogEnabled=1\nStartupLogEnabled=0" : "[Settings]\nUtilLogEnabled=0\nStartupLogEnabled=1";
        if (legacy) {
            files.erase(storage+L"\\settings.ini");
            local_app_data=L"C:\\Users\\Test\\AppData\\Local";
            files[local_app_data+L"\\RefunctUtility\\settings.ini"]=prefs;
        } else files[storage+L"\\settings.ini"]=prefs;
        assert(SavedLoggingEnabled()==startup_off);
        assert(SavedLoggingEnabled(L"StartupLogEnabled")==!startup_off);
        StartupTrace("must never be saved");
        StartupTrace("also suppressed");
        assert(files.at(logs+L"\\RefunctUtility.startup.txt")=="startup\r\n");
        assert(opened.empty() && debug_messages==0);
        assert(g_util_log_enabled==startup_off && g_startup_log_enabled==!startup_off);
        // A preference enabled after bootstrap must no longer be blocked by the cached initial value.
        g_util_log_enabled=true;
        g_startup_log_enabled=true;
        StartupTrace("enabled live");
        assert(files.at(logs+L"\\RefunctUtility.startup.txt")=="startup\r\nenabled live\r\n");
        return 0;
    }
    assert(SavedLoggingEnabled() && SavedLoggingEnabled(L"StartupLogEnabled"));
    StartupTrace("first startup line");
    assert(opened==logs+L"\\RefunctUtility.startup.txt");
    assert(files.at(opened)=="startup\r\nfirst startup line\r\n");
    assert(!files.count(bin+L"\\RefunctUtility.startup.txt"));
    assert(files.at(logs+L"\\RefunctUtility.log")=="current log");
    assert(files.at(logs+L"\\RefunctUtility.legacy-1.log")=="older archive");
    assert(files.at(logs+L"\\RefunctUtility.legacy-2.log")=="old log");
    assert(!files.count(storage+L"\\RefunctUtility.log"));
    assert(files.at(logs+L"\\RefunctUtility(20261117).log")=="dated");
    assert(files.at(logs+L"\\button-sync-diagnostics.txt")=="diagnostic");
    assert(files.at(logs+L"\\RefunctUtilityRestart.log")=="restart");
    assert(files.at(bin+L"\\unrelated.log")=="game log");
    assert(files.at(storage+L"\\settings.ini")=="settings");
    assert(files.at(storage+L"\\notes.txt")=="notes");
    assert(files.at(storage+L"\\not.logx")=="other");
    assert(files.at(storage+L"\\locked.log")=="locked");
    assert(dirs.count(storage+L"\\directory.log"));
    StartupTrace("second startup line");
    assert(enumerations==1 && files.at(opened)=="startup\r\nfirst startup line\r\nsecond startup line\r\n");
    g_util_log_enabled=false;
    StartupTrace("suppressed live");
    assert(files.at(opened)=="startup\r\nfirst startup line\r\nsecond startup line\r\n");
    g_util_log_enabled=true;
    g_startup_log_enabled=false;
    StartupTrace("startup suppressed independently");
    assert(files.at(opened)=="startup\r\nfirst startup line\r\nsecond startup line\r\n");
    g_startup_log_enabled=true;
    // A later launch retries files which could not be moved, without overwriting.
    locked.clear(); assert(Prepare(g_module)==logs);
    assert(!files.count(storage+L"\\locked.log") && files.at(logs+L"\\locked.log")=="locked");
    files[logs+L"\\notes.txt"]="keep";
    files[logs+L"\\settings.ini"]="keep";
    files[logs+L"\\RefunctUtility.startup.legacy-2.txt"]="archive";
    files[logs+L"\\button-sync-diagnostics.legacy-1.txt"]="archive";
    dirs.insert(logs+L"\\Nested.log");
    files[logs+L"\\Nested.log\\another.log"]="keep";
    locked.insert(logs+L"\\locked.log");
    assert(!ClearDirectory(logs));
    assert(files.count(logs+L"\\locked.log"));
    assert(!files.count(logs+L"\\RefunctUtility.log"));
    assert(!files.count(logs+L"\\RefunctUtility.startup.txt"));
    assert(!files.count(logs+L"\\button-sync-diagnostics.txt"));
    assert(!files.count(logs+L"\\RefunctUtility.startup.legacy-2.txt"));
    assert(!files.count(logs+L"\\button-sync-diagnostics.legacy-1.txt"));
    assert(!files.count(logs+L"\\RefunctUtility(20261117).log"));
    assert(files.at(logs+L"\\notes.txt")=="keep");
    assert(files.at(logs+L"\\settings.ini")=="keep");
    assert(files.at(logs+L"\\Nested.log\\another.log")=="keep");
    assert(files.at(storage+L"\\settings.ini")=="settings");
    assert(files.at(bin+L"\\unrelated.log")=="game log");
    locked.clear(); assert(ClearDirectory(logs));
    assert(!files.count(logs+L"\\locked.log"));
    assert(ClearDirectory(logs));
    // Fresh install creates both directories before the first write.
    exe=L"C:\\Portable\\Refunct-Win32-Shipping.exe";
    asi=L"C:\\Portable\\RefunctUtility.asi";
    dirs.insert(L"C:\\Portable");
    assert(Prepare(g_module)==L"C:\\Portable\\RefunctUtility\\Logs");
    assert(dirs.count(L"C:\\Portable\\RefunctUtility\\Logs"));
    // A blocked Logs directory must not overwrite an existing ordinary file.
    exe=L"C:\\Blocked\\Refunct-Win32-Shipping.exe";
    asi=L"C:\\Blocked\\RefunctUtility.asi";
    dirs.insert(L"C:\\Blocked"); dirs.insert(L"C:\\Blocked\\RefunctUtility");
    files[L"C:\\Blocked\\RefunctUtility\\Logs"]="do not overwrite";
    assert(Prepare(g_module).empty());
    assert(files.at(L"C:\\Blocked\\RefunctUtility\\Logs")=="do not overwrite");
}
'''

compiler = shutil.which('g++') or shutil.which('clang++')
assert compiler, 'A C++20 compiler is required'
with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    (tmp / 'Windows.h').write_text(mock)
    (tmp / 'test.cpp').write_text('#include "refunct/runtime_logs.hpp"\nnamespace refunct {\nHMODULE g_module=reinterpret_cast<HMODULE>(1); std::atomic_bool g_util_log_enabled{true}, g_startup_log_enabled{true};\n' + trace + '\n}\n' + main)
    subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-I', str(tmp), '-I', str(root/'include'), str(tmp/'test.cpp'), '-o', str(tmp/'test')], check=True)
    subprocess.run([str(tmp/'test')], check=True)
    subprocess.run([str(tmp/'test'), 'local-off'], check=True)
    subprocess.run([str(tmp/'test'), 'legacy-off'], check=True)
    subprocess.run([str(tmp/'test'), 'local-startup-off'], check=True)
    subprocess.run([str(tmp/'test'), 'legacy-startup-off'], check=True)
print('PASS: shared log paths, early startup trace, safe migration, collision preservation, locked-file retry, fresh directory and blocked path')

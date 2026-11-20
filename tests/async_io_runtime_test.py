#!/usr/bin/env python3
"""Compile the actual async worker bodies against deterministic Win32 mocks.

This is a portable threaded test, not a replacement for Windows x86 linking.
"""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
core = (ROOT / 'src/core.cpp').read_text()
storage = (ROOT / 'src/storage.cpp').read_text()
ghost = (ROOT / 'src/ghost.cpp').read_text()
core_part = core[core.index('namespace refunct {'):core.index('\nbool IsReadable(')] + '\n}\n'
storage_part = storage[storage.index('// Retain the existing INI key names/format.'):storage.index('\nvoid SaveSettings() {')]
assert 'SaveMovementModeSettings();' not in storage[storage.index('void SaveSettings() {'):storage.index('// Read large percentages')], 'full save must be a single job'
assert 'write(L"Movement", L"Flying"' in storage and 'write(L"Movement", L"Noclip"' in storage
assert 'EnqueueSettings(std::move(entries))' in storage
assert all('QueueSettingsValue(L"Ghost", L"' + key in ghost for key in
           ('ShowPBGhost', 'StartNextGhostOnButton', 'UseFastestSections'))

stubs = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <map>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <deque>
#include <chrono>
#include <cstdio>
#include <cstring>
using DWORD = unsigned long;
using BOOL = int;
using PVOID = void*;
using LPVOID = void*;
using PINIT_ONCE = struct INIT_ONCE*;
using HANDLE = void*;
using HMODULE = void*;
#define WINAPI
#define CALLBACK
#define TRUE 1
#define FALSE 0
#define MAX_PATH 260
#define MAXDWORD UINT32_MAX
#define INFINITE 0xffffffffUL
#define FILE_APPEND_DATA 4
#define FILE_SHARE_READ 1
#define FILE_SHARE_WRITE 2
#define FILE_SHARE_DELETE 4
#define OPEN_ALWAYS 4
#define FILE_ATTRIBUTE_NORMAL 128
#define INVALID_HANDLE_VALUE ((HANDLE)-1)
#define WAIT_OBJECT_0 0
#define WAIT_TIMEOUT 258
#define SRWLOCK_INIT {}
#define INIT_ONCE_STATIC_INIT {}
struct SRWLOCK { std::mutex mutex; };
struct INIT_ONCE { std::once_flag once; };
struct Handle {
    enum Kind {Event, Thread, File} kind;
    std::mutex mutex;
    std::condition_variable cond;
    bool manual = false, signaled = false;
};
static std::mutex g_io_mutex;
static std::wstring g_log_output;
static std::map<std::wstring, std::wstring> g_settings;
static std::thread::id g_main_thread;
static int g_main_disk_calls = 0;
static int g_file_opens = 0;
static std::atomic_bool clear_fail{false};
static int clear_calls=0;
static int open_files=0;
static std::wstring g_opened_path;
static int g_background_ini_writes = 0;
static std::vector<std::thread::id> g_logger_threads;
void Sleep(DWORD ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
void CheckBackground() {
    if (std::this_thread::get_id() == g_main_thread) ++g_main_disk_calls;
}
void AcquireSRWLockExclusive(SRWLOCK* lock) {lock->mutex.lock();}
void ReleaseSRWLockExclusive(SRWLOCK* lock) {lock->mutex.unlock();}
HANDLE CreateEventW(void*, BOOL manual, BOOL initial, const wchar_t*) {
    auto* h = new Handle;
    h->kind = Handle::Event;
    h->manual = manual != 0;
    h->signaled = initial != 0;
    return h;
}
BOOL SetEvent(HANDLE value) {
    auto* h = static_cast<Handle*>(value);
    std::lock_guard lk(h->mutex); h->signaled = true;
    h->cond.notify_all(); return 1;
}
BOOL ResetEvent(HANDLE value) {
    auto* h = static_cast<Handle*>(value);
    std::lock_guard lk(h->mutex); h->signaled = false; return 1;
}
DWORD WaitForSingleObject(HANDLE value, DWORD millis) {
    auto* h = static_cast<Handle*>(value);
    std::unique_lock lk(h->mutex);
    if (millis == INFINITE) h->cond.wait(lk, [&]{return h->signaled;});
    else if (!h->cond.wait_for(lk, std::chrono::milliseconds(millis), [&]{return h->signaled;}))
        return WAIT_TIMEOUT;
    if (!h->manual) h->signaled = false;
    return WAIT_OBJECT_0;
}
HANDLE CreateThread(void*, size_t, DWORD (*fn)(LPVOID), LPVOID ctx, DWORD, DWORD*) {
    auto* h = new Handle;
    h->kind = Handle::Thread;
    std::thread([=] {fn(ctx);}).detach();
    return h;
}
BOOL CloseHandle(HANDLE value) {
    auto* h = static_cast<Handle*>(value);
    if (h->kind == Handle::File) { std::lock_guard lk(g_io_mutex); --open_files; delete h; }
    else if (h->kind == Handle::Thread) delete h;
    return 1;
}
DWORD GetModuleFileNameW(HMODULE, wchar_t* path, DWORD cap) {
    std::wstring target=L"C:\\Refunct\\RefunctUtility.asi";
    if (target.size() >= cap) return cap;
    target.copy(path, target.size()); path[target.size()] = 0;
    return static_cast<DWORD>(target.size());
}
int wcscpy_s(wchar_t* dst, size_t cap, const wchar_t* src) {
    if (std::wcslen(src) >= cap) return 1;
    std::wcscpy(dst, src); return 0;
}
HANDLE CreateFileW(const wchar_t* path, DWORD, DWORD, void*, DWORD, DWORD, void*) {
    CheckBackground();
    std::lock_guard lk(g_io_mutex); ++g_file_opens; ++open_files; g_opened_path = path;
    auto* h = new Handle; h->kind = Handle::File; return h;
}
BOOL WriteFile(HANDLE, const void* buffer, DWORD bytes, DWORD* written, void*) {
    CheckBackground();
    std::lock_guard lk(g_io_mutex);
    g_log_output.append(static_cast<const wchar_t*>(buffer), bytes/sizeof(wchar_t));
    *written=bytes; return 1;
}
void OutputDebugStringW(const wchar_t*) {
    CheckBackground();
    std::lock_guard lk(g_io_mutex);
    g_logger_threads.push_back(std::this_thread::get_id());
}
BOOL WritePrivateProfileStringW(const wchar_t* section, const wchar_t* key,
                                const wchar_t* value, const wchar_t*) {
    CheckBackground();
    std::lock_guard lk(g_io_mutex);
    ++g_background_ini_writes;
    g_settings[std::wstring(section)+L"/"+key] = value;
    return 1;
}
BOOL InitOnceExecuteOnce(INIT_ONCE* once, BOOL (*callback)(PINIT_ONCE, PVOID, PVOID*),
                         PVOID param, PVOID* result) {
    std::call_once(once->once, [=] { callback(once, param, result); });
    return 1;
}
namespace refunct {
namespace runtime_logs {
std::wstring ParentDirectory(const std::wstring& p) {return p.substr(0,p.find_last_of(L'\\'));}
bool ClearDirectory(const std::wstring& p) {
    CheckBackground();
    std::lock_guard lk(g_io_mutex);
    assert(p==L"C:\\Refunct\\RefunctUtility\\Logs" && open_files==0);
    ++clear_calls;
    if(clear_fail) return false;
    g_log_output.clear();
    return true;
}
}
    HMODULE g_module = reinterpret_cast<HMODULE>(1);
    std::atomic_bool g_util_log_enabled{true};
    std::wstring g_settings_path = L"C:\\Refunct\\settings.ini";
    bool g_flying = false, g_restore_flying_pending = false;
    bool g_noclip = false, g_restore_noclip_pending = false;
}
'''

main = r'''
int main() {
    using namespace refunct;
    g_main_thread = std::this_thread::get_id();
    // Bootstrap logs stay buffered until the settings directory is known.
    Log(L"early bootstrap entry");
    ConfigureUtilLogDirectory(L"C:\\Refunct\\RefunctUtility");
    for (int i=0; i<900; ++i) Log(L"periodic diagnostic");
    Log(L"a different message");
    assert(DrainLogWriter(3000));
    {
        std::lock_guard lk(g_io_mutex);
        assert(g_log_output.find(L"periodic diagnostic") != std::wstring::npos);
        assert(g_log_output.find(L"a different message") != std::wstring::npos);
        assert(g_file_opens == 1);
        assert(g_opened_path == L"C:\\Refunct\\RefunctUtility\\Logs\\RefunctUtility.log");
        assert(g_log_output.find(L"early bootstrap entry") != std::wstring::npos);
    }
    // OFF drops queued messages and makes no further file writes;
    // ON can resume using the same worker and folder.
    SetUtilLogEnabled(false);
    assert(DrainLogWriter(3000));
    size_t logged_before = 0;
    {
        std::lock_guard lk(g_io_mutex);
        logged_before = g_log_output.size();
    }
    for (int i=0; i<500; ++i) Log(L"should never reach disk");
    assert(DrainLogWriter(3000));
    {
        std::lock_guard lk(g_io_mutex);
        assert(g_log_output.size() == logged_before);
    }
    SetUtilLogEnabled(true);
    Log(L"resumed logging");
    assert(DrainLogWriter(3000));
    {
        std::lock_guard lk(g_io_mutex);
        assert(g_log_output.find(L"resumed logging") != std::wstring::npos);
        assert(g_log_output.find(L"should never reach disk") == std::wstring::npos);
    }
    assert(RequestClearRuntimeLogs());
    Log(L"fresh after clear");
    assert(DrainLogWriter(3000));
    assert(std::wstring(ClearLogsMenuLabel())==L"Clear Logs (cleared)");
    {std::lock_guard lk(g_io_mutex); assert(g_log_output.find(L"fresh after clear")!=std::wstring::npos);
     assert(g_log_output.find(L"resumed logging")==std::wstring::npos && clear_calls==1);}
    SetUtilLogEnabled(false);
    assert(RequestClearRuntimeLogs());
    assert(DrainLogWriter(3000));
    {std::lock_guard lk(g_io_mutex);assert(g_log_output.empty() && clear_calls==2);}
    assert(!g_util_log_enabled);
    clear_fail=true;
    assert(RequestClearRuntimeLogs()); assert(DrainLogWriter(3000));
    assert(std::wstring(ClearLogsMenuLabel())==L"Clear Logs (some logs could not be cleared)");
    // The newest value must win even when settings are queued repeatedly.
    for (int i=0; i<50; ++i) {
        assert(QueueSettingsValue(L"Ghost", L"StartNextGhostOnButton", i%2?L"1":L"0"));
    }
    g_restore_flying_pending = true;
    SaveMovementModeSettings();
    assert(DrainSettingsWriter(3000));
    {
        std::lock_guard lk(g_io_mutex);
        assert(g_settings.at(L"Ghost/StartNextGhostOnButton") == L"1");
        assert(g_settings.at(L"Movement/Flying") == L"1");
        assert(g_settings.at(L"Movement/Noclip") == L"0");
    }
    // Mimic two large coalesced SaveSettings jobs. The immutable snapshot
    // must not accidentally read later mutable global values from the worker.
    std::vector<IniEntry> batch;
    for (int i=0; i<160; ++i)
        batch.push_back({L"Display", L"Option"+std::to_wstring(i), L"1"});
    assert(EnqueueSettings(std::move(batch)));
    assert(QueueSettingsValue(L"Display", L"Option99", L"2"));
    assert(DrainSettingsWriter(3000));
    {
        std::lock_guard lk(g_io_mutex);
        assert(g_settings.at(L"Display/Option99") == L"2");
        assert(g_settings.at(L"Display/Option159") == L"1");
        assert(g_background_ini_writes >= 160);
        assert(g_main_disk_calls == 0); // all disk and debug calls on worker
    }
    std::puts("PASS: worker-backed logging, coalesced settings, immutable snapshots and main-thread isolation");
    std::fflush(stdout);
    std::_Exit(0); // process-lifetime workers are intentionally never joined
}
'''
with tempfile.TemporaryDirectory() as temp:
    source = Path(temp) / 'worker_mock.cpp'
    exe = Path(temp) / 'worker_mock'
    source.write_text(stubs + '\n' + core_part + '\nnamespace refunct {\n' + storage_part + '\n}\n' + main)
    result = subprocess.run(['g++', '-std=c++20', '-O1', '-Wall', '-Wextra', '-pthread',
                             str(source), '-o', str(exe)], capture_output=True, text=True)
    if result.returncode:
        raise AssertionError('Mock compilation failed:\n' + result.stderr)
    for trial in range(3):
        result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
        assert result.returncode == 0, f'Runtime failed {trial}: {result.stdout}\n{result.stderr}'
        print(result.stdout.strip())
print('PASS: source integration checks')

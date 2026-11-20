"""Compile the actual Settings row, activation, startup and timing code with mocks."""
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
menu = (root / 'src/menu.cpp').read_text()
storage = (root / 'src/storage.cpp').read_text()
bootstrap = (root / 'src/bootstrap.cpp').read_text()
performance = (root / 'src/performance.cpp').read_text().split('\n', 2)[2]
rows = menu.split('case Page::Logs:\n            return {', 1)[1].split('case Page::ConfirmClearLogs:', 1)[0]
settings_rows = menu.split('case Page::Settings:\n            return {', 1)[1].split('case Page::Logs:', 1)[0]
assert 'L"Logs  >"' in settings_rows and 'Save Logs:' not in settings_rows
activation = menu.split('case Page::Settings:\n            if (i == 0)', 1)[1].split('case Page::Hotkeys:', 1)[0]
assert 'SetPage(Page::Logs);' in activation
assert 'case Page::Persistence: case Page::Hotkeys: case Page::Logs:\n            SetPage(Page::Settings);' in menu
assert 'case Page::Logs: return L"Settings / Logs";' in menu
load = storage.split('const bool util_log_requested =', 1)[1].split('wchar_t profile[64]', 1)[0]
settings = storage.split('void SaveSettings() {', 1)[1].split('write(L"Display",', 1)[0]
assert 'write(L"Settings", L"PerformanceMonitor",' not in settings
assert 'GetPrivateProfileIntW(L"Settings", L"PerformanceMonitor"' not in storage
assert 'write(L"Settings", L"UtilLogEnabled",' in settings
assert 'write(L"Settings", L"StartupLogEnabled",' in settings
assert 'StartupTrace("BUILD: Optimisation Log menu fix 1");' in bootstrap
# Build-only patch intentionally removed installer prompts.
assert 'verify_install_asi.ps1' not in (root / 'build_win32.bat').read_text()

stubs = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cwchar>
#include <string>
#include <vector>
#include "refunct/performance.hpp"
struct LARGE_INTEGER { long long QuadPart; };
long long now_qpc = 1000000;
int clock_calls = 0;
int QueryPerformanceCounter(LARGE_INTEGER* v) { ++clock_calls; v->QuadPart=now_qpc; return 1; }
int QueryPerformanceFrequency(LARGE_INTEGER* v) { v->QuadPart=1000000; return 1; }
namespace refunct {
std::atomic_bool g_util_log_enabled{true}, g_startup_log_enabled{true};
std::wstring g_settings_path=L"settings.ini", g_storage_dir=L"RefunctUtility";
std::vector<std::wstring> messages;
bool saved_util=true, saved_perf=false, saved_startup=true, configured=false;
int back=0, page_changes=0;
enum class Page { Settings, Hotkeys, Persistence, Logs, ConfirmClearLogs };
int clear_count=0;
bool RequestClearRuntimeLogs(){ ++clear_count; return true; }
const wchar_t* ClearLogsMenuLabel(){return L"Clear Logs";}
Page page=Page::Settings;
void SetPage(Page p) { page=p; ++page_changes; }
void GoBack() { page=page==Page::ConfirmClearLogs ? Page::Logs : Page::Settings; ++back; }
void SetUtilLogEnabled(bool v) { g_util_log_enabled=v; }
void SaveSettings() { saved_util=g_util_log_enabled; saved_perf=g_performance_monitor_enabled; saved_startup=g_startup_log_enabled; }
void Notify(const wchar_t*) {}
void Log(const wchar_t* text) { if(g_util_log_enabled) messages.emplace_back(text); }
void ConfigureUtilLogDirectory(const std::wstring&) {configured=true;}
int GetPrivateProfileIntW(const wchar_t*, const wchar_t* key, int, const wchar_t*) {
 if (std::wcscmp(key,L"StartupLogEnabled")==0) return saved_startup;
 return std::wcscmp(key,L"PerformanceMonitor")==0 ? saved_perf : saved_util;
}
std::vector<std::wstring> LogRows() { return {'''

code = stubs + rows + '\n}\nvoid Activate(int i) { switch(page) { case Page::Settings: if (i == 0)' + activation + 'default: break; } }\nvoid Load() { const bool util_log_requested =' + load + '\n}\n}\n' + performance
code += r'''
int main() {
 using namespace refunct;
 auto has = [](const wchar_t* text) { return std::any_of(messages.begin(),messages.end(),[&](const auto& s){return s.find(text)!=std::wstring::npos;}); };
 assert(LogRows().size()==4);
 assert(LogRows()[0]==L"Save Logs: ON");
 assert(LogRows()[1]==L"Startup Logs: ON");
 assert(LogRows()[2]==L"Clear Logs");
 assert(LogRows()[3]==L"Back");
 Activate(0); assert(page==Page::Hotkeys); page=Page::Settings;
 Activate(1); assert(page==Page::Persistence); page=Page::Settings;
 Activate(2); assert(page==Page::Logs && page_changes==3);
 Activate(1); assert(!g_startup_log_enabled && !saved_startup && g_util_log_enabled);
 assert(LogRows()[1]==L"Startup Logs: OFF");
 Activate(0); assert(!g_util_log_enabled && !g_performance_monitor_enabled);
 Activate(1); assert(g_startup_log_enabled && saved_startup && !g_util_log_enabled);
 assert(LogRows()[1]==L"Startup Logs: ON (paused)");
 Activate(0); assert(g_util_log_enabled && g_performance_monitor_enabled && saved_util && saved_perf);
 Activate(1); assert(!g_startup_log_enabled && !saved_startup && g_performance_monitor_enabled);
 Activate(2); assert(page==Page::ConfirmClearLogs && clear_count==0);
 Activate(1); assert(page==Page::Logs && clear_count==0);
 Activate(2); Activate(0); assert(page==Page::Logs && clear_count==1);
 // Reports really reach the log path and use fresh samples after toggling.
 for(int i=0;i<600;++i) {
   { PerformanceScope tick(PerfArea::Tick); {PerformanceScope water(PerfArea::Water); now_qpc+=100;} }
   now_qpc+=9900;
 }
 assert(has(L"Performance Water:")); assert(has(L"character tick intervals:"));
 // Simulate turning the monitor off and back on inside an active tick.
 messages.clear();
 { PerformanceScope outer(PerfArea::Tick);
   { PerformanceScope water(PerfArea::Water); now_qpc+=100; }
   Activate(0); Activate(0);
   { PerformanceScope water(PerfArea::Water); now_qpc+=100; }
 }
 messages.clear();
 for(int i=0;i<600;++i) {
   { PerformanceScope tick(PerfArea::Tick); {PerformanceScope native(PerfArea::NativeTick); now_qpc+=100;} }
   now_qpc+=9900;
 }
 assert(has(L"Performance Native tick:")); assert(!has(L"Performance Water:"));
 Activate(0); assert(!g_util_log_enabled && !g_performance_monitor_enabled && !saved_util && !saved_perf);
 const int before=clock_calls; {PerformanceScope tick(PerfArea::Tick);} assert(clock_calls==before);
 Activate(3); assert(back==3 && page==Page::Settings);
 Activate(2); assert(page==Page::Logs);
 // Master OFF must override contradictory old settings, even after restart.
 saved_util=false; saved_perf=true; saved_startup=true; Load();
 assert(configured && !g_util_log_enabled && !g_performance_monitor_enabled && g_startup_log_enabled);
 saved_util=true; saved_perf=false; saved_startup=false; Load();
 assert(g_util_log_enabled && g_performance_monitor_enabled && !g_startup_log_enabled);
 Activate(0); assert(!g_util_log_enabled && !g_performance_monitor_enabled && !saved_perf);
 Activate(2); Activate(0); assert(page==Page::Logs && clear_count==2 && !g_util_log_enabled);
}
'''
compiler = shutil.which('g++') or shutil.which('clang++')
assert compiler
with tempfile.TemporaryDirectory() as temp:
    cpp = Path(temp) / 'test.cpp'
    exe = Path(temp) / 'test'
    cpp.write_text(code)
    subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', '-O2', '-I', str(root/'include'), str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('PASS: actual menu rows and actions, persistence, log reports, session reset and disabled timing')

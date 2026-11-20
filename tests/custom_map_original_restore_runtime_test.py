"""Compile real map restore functions against a small state simulator.

This checks recovery state transitions without claiming UE4 runtime validation.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/maps.cpp').read_text(encoding='utf-8')
restart = (root / 'src/restart.cpp').read_text(encoding='utf-8')
header = (root / 'include/refunct/features/maps.hpp').read_text(encoding='utf-8')

def extract_function(signature):
    begin = source.index(signature)
    brace = source.index('{', begin)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == '{':
            depth += 1
        elif source[index] == '}':
            depth -= 1
            if not depth:
                return source[begin:index + 1]
    raise AssertionError(f'Unterminated {signature}')

for name in ('OnCustomMapFullLevelReloadStart', 'OnCustomMapFullLevelReloadResult'):
    assert f'void {name}(' in header
    assert f'{name}(' in restart, f'{name} is not connected to native reload'
assert 'if (s_map.awaiting_original_reload || s_map.original_reload_unconfirmed) return;' in source
assert 's_map.awaiting_original_reload) return false;' in source
assert 'OnCustomMapFullLevelReloadStart();' in restart
assert 'OnCustomMapFullLevelReloadResult(true);' in restart
assert restart.count('OnCustomMapFullLevelReloadResult(false);') >= 2
assert 'QueueOriginalWorldReload(L"native rollback failed")' in source

functions = '\n\n'.join(extract_function(signature) for signature in (
    'bool QueueOriginalWorldReload(',
    'bool RequestCustomMapRestore(',
    'void OnCustomMapFullLevelReloadStart(',
    'void OnCustomMapFullLevelReloadResult(',
))

prelude = r'''
#include <string>
#include <vector>
#include <cassert>
struct Map {};
struct TestState {
    bool applying=false, snapshot=false, restoring=false, active=false;
    bool restart_after_change=false;
    bool awaiting_original_reload=false, original_reload_unconfirmed=false;
    int cursor=0, last_resets=-1, restart_reapply_delay=0;
    std::wstring status, current_file;
    std::vector<int> actors;
    struct Empty {} roots,api;
    Map loaded_map;
} s_map;
bool valid_snapshot=false, root_ready=true, reload_allowed=true;
int s_source_offset=0, s_speed_offset=0;
int reload_calls=0, cheat_calls=0;
std::vector<std::wstring> logs, notices;
bool CurrentSnapshotValid() { return valid_snapshot; }
bool BeginApplyRoots(bool restoring) { assert(restoring); return root_ready; }
bool RequestFullLevelReload() { ++reload_calls; return reload_allowed; }
void MarkMapAsCheat() { ++cheat_calls; }
void Log(const wchar_t* msg) { logs.emplace_back(msg); }
void Notify(const std::wstring& msg) { notices.push_back(msg); }
void MapError(const std::wstring& msg) { s_map.status=L"Map error: "+msg; }
'''

checks = r'''
int main() {
    // No custom map ever loaded: original world is already present.
    assert(RequestCustomMapRestore());
    assert(reload_calls == 0 && !s_map.awaiting_original_reload);
    assert(s_map.status == L"Original world already active");

    // In-use original snapshot expired: schedule native travel, not stale writes.
    s_map.active = true;
    s_map.snapshot = true;
    s_map.actors = {1,2};
    s_map.current_file = L"StarWorld.json";
    assert(RequestCustomMapRestore());
    assert(reload_calls == 1 && cheat_calls == 1);
    assert(s_map.awaiting_original_reload);
    assert(!RequestCustomMapRestore());  // no duplicate reload requests
    OnCustomMapFullLevelReloadStart();
    assert(!s_map.active && !s_map.snapshot && s_map.actors.empty());
    assert(s_source_offset == -1 && s_speed_offset == -1);
    assert(s_map.current_file.empty());
    OnCustomMapFullLevelReloadResult(true);
    assert(!s_map.awaiting_original_reload);
    assert(s_map.status == L"Original world restored (level reloaded)");

    // If the native reload can't be queued, don't silently clear world state.
    s_map.active = true;
    s_map.current_file = L"OtherMap.json";
    s_map.actors = {3};
    reload_allowed = false;
    assert(!RequestCustomMapRestore());
    assert(s_map.active && s_map.actors.size() == 1);
    assert(!s_map.awaiting_original_reload);

    // A native reload can be requested after root preflight failure.
    reload_allowed = true;
    valid_snapshot = true;
    root_ready = false;
    assert(RequestCustomMapRestore());
    assert(s_map.awaiting_original_reload);
    OnCustomMapFullLevelReloadResult(false);
    assert(!s_map.awaiting_original_reload);
    assert(s_map.original_reload_unconfirmed);
    assert(s_map.status == L"Map error: original world reload not confirmed");

    // Failed OpenLevel after native actors were cleared must be retryable.
    valid_snapshot = false;
    s_map.active = false;
    s_map.snapshot = false;
    s_map.current_file.clear();
    assert(RequestCustomMapRestore());
    assert(s_map.awaiting_original_reload);
    OnCustomMapFullLevelReloadResult(true);
    assert(!s_map.original_reload_unconfirmed);

    // Original snapshot works: preserve the non-reload restore method.
    root_ready = true;
    valid_snapshot = true;
    s_map.applying = false;
    const auto previous_calls = reload_calls;
    assert(RequestCustomMapRestore());
    assert(reload_calls == previous_calls);
    assert(s_map.applying && s_map.restoring && s_map.restart_after_change);

    // Manual full-level reload also invalidates the custom map's actor handles.
    s_map.awaiting_original_reload = false;
    s_map.active = true;
    s_map.snapshot = true;
    s_map.current_file = L"SomeMap.json";
    OnCustomMapFullLevelReloadStart();
    assert(!s_map.active && !s_map.snapshot && s_map.current_file.empty());
    assert(!s_map.restart_after_change);
    assert(s_map.status == L"No custom map loaded");
}
'''
compiler = shutil.which('clang++') or shutil.which('g++')
if not compiler:
    raise SystemExit('No portable C++ compiler available')
with tempfile.TemporaryDirectory() as temp:
    cpp = Path(temp) / 'restore.cpp'
    exe = Path(temp) / 'restore'
    cpp.write_text(prelude + '\n' + functions + '\n' + checks, encoding='utf-8')
    subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror',
                    '-o', str(exe), str(cpp)], check=True, timeout=60)
    subprocess.run([str(exe)], check=True, timeout=15)
print('PASS: original-map restore compiled simulation (clean, expired, failed, valid, manual reload)')

"""Execute the real map-update completion logic against small native mocks.

Validates R restart queuing only after successful user-selected changes, not
while applying, on reapply, after rollback, or during a full-level reload.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/maps.cpp').read_text(encoding='utf-8')
tick = (root / 'src/game_tick.cpp').read_text(encoding='utf-8')


def extract_function(signature: str) -> str:
    start = source.index(signature)
    brace = source.index('{', start)
    depth = 0
    for i in range(brace, len(source)):
        if source[i] == '{':
            depth += 1
        elif source[i] == '}':
            depth -= 1
            if not depth:
                return source[start:i + 1]
    raise AssertionError(f'Cannot extract {signature}')


assert 's_map.restart_after_change = true;' in extract_function('bool RequestCustomMapLoad(')
assert 's_map.restart_after_change = true;' in extract_function('bool RequestCustomMapRestore(')
assert 's_map.restart_after_change = false;' in extract_function('bool QueueOriginalWorldReload(')
assert 's_map.restart_after_change = false;' in extract_function('void OnCustomMapFullLevelReloadStart(')
assert 's_map.restart_after_change = false; // failure rollback' in extract_function('void UpdateCustomMapLoad(')
assert tick.index('UpdateCustomMapLoad();') < tick.index('if (g_restart_requested) {')
assert 'if (RestartRun(player))' in tick

prelude = r'''
#include <array>
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>
struct Map {};
struct LevelState { int resets = 0; } state;
struct NativeActor { int handle = 42; };
struct NativeRoot { struct { void* actor = nullptr; } handle; };
struct TestState {
    std::vector<NativeActor> actors;
    std::array<NativeRoot,31> roots{};
    Map loaded_map;
    std::wstring current_file = L"test.json", status;
    size_t cursor = 0;
    bool snapshot=true, applying=true, restoring=false, active=false;
    bool restart_after_change=true, awaiting_original_reload=false, original_reload_unconfirmed=false;
    int last_resets=0, restart_reapply_delay=0;
} s_map;
struct LARGE_INTEGER { long long QuadPart=0; };
void* g_practice_level_actors[31]{};
bool g_restart_requested=false;
bool valid_handles=true, preflight=true, should_apply=true;
int apply_calls=0, rollback_calls=0, root_calls=0, fallback_calls=0, notifications=0;
const LevelState* PracticeLevelState() { return &state; }
bool ValidHandle(const decltype(NativeRoot::handle)&) { return valid_handles; }
bool CurrentSnapshotValid() { return valid_handles; }
bool BeginApplyRoots(bool) { ++root_calls; return preflight; }
bool ApplyNextActor(const NativeActor&, bool) { ++apply_calls; return should_apply; }
bool QueueOriginalWorldReload(const wchar_t*) { ++fallback_calls; return true; }
bool QueryPerformanceFrequency(LARGE_INTEGER*) { return false; }
bool QueryPerformanceCounter(LARGE_INTEGER*) { return false; }
void MapError(const std::wstring& message) { s_map.status = L"Map error: " + message; }
void Log(const wchar_t*) {}
void Notify(const wchar_t*) { ++notifications; }
void Notify(const std::wstring&) { ++notifications; }
'''
checks = r'''
void Reset() {
    s_map = TestState{};
    s_map.actors = std::vector<NativeActor>(9); // more than one tick of work
    g_practice_level_actors[0] = reinterpret_cast<void*>(0x1234);
    s_map.roots[0].handle.actor = g_practice_level_actors[0];
    g_restart_requested = false;
    valid_handles = preflight = should_apply = true;
    state.resets = 0;
    apply_calls = rollback_calls = root_calls = fallback_calls = 0;
}
int main() {
    // Loading a user-selected custom map should not interrupt a partial apply.
    Reset();
    UpdateCustomMapLoad();
    assert(s_map.applying && !g_restart_requested && s_map.cursor == 8);
    UpdateCustomMapLoad();
    assert(!s_map.applying && s_map.active && g_restart_requested);
    assert(!s_map.restart_after_change && s_map.status == L"Loaded: test.json");
    assert(fallback_calls == 0);

    // A subsequent R restart causes reapply, but no *second* automatic restart.
    g_restart_requested = false;
    state.resets = 1;
    UpdateCustomMapLoad();
    assert(s_map.restart_reapply_delay == 1);
    UpdateCustomMapLoad();
    assert(s_map.applying && !g_restart_requested);
    UpdateCustomMapLoad();
    assert(!s_map.applying && s_map.active && !g_restart_requested);

    // Selecting original map should restart once only after restoration finishes.
    Reset();
    s_map.restoring = true;
    UpdateCustomMapLoad();
    assert(!g_restart_requested);
    UpdateCustomMapLoad();
    assert(!s_map.active && g_restart_requested && s_map.current_file.empty());
    assert(s_map.status == L"Original world restored");

    // A map-transform failure initiates rollback and must never trigger R.
    Reset();
    should_apply = false;
    UpdateCustomMapLoad();
    assert(s_map.restoring && !s_map.restart_after_change);
    assert(!g_restart_requested);
    should_apply = true;
    UpdateCustomMapLoad();
    UpdateCustomMapLoad();
    assert(!g_restart_requested && !s_map.active);

    // Full-level recovery owns its native restart; never trigger an R here.
    Reset();
    s_map.awaiting_original_reload = true;
    UpdateCustomMapLoad();
    assert(!g_restart_requested && apply_calls == 0);

    // An expired actor handle cancels rather than restarting a broken map.
    Reset();
    valid_handles = false;
    UpdateCustomMapLoad();
    assert(!g_restart_requested && !s_map.restart_after_change && !s_map.applying);
}
'''
compiler = shutil.which('clang++') or shutil.which('g++')
if not compiler:
    raise SystemExit('No C++20 compiler available')
with tempfile.TemporaryDirectory() as temp:
    cpp = Path(temp) / 'map_switch.cpp'
    exe = Path(temp) / 'map_switch'
    cpp.write_text(prelude + '\n' + extract_function('void UpdateCustomMapLoad(') + '\n' + checks,
                   encoding='utf-8')
    subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror',
                    str(cpp), '-o', str(exe)], check=True, timeout=30)
    subprocess.run([str(exe)], check=True, timeout=10)
print('PASS: compiled custom-map switch, original restore, rollback, restart reapply and reload tests')

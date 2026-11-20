"""Compile the actual UpdateButtonFix loop against a portable game-state mock.

Reproduce normal button 20 dwell, a genuine broken next-button gate, a future
button, and explicit manual recovery without needing a Windows x86 runtime.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "src/sections.cpp").read_text(encoding="utf-8")
start = source.index("void UpdateButtonFix() {")
end = source.index("\n} // namespace", start)
actual = source[start:end]

mock = r"""
#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>
using ULONGLONG = unsigned long long;
struct LevelState { int buttons=0, resets=0; };
struct Button {
    void* actor = nullptr;
    void* get_fn = nullptr;
    int get_result_offset = 0;
    bool pressed = true, collision = false;
    bool visual_applied = false, visual_active = false;
    unsigned visual_write_serial = 0;
    int section = 0;
};
struct SectionStub {
    bool ready = true, buttons_initialized = true;
    struct {bool active=false;} press_batch;
    std::array<Button, 37> buttons{};
    std::wstring button_init_issue = L"";
} s_sections;
std::array<void*,37> s_button_fix_actor_cache{};
std::array<bool,37> s_button_fix_repaired{};
std::array<bool,37> s_button_fix_state_known{};
std::array<bool,37> s_button_fix_prior_pressed{};
std::array<bool,37> s_button_fix_prior_collision{};
std::array<bool,37> s_button_fix_seen_healthy{};
std::array<uint32_t,37> s_button_fix_visual_serial{};
std::array<ULONGLONG,37> s_button_fix_near_since{};
std::array<ULONGLONG,37> s_button_fix_last_repair_ms{};
bool s_button_fix_manual_pending = false;
bool s_button_fix_auto_scan_active = false;
bool world_options_active = true;
bool removed_buttons = false;
enum class WorldObjectKind { Buttons };
bool IsWorldObjectRemoved(WorldObjectKind) { return removed_buttons; }
bool AnyWorldGameplayOptionActive() { return world_options_active; }
unsigned s_button_fix_manual_repairs = 0;
ULONGLONG s_button_fix_next_status_log=0;
unsigned s_button_fix_attempts=0;
ULONGLONG s_button_fix_retry_after_ms=0;
bool s_button_fix_faulted = false;
ULONGLONG s_button_fix_next_check=0;
int s_button_fix_last_resets=-1;
int kRefunctButtonCount=37;
LevelState state{};
ULONGLONG clock_ms=10000;
bool near_button=true, platform_raised=true;
int repairs=0, read_calls=0;
std::vector<int> repaired_indices{};
int function_stub=123;
int actor_stub=456;
bool enabled_process_event=true;
void PE(void* actor,void* fn,void* bytes) {
    assert(fn==&function_stub);
    ++read_calls;
    reinterpret_cast<uint8_t*>(bytes)[0] = static_cast<Button*>(actor)->collision;
}
void (*g_process_event)(void*,void*,void*) = PE;
bool g_full_level_reload_requested=false, g_restart_requested=false;
bool g_preloaded_restart_requested=false,g_practice_restart_requested=false;
bool g_practice_active=false;
enum class PracticePhase {Ready,Idle,Other};
PracticePhase g_practice_phase=PracticePhase::Ready;
ULONGLONG GetTickCount64(){ return clock_ms; }
void Log(const wchar_t*){}
void Notify(const std::wstring&){}
bool InitializeSectionButtons(){return true;}
const LevelState* PracticeLevelState(){return &state;}
bool ReadButtonPressed(const Button& b,bool& p){if(!b.actor)return false; p=b.pressed;return true;}
bool IsSectionPhysicallyRaised(int, bool& raised){raised=platform_raised;return true;}
bool ButtonFixPlayerIsNear(const Button&){return near_button;}
bool RepairUsableButton(int idx){++repairs; repaired_indices.push_back(idx); s_sections.buttons[idx].pressed=false; s_sections.buttons[idx].collision=true;return true;}
void Setup() {
    s_sections = {};
    s_sections.ready=true;
    s_button_fix_actor_cache.fill(nullptr);
    s_button_fix_repaired.fill(false);
    s_button_fix_state_known.fill(false);
    s_button_fix_prior_pressed.fill(false);
    s_button_fix_prior_collision.fill(false);
    s_button_fix_seen_healthy.fill(false);
    s_button_fix_visual_serial.fill(0);
    s_button_fix_near_since.fill(0);
    s_button_fix_last_repair_ms.fill(0);
    s_button_fix_manual_pending=false;
    s_button_fix_auto_scan_active=false;
    world_options_active=true;
    removed_buttons=false;
    s_button_fix_manual_repairs=0;
    s_button_fix_next_status_log=0;
    s_button_fix_attempts=0;
    s_button_fix_faulted=false;
    s_button_fix_retry_after_ms=0;
    s_button_fix_next_check=0;
    s_button_fix_last_resets=-1;
    state={19,0}; // 19 completed, current next is one-based button 20
    clock_ms=10000; repairs=0; read_calls=0; near_button=true; platform_raised=true;
    repaired_indices.clear();
    Button& button=s_sections.buttons[19];
    button.actor=&button;
    button.get_fn=&function_stub;
    button.pressed=false;
    button.collision=true;
}
"""
main=r"""
int main(){
    // No World gameplay modifications: automatic scan must do zero UE calls.
    Setup();
    world_options_active=false;
    UpdateButtonFix();
    assert(read_calls==0 && repairs==0);

    // A World option enables the full native 37-button observation pass.
    Setup();
    for (auto& button : s_sections.buttons) {
        button.actor=&button;
        button.get_fn=&function_stub;
        button.pressed=true;
        button.collision=false;
    }
    auto& current=s_sections.buttons[19];
    current.pressed=false;
    current.collision=true;
    world_options_active=true;
    UpdateButtonFix();
    assert(read_calls==37 && repairs==0);
    // After the option is turned off, no native calls happen on the next pass.
    world_options_active=false;
    clock_ms+=400;
    UpdateButtonFix();
    assert(read_calls==37 && !s_button_fix_auto_scan_active);
    // Turning it on again must observe a healthy gate before any repair.
    current.pressed=true;
    current.collision=false;
    world_options_active=true;
    clock_ms+=400;
    UpdateButtonFix();
    for (int j=0;j<10;++j){clock_ms+=400;UpdateButtonFix();}
    assert(repairs==0);

    // Removing buttons intentionally disables collision, so the automatic
    // ghost-button repair must not revert that setting.
    Setup();
    removed_buttons=true;
    UpdateButtonFix();
    assert(read_calls==0 && repairs==0);

    // Manual 37-button repair bypasses the gate with every World option OFF.
    Setup();
    for (auto& button : s_sections.buttons) {
        button.actor=&button;
        button.get_fn=&function_stub;
        button.pressed=true;
        button.collision=false;
    }
    s_sections.buttons[19].pressed=false;
    s_sections.buttons[19].collision=true;
    world_options_active=false;
    s_button_fix_manual_pending=true;
    UpdateButtonFix();
    assert(read_calls==37);

    // The reported bug: camping at the NORMAL, pressable button 20 must
    // never invoke Reenable or flag a run, even after many cooldowns.
    Setup();
    for(int i=0;i<110;++i){UpdateButtonFix();clock_ms+=400;}
    assert(repairs==0);

    // Future or completed buttons must not be automatically modified.
    Setup();
    auto& future=s_sections.buttons[19];
    future.pressed=true;future.collision=false;
    state.buttons=18; // button 20 is not the next one
    for(int i=0;i<40;++i){UpdateButtonFix();clock_ms+=400;}
    assert(repairs==0);

    // A gate which is initially locked is ambiguous, not proof of a ghost.
    Setup();
    s_sections.buttons[19].pressed=true;
    s_sections.buttons[19].collision=false;
    for(int i=0;i<40;++i){UpdateButtonFix();clock_ms+=400;}
    assert(repairs==0);

    // REAL regression: the current button was healthy, then its native gate
    // breaks. Only after two seconds of sustained proximity should we repair.
    Setup();
    UpdateButtonFix();clock_ms+=400; // first observe healthy
    auto& broken=s_sections.buttons[19];
    broken.pressed=true;broken.collision=false;
    for(int i=0;i<5;++i){UpdateButtonFix();clock_ms+=400;}
    assert(repairs==0);
    for(int i=0;i<10;++i){UpdateButtonFix();clock_ms+=400;}
    assert(repairs==1 && repaired_indices.at(0)==19);
    for(int i=0;i<40;++i){UpdateButtonFix();clock_ms+=400;}
    assert(repairs==1);

    // Do not auto-repair a section that is not physically available.
    Setup();
    s_sections.buttons[19].pressed=true;
    s_sections.buttons[19].collision=false;
    platform_raised=false;
    for(int i=0;i<40;++i){UpdateButtonFix();clock_ms+=400;}
    assert(repairs==0);

    // Even a broken next-button gate isn't proof when the player is away.
    Setup();
    s_sections.buttons[19].pressed=true;
    s_sections.buttons[19].collision=false;
    near_button=false;
    for(int i=0;i<40;++i){UpdateButtonFix();clock_ms+=400;}
    assert(repairs==0);

    // Manual recovery still permits refreshing a healthy-looking ghost.
    Setup();
    s_button_fix_manual_pending=true;
    UpdateButtonFix();
    assert(repairs==1 && repaired_indices.at(0)==19);
    std::cout<<"PASS: gated 37-button scan; button 20 healthy; future; fault; manual\n";
}
"""
compiler=shutil.which('g++') or shutil.which('clang++')
if not compiler: raise SystemExit('SKIP: no C++ compiler')
with tempfile.TemporaryDirectory(prefix='button20_') as td:
    cpp=Path(td)/'test.cpp'
    exe=Path(td)/'test'
    cpp.write_text(mock+actual+main,encoding='utf-8')
    subprocess.run([compiler,'-std=c++20','-Wall','-Wextra','-Werror',str(cpp),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)

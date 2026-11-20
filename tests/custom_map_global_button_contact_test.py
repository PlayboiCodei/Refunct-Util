"""Compile the real global-button hook and polling logic against a fake game state.

Tests multiple button presses between ticks and ensures that a native press
is not overwritten by a later end-of-frame player position. No real UE4 APIs.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
practice = (root / 'src/practice.cpp').read_text(encoding='utf8')
sections = (root / 'src/sections.cpp').read_text(encoding='utf8')
header = (root / 'include/refunct/features/practice.hpp').read_text(encoding='utf8')

start = practice.index('void CaptureGlobalLastButtonContact(MyCharacter* player,')
end = practice.index('\n// Called on the game thread after the original tick.', start)
new_functions = practice[start:end]

assert 'CaptureGlobalLastButtonContact(player, before, after);' in sections
assert sections.index('g_original_add_based_character(actor, player);') < sections.index('CaptureGlobalLastButtonContact(player, before, after);')
assert 'g_button_state_initialized && !g_practice_active' not in sections.split('const auto invoke_native = [&]() {')[1].split('};')[0]
assert 'void CaptureGlobalLastButtonContact(' in header
assert 's_native_global_contact_since_tick = true;' in new_functions
assert 'if (!native_global_contact) s_global_last_contact = contact;' in new_functions

prelude = r'''
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>
namespace refunct {
constexpr int kRefunctButtonCount = 37;
constexpr int VK_SHIFT=16;
struct Vec3 { float x{}, y{}, z{}; };
struct Rotator {float pitch{},yaw{},roll{};};
struct Checkpoint {int buttons{}; Vec3 location{};Rotator rotation{};};
struct ButtonContact {Checkpoint checkpoint{}; bool valid{},crouched{},stance_valid{};Vec3 velocity{};bool velocity_valid{};};
struct MovementComponent {Vec3 velocity{};};
struct MyCharacter {MovementComponent* movement{};Vec3 position{};};
struct LevelState {int buttons{},resets{};};
enum class PracticePhase {Idle,Ready};
ButtonContact s_last_contact{},s_second_last_contact{},s_global_last_contact{},s_selected_contact{};
bool s_native_global_contact_since_tick = false;
bool s_last_button_stance_pending=false,s_last_button_momentum_pending=false;
Checkpoint g_last_button_checkpoint{};
bool g_last_button_checkpoint_valid = false;
bool g_practice_active=false, g_flying=false,g_noclip=false;
bool g_preloaded_restart_armed=false,g_preloaded_reset_seen=false,g_preloaded_restart_active=false;
PracticePhase g_practice_phase=PracticePhase::Idle;
int g_observed_buttons=2,g_observed_resets=0;
int saves=0,loads=0, teleports=0;
bool synthetic=false;
Vec3 last_teleport{};
std::wstring notification, diagnostic;
LevelState live_state{2,0};
void LoadButtonHistoryOnce(){ ++loads; }
void PersistButtonHistory(){ ++saves; }
const LevelState* PracticeLevelState(){ return &live_state; }
bool IsSyntheticPracticeButtonContact(){ return synthetic; }
bool CaptureCheckpoint(MyCharacter* p, Checkpoint& cp){
    if (!p) return false;
    cp.buttons=live_state.buttons; cp.location=p->position;
    return true;
}
struct CrouchState {bool valid{},value{};};
CrouchState ReadCrouchedState(MyCharacter*) {return {};}
bool HasFocusedGameWindow(){return false;}
short GetAsyncKeyState(int){return 0;}
bool IsReadable(const void* p,std::size_t){return p!=nullptr;}
bool ValidLastButtonVelocity(const Vec3&){return true;}
bool ValidButtonContact(const ButtonContact& c){return c.valid && c.checkpoint.buttons>0;}
void ResetMovementOnNormalRestart(MyCharacter*) {}
std::wstring CheckpointFloat(float a){return std::to_wstring(a);}
void Log(const wchar_t* txt){diagnostic=txt;}
void Notify(const std::wstring& n){notification=n;}
std::wstring ButtonLabel(int n){return std::to_wstring(n);}
bool TeleportToCoordinates(MyCharacter*,const Vec3& pos, bool allow_ready){
    assert(allow_ready); last_teleport=pos; ++teleports;return true;
}
'''

main = r'''
int main(){
    using namespace refunct;
    MovementComponent movement{};
    MyCharacter player{&movement,{10.f,20.f,30.f}};
    // A real custom-map press. The player moves before polling this tick.
    live_state.buttons=3;
    CaptureGlobalLastButtonContact(&player,2,3);
    assert(s_global_last_contact.valid);
    assert(s_global_last_contact.checkpoint.location.x==10.f);
    assert(diagnostic.find(L"native last button 3")!=std::wstring::npos);
    player.position={900.f,901.f,902.f};
    TrackLastButton(&player);
    assert(s_last_contact.checkpoint.location.x==900.f); // vanilla B/N unchanged
    assert(s_global_last_contact.checkpoint.location.x==10.f); // event-time pose kept

    // Two real contacts in one frame: 3->4->5, which old +1 polling misses.
    player.position={40.f,50.f,60.f};
    live_state.buttons=4;
    CaptureGlobalLastButtonContact(&player,3,4);
    player.position={70.f,80.f,90.f};
    live_state.buttons=5;
    CaptureGlobalLastButtonContact(&player,4,5);
    player.position={1000.f,1001.f,1002.f};
    TrackLastButton(&player);
    assert(s_global_last_contact.checkpoint.buttons==5);
    assert(s_global_last_contact.checkpoint.location.x==70.f);

    // Synthetic replay must never replace the real last button.
    synthetic=true;
    player.position={1.f,2.f,3.f};
    live_state.buttons=6;
    CaptureGlobalLastButtonContact(&player,5,6);
    assert(s_global_last_contact.checkpoint.buttons==5);
    synthetic=false;

    // Button progress can reset when the player changes maps, yet the next
    // native press must replace the old global target, not require a +1 tick.
    live_state.buttons=0;
    live_state.resets=1;
    TrackLastButton(&player);
    live_state.buttons=1;
    player.position={-450.f,120.f,390.f};
    CaptureGlobalLastButtonContact(&player,0,1);
    player.position={3333.f,3333.f,3333.f};
    TrackLastButton(&player);
    assert(s_global_last_contact.checkpoint.buttons==1);
    assert(s_global_last_contact.checkpoint.location.x==-450.f);
    assert(saves>=4);
    return 0;
}
'''
with tempfile.TemporaryDirectory() as folder:
    source = Path(folder) / 'test.cpp'
    binary = Path(folder) / 'test'
    source.write_text(prelude + new_functions + '\n}\n' + main, encoding='utf8')
    result = subprocess.run(['clang++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                             str(source), '-o', str(binary)], capture_output=True,
                            text=True, timeout=45)
    assert result.returncode == 0, result.stderr
    subprocess.run([str(binary)], check=True, timeout=10)
print('PASS: native real-contact capture, multi-button frame, restart, synthetic suppression, no B/N regression')

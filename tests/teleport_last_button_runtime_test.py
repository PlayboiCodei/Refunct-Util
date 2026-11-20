"""Portable C++ simulation: custom-map-aware, post-tick global last button teleport."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
s = (root / "src/practice.cpp").read_text(encoding="utf8")
start = s.index("void TeleportToLastPressedButton(MyCharacter* player) {")
end = s.index("\nint SecondLastButtonNumber()", start)
action = s[start:end]
restart_start = s.index("void ClearButtonHistoryOnRestart() {")
restart_end = s.index("\n// Restart with Progress", restart_start)
restart = s[restart_start:restart_end]
prelude = r'''
#include <cassert>
#include <cmath>
#include <cstdint>
#include <string>
#include <cstddef>
namespace refunct {
using ULONGLONG = std::uint64_t;
struct Vec3 { float x{},y{},z{};};
struct SceneComponent {Vec3 location{};};
struct MovementComponent {Vec3 velocity{},acceleration{};};
struct MyCharacter {SceneComponent* root_component{};MovementComponent* movement{};};
struct Checkpoint {int buttons{}; Vec3 location{};};
struct ButtonContact {Checkpoint checkpoint{};bool valid{};};
struct PendingGlobalTeleport {Vec3 destination{};int button_number=0; ULONGLONG requested_ms=0;int settle_frames=0;bool active=false,placed=false,waiting_logged=false;};
PendingGlobalTeleport s_pending_global_teleport{};
ButtonContact s_last_contact{},s_second_last_contact{},s_selected_contact{},s_global_last_contact{};
Checkpoint g_last_button_checkpoint{};
bool g_last_button_checkpoint_valid=false;
bool s_last_button_stance_pending=false,s_last_button_momentum_pending=false;
bool g_practice_active=false,g_restart_requested=false,g_practice_restart_requested=false;
bool g_full_level_reload_requested=false,g_preloaded_restart_requested=false;
enum class PracticePhase {Idle,Ready,Stabilize};
PracticePhase g_practice_phase=PracticePhase::Idle;
int saves=0,loads=0,calls=0;
ULONGLONG tick=1000;
bool busy=false,teleport_success=true,seen_allow_ready=false;
std::wstring notification,diagnostic;
Vec3 target{};
bool IsReadable(const void* p,std::size_t){return p!=nullptr;}
bool ValidButtonContact(const ButtonContact& c){return c.valid && c.checkpoint.buttons>0;}
void LoadButtonHistoryOnce(){++loads;}
void PersistButtonHistory(){++saves;}
void Notify(const std::wstring& n){notification=n;}
void Log(const wchar_t* n){diagnostic=n;}
std::wstring ButtonLabel(int n){return std::to_wstring(n);}
std::wstring CheckpointFloat(float v){return std::to_wstring(v);}
bool CustomMapActive(){return true;}
bool CustomMapApplying(){return busy;}
ULONGLONG GetTickCount64(){return tick;}
bool TeleportToCoordinates(MyCharacter* player,const Vec3& destination,bool allow_ready){
  seen_allow_ready=allow_ready;
  ++calls; target=destination;
  if (!teleport_success) return false;
  player->root_component->location=destination;
  return true;
}
'''
main = r'''
int main(){
 using namespace refunct;
 SceneComponent scene{{2.f,4.f,6.f}};
 MovementComponent movement{};
 MyCharacter player{&scene,&movement};
 TeleportToLastPressedButton(&player);
 assert(calls==0 && notification.find(L"No last button")!=std::wstring::npos);
 s_last_contact={{3,{1.f,2.f,3.f}},true};
 g_last_button_checkpoint_valid=true;
 s_global_last_contact={{7,{23.5f,-451.f,940.5f}},true};
 ClearButtonHistoryOnRestart();
 assert(!s_last_contact.valid && s_global_last_contact.valid && saves==1);
 // Never teleport while an asynchronous custom map is moving the world.
 busy=true;
 TeleportToLastPressedButton(&player);
 assert(s_pending_global_teleport.active && calls==0);
 UpdateGlobalLastButtonTeleport(&player);
 assert(calls==0 && diagnostic.find(L"waiting")!=std::wstring::npos);
 busy=false;
 // Practice checkpoint stabilization also delays teleport.
 g_practice_active=true;g_practice_phase=PracticePhase::Stabilize;
 UpdateGlobalLastButtonTeleport(&player);assert(calls==0);
 g_practice_phase=PracticePhase::Ready;
 UpdateGlobalLastButtonTeleport(&player);
 assert(calls==1 && seen_allow_ready && target.x==23.5f);
 assert(scene.location.x==23.5f);
 // Native physics overwrote the position after the first placement.
 scene.location={1.f,2.f,3.f};
 UpdateGlobalLastButtonTeleport(&player);
 assert(scene.location.x==23.5f && scene.location.y==-451.f);
 UpdateGlobalLastButtonTeleport(&player);
 UpdateGlobalLastButtonTeleport(&player);
 assert(!s_pending_global_teleport.active);
 assert(diagnostic.find(L"COMPLETE")!=std::wstring::npos);
 assert(notification.find(L"progress unchanged")!=std::wstring::npos);
 assert(!g_last_button_checkpoint_valid && saves==1);
 // A failed actual placement must not claim success.
 teleport_success=false;
 TeleportToLastPressedButton(&player);
 UpdateGlobalLastButtonTeleport(&player);
 assert(!s_pending_global_teleport.active && calls==2);
 assert(diagnostic.find(L"FAILED")!=std::wstring::npos);
 // Stalled map loading times out, never teleporting into partial geometry.
 teleport_success=true;
 busy=true;
 TeleportToLastPressedButton(&player);
 tick+=10001;
 UpdateGlobalLastButtonTeleport(&player);
 assert(!s_pending_global_teleport.active);
 assert(diagnostic.find(L"FAILED")!=std::wstring::npos);
 return 0;
}
'''
with tempfile.TemporaryDirectory() as folder:
    source=Path(folder)/"test.cpp"
    binary=Path(folder)/"test"
    source.write_text(prelude + action + "\n" + restart + "\n}\n" + main,encoding="utf8")
    compile_result=subprocess.run(["clang++","-std=c++20","-Wall","-Wextra","-Werror",str(source),"-o",str(binary)],capture_output=True,text=True,timeout=30)
    assert compile_result.returncode == 0, compile_result.stderr
    subprocess.run([str(binary)],check=True,timeout=10)
print("PASS: queued global teleport waits for map/practice, stabilizes position, reports success/failure")

"""Compile the actual FOV state machine against a camera-manager mock."""
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/movement.cpp').read_text()
tick = (root / 'src/game_tick.cpp').read_text()
assert tick.index('UpdateGhostReplayViewer(player);') < tick.index('ApplyPlayerFov(player);')
assert tick.count('ApplyPlayerFov(player);') == 1
state = source[source.index('MyCharacter* s_fov_player = nullptr;'):source.index('// APlayerController::FOV is an exec UFUNCTION')]
code = source[source.index('void ResetPlayerFovTracking() {'):source.index('// TAS: UWorld')]
prelude = r'''
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <string>
using ULONGLONG = std::uint64_t;
struct UObjectHeader {};
struct PlayerController {};
struct MyCharacter { PlayerController* controller; };
struct TunedFloatProperty { float* address=nullptr; float original=0, last_written=0; bool modified=false; };
static int g_player_fov_degrees=0;
static bool replay=false, available=true;
static float locked=0, component_fov=90;
static int calls=0, component_calls=0;
static ULONGLONG clock_ms=100;
bool IsReadable(const void* p, size_t) {return p!=nullptr;}
ULONGLONG GetTickCount64() {return clock_ms;}
bool IsGhostReplayFirstPersonActive(const MyCharacter*) {return replay;}
void Log(const wchar_t*) {}
void Notify(const wchar_t*) {}
bool ApplyControllerFov(PlayerController*,float degrees) {++calls; if (!available) return false; locked=degrees; return true;}
bool ReadRenderedFov(PlayerController*,float& out) {out=locked; return true;}
void* FindFirstPersonCamera(MyCharacter*) {return &component_fov;}
TunedFloatProperty ResolveTunedFloat(void*,const wchar_t*,float,float) {return {&component_fov,component_fov,0,false};}
bool SetCameraComponentFov(void*,float val) {++component_calls; component_fov=val; return true;}
'''
main = r'''
int main() {
    PlayerController controller{};
    MyCharacter player{&controller}, respawn{&controller};
    ApplyPlayerFov(&player); assert(calls==0);
    replay=true; ApplyPlayerFov(&player); assert(locked==130 && calls==1);
    assert(g_player_fov_degrees==0 && component_calls==0);
    for (int i=0;i<240;++i) ApplyPlayerFov(&player);
    assert(calls==1);
    replay=false; ApplyPlayerFov(&player); assert(locked==0 && calls==2);
    g_player_fov_degrees=105; ApplyPlayerFov(&player); assert(locked==105);
    replay=true; ApplyPlayerFov(&player); assert(locked==130 && g_player_fov_degrees==105);
    replay=false; ApplyPlayerFov(&player); assert(locked==105);
    g_player_fov_degrees=0; replay=true; ApplyPlayerFov(&player); assert(locked==130);
    replay=false; ApplyPlayerFov(&respawn); assert(locked==0); // shared controller survives respawn
    available=false; replay=true; int before=calls; ApplyPlayerFov(&respawn);
    for (int i=0;i<240;++i) ApplyPlayerFov(&respawn);
    assert(calls==before+1 && component_calls==0);
    clock_ms+=2500; available=true; ApplyPlayerFov(&respawn); assert(locked==130);
    replay=false; available=false; before=calls; ApplyPlayerFov(&respawn);
    for (int i=0;i<240;++i) ApplyPlayerFov(&respawn);
    assert(calls==before+1);
    clock_ms+=1000; available=true; ApplyPlayerFov(&respawn); assert(locked==0);
    g_player_fov_degrees=-10; ApplyPlayerFov(&respawn); assert(component_fov==-10);
    replay=true; ApplyPlayerFov(&respawn); assert(locked==130 && component_fov==-10);
    replay=false; ApplyPlayerFov(&respawn); assert(locked==0 && component_fov==-10);
    replay=true; ApplyPlayerFov(&respawn); assert(locked==130);
    replay=false; g_player_fov_degrees=0; ApplyPlayerFov(&respawn); assert(locked==0 && component_fov==90);
}
'''
compiler = shutil.which('g++') or shutil.which('clang++')
assert compiler
with tempfile.TemporaryDirectory() as tmp:
    cpp = Path(tmp)/'test.cpp'; exe = Path(tmp)/'test'
    cpp.write_text(prelude + state + code + main)
    subprocess.run([compiler,'-std=c++20','-Wall','-Wextra','-Werror',str(cpp),'-o',str(exe)],check=True,timeout=60)
    subprocess.run([str(exe)],check=True,timeout=10)
print('PASS: replay FOV lock, restore default/custom, respawn and bounded retries')

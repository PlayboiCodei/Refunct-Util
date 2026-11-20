"""Compile the real key-hook code with a simulated Refunct native input handler."""
from pathlib import Path
import subprocess, shutil, tempfile
root=Path(__file__).resolve().parents[1]
source=(root/'src/input.cpp').read_text(encoding='utf-8')
start=source.index('namespace {\n// A key held before entering Free Cam')
end=source.rindex('} // namespace refunct')
actual=source[start:end]
harness=r'''
#include <array>
#include <atomic>
#include <cstdint>
#include <cassert>
#define __fastcall
constexpr int VK_F5=116,VK_F4=115,VK_MENU=18,VK_SPACE=32,VK_CONTROL=17,VK_SHIFT=16,VK_LSHIFT=160,VK_RSHIFT=161;
constexpr int VK_LEFT=37,VK_UP=38,VK_RIGHT=39,VK_DOWN=40;
constexpr int VK_OEM_2=191,VK_RETURN=13,VK_ESCAPE=27,VK_BACK=8;
namespace refunct {
enum class Action {Menu};
int Hotkey(Action) {return VK_F5;}
struct Binding { std::atomic_int value{0}; int load(std::memory_order o) const{return value.load(o);} };
std::array<Binding,4> g_keys{};
std::array<std::atomic_int,256> g_pending{};
std::atomic_bool g_pending_any{false};
std::array<std::atomic_int,256> g_suppress_keyup{};
std::atomic_bool g_menu_open{false}, g_rebinding{false},g_name_editing{false},g_coordinate_editing{false};
bool freecam=false, orbit=false, menu_search=false;
int native_down=0,native_up=0;
void NativeKeyDown(void*,int,uint32_t,bool) {++native_down;}
void NativeKeyUp(void*,int,uint32_t,bool) {++native_up;}
auto g_original_keydown=&NativeKeyDown;
auto g_original_keyup=&NativeKeyUp;
int GetAsyncKeyState(int){return 0;}
bool HasFocusedGameWindow(){return true;}
bool IsGhostReplayFreeCameraActive(){return freecam;}
bool IsGhostReplayOrbitCameraActive(){return orbit;}
bool IsMenuSearchTypingKey(int key){return menu_search && key>='A' && key<='Z';}
void QueueMenuSearchKey(int){}
'''
main=r'''
int main() {
    using namespace refunct;
    // Already-held movement must receive its native KeyUp after Free Cam starts.
    KeyDownHook(nullptr,nullptr,'W',0,false);
    assert(native_down==1);
    freecam=true;
    KeyDownHook(nullptr,nullptr,'W',0,true);
    assert(native_down==1);
    KeyUpHook(nullptr,nullptr,'W',0,false);
    assert(native_up==1);
    // A freecam-only press never enters Refunct's native player input.
    g_pending['A'].store(0);
    KeyDownHook(nullptr,nullptr,'A',0,false);
    assert(native_down==1 && g_pending['A'].load()==0);
    KeyUpHook(nullptr,nullptr,'A',0,false);
    assert(native_up==1);
    freecam=false;
    KeyDownHook(nullptr,nullptr,'A',0,false);
    KeyUpHook(nullptr,nullptr,'A',0,false);
    assert(native_down==2 && native_up==2);
    // Free Cam continues flying while F5 menu is open, without moving player.
    g_menu_open.store(true);
    freecam=true;
    const int menu_native_down=native_down, menu_native_up=native_up;
    for (const int key : {int('W'),int('S'),int('A'),int('D'),VK_SPACE,VK_SHIFT}) {
        g_pending[key].store(0);
        KeyDownHook(nullptr,nullptr,key,0,false);
        KeyUpHook(nullptr,nullptr,key,0,false);
        assert(g_pending[key].load()==0);
    }
    assert(native_down==menu_native_down && native_up==menu_native_up);
    // Menu arrows are still forwarded to the overlay (never to native UE4).
    g_pending[VK_LEFT].store(0);
    KeyDownHook(nullptr,nullptr,VK_LEFT,0,false);
    KeyUpHook(nullptr,nullptr,VK_LEFT,0,false);
    assert(g_pending[VK_LEFT].load()==1);
    assert(native_down==menu_native_down && native_up==menu_native_up);
    // Opening search preserves text entry (W must reach the search queue).
    menu_search=true;
    KeyDownHook(nullptr,nullptr,'W',0,false);
    KeyUpHook(nullptr,nullptr,'W',0,false);
    assert(native_down==menu_native_down && native_up==menu_native_up);
    menu_search=false;
    // Hotkey capture must take priority over Free Cam movement.
    g_rebinding.store(true);
    g_pending['W'].store(0);
    KeyDownHook(nullptr,nullptr,'W',0,false);
    KeyUpHook(nullptr,nullptr,'W',0,false);
    assert(g_pending['W'].load()==1);
    g_rebinding.store(false);
    // Flight-only keys go to the spectator, not the real character.
    g_menu_open.store(false);
    freecam=true;
    const int before_down=native_down, before_up=native_up;
    for (const int key : {VK_SPACE,VK_SHIFT,VK_LSHIFT,VK_RSHIFT}) {
        KeyDownHook(nullptr,nullptr,key,0,false);
        KeyUpHook(nullptr,nullptr,key,0,false);
    }
    assert(native_down==before_down && native_up==before_up);
    // E, Q and Ctrl aren't spectator flight controls; leave them untouched.
    for (const int key : {int('E'),int('Q'),VK_CONTROL}) {
        KeyDownHook(nullptr,nullptr,key,0,false);
        KeyUpHook(nullptr,nullptr,key,0,false);
    }
    assert(native_down==before_down+3 && native_up==before_up+3);
    // A Shift press that started while playing still receives native release.
    freecam=false;
    KeyDownHook(nullptr,nullptr,VK_SHIFT,0,false);
    const int before_shift_up=native_up;
    freecam=true;
    KeyUpHook(nullptr,nullptr,VK_SHIFT,0,false);
    assert(native_up==before_shift_up+1);
    // Orbit now uses the mouse wheel, not arrow keys. F5 arrows still
    // navigate the menu without reaching the actual character.
    freecam=false;
    orbit=true;
    g_menu_open.store(true);
    const int orbit_down=native_down, orbit_up=native_up;
    g_pending[VK_UP].store(0);
    KeyDownHook(nullptr,nullptr,VK_UP,0,false);
    KeyUpHook(nullptr,nullptr,VK_UP,0,false);
    assert(g_pending[VK_UP].load()==1);
    assert(native_down==orbit_down && native_up==orbit_up);
    return 0;
}
'''
compiler=shutil.which('g++') or shutil.which('clang++')
assert compiler
with tempfile.TemporaryDirectory(prefix='replay_freecam_keyhook_') as td:
    cpp=Path(td)/'mock.cpp'
    cpp.write_text(harness+actual+'}\n'+main,encoding='utf-8')
    exe=Path(td)/'test'
    subprocess.run([compiler,'-std=c++20','-O2','-Wall','-Wextra','-Werror',str(cpp),'-o',str(exe)],check=True,timeout=45)
    subprocess.run([str(exe)],check=True,timeout=10)
print('PASS: freecam F5 key isolation, menu navigation, text capture and key releases')

"""Compile Refunct's actual Win32 wheel hook using an isolated fake message thread.

Ensures orbit zoom works with scroll-jump OFF, steals wheel events from the
player only during orbit, and restores scroll-jump behaviour on exit.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
src = (root / 'src/input.cpp').read_text()
start = src.index('LRESULT CALLBACK MouseWheelInputHook(')
end = src.index('\nnamespace {', start)
actual = src[start:end]
assert 'QueueGhostReplayOrbitWheel(delta)' in actual
assert '!g_scroll_to_jump && !IsGhostReplayOrbitCameraActive()' in actual
assert 'return 1;' in actual

mock = r'''
#include <cassert>
#include <atomic>
#include <cstdint>
#include <string>
using HWND=void*; using HHOOK=void*; using DWORD=unsigned long;
using ULONGLONG=unsigned long long;
using LRESULT=long; using WPARAM=std::uintptr_t; using LPARAM=std::uintptr_t;
#define CALLBACK
#define HC_ACTION 0
#define WM_MOUSEWHEEL 0x020A
#define WH_MOUSE 7
#define HIWORD(v) ((static_cast<unsigned long>(v) >> 16) & 0xffff)
struct MOUSEHOOKSTRUCTEX { unsigned long mouseData=0; };
namespace refunct {
bool g_scroll_to_jump=false;
std::atomic_bool g_rebinding{false},g_name_editing{false},g_coordinate_editing{false};
std::atomic<int> g_wheel_jump_requests{0};
HHOOK g_mouse_wheel_hook=nullptr;
DWORD g_mouse_wheel_thread=0;
ULONGLONG g_mouse_wheel_retry_after=0;
bool g_mouse_wheel_warning_logged=false;
bool orbit=false;
int queued_zoom=0,call_next=0,install_count=0,uninstall_count=0;
HWND GetForegroundWindow(){return reinterpret_cast<HWND>(1);}
DWORD GetCurrentProcessId(){return 77;}
DWORD GetWindowThreadProcessId(HWND,DWORD* pid){*pid=77;return 88;}
ULONGLONG GetTickCount64(){return 5000;}
bool HasFocusedGameWindow(){return true;}
bool IsGhostReplayOrbitCameraActive(){return orbit;}
void QueueGhostReplayOrbitWheel(short delta){queued_zoom+=delta;}
LRESULT CallNextHookEx(HHOOK,int,WPARAM,LPARAM){++call_next;return -44;}
HHOOK SetWindowsHookExW(int,LRESULT (CALLBACK*)(int,WPARAM,LPARAM),HHOOK,DWORD){++install_count;return reinterpret_cast<HHOOK>(0x12);}
bool UnhookWindowsHookEx(HHOOK){++uninstall_count;return true;}
void Log(const wchar_t*){}
void Notify(const wchar_t*){}
''' + actual + r'''
}
int main(){
  using namespace refunct;
  MOUSEHOOKSTRUCTEX ev{};
  ev.mouseData=static_cast<unsigned long>(static_cast<unsigned short>(+120))<<16;
  orbit=true; // Orbit must install the hook even with Scroll Jump disabled.
  UpdateMouseWheelHook();
  assert(install_count==1 && g_mouse_wheel_hook!=nullptr);
  assert(MouseWheelInputHook(HC_ACTION,WM_MOUSEWHEEL,reinterpret_cast<LPARAM>(&ev))==1);
  assert(queued_zoom==120 && g_wheel_jump_requests==0 && call_next==0);
  // Zoom remains exclusive even if scroll-jump is enabled too.
  g_scroll_to_jump=true;
  ev.mouseData=static_cast<unsigned long>(static_cast<unsigned short>(-240))<<16;
  assert(MouseWheelInputHook(HC_ACTION,WM_MOUSEWHEEL,reinterpret_cast<LPARAM>(&ev))==1);
  assert(queued_zoom==-120 && g_wheel_jump_requests==0);
  // When orbit ends, scroll jump works in both wheel directions again.
  orbit=false;
  assert(MouseWheelInputHook(HC_ACTION,WM_MOUSEWHEEL,reinterpret_cast<LPARAM>(&ev))==-44);
  ev.mouseData=static_cast<unsigned long>(static_cast<unsigned short>(+120))<<16;
  assert(MouseWheelInputHook(HC_ACTION,WM_MOUSEWHEEL,reinterpret_cast<LPARAM>(&ev))==-44);
  assert(g_wheel_jump_requests==2);
  g_scroll_to_jump=false;
  UpdateMouseWheelHook();
  assert(uninstall_count==1 && g_mouse_wheel_hook==nullptr);
  assert(g_wheel_jump_requests==0);
  return 0;
}
'''
compiler = shutil.which('g++') or shutil.which('clang++')
assert compiler
with tempfile.TemporaryDirectory(prefix='refunct_replay_scrollhook_') as tmp:
    cpp = Path(tmp)/'hook.cpp'
    exe = Path(tmp)/'hook'
    cpp.write_text(mock)
    subprocess.run([compiler,'-std=c++20','-O2','-Wall','-Wextra','-Werror',
                    str(cpp),'-o',str(exe)],check=True,timeout=90)
    subprocess.run([str(exe)],check=True,timeout=10)
print('PASS: Win32 orbit wheel +120/-240, scroll-jump isolation, install without scroll-jump, cleanup')

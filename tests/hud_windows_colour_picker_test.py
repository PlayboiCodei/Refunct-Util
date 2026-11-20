"""Compile the actual HUD Windows-picker method and result handoff with portable Win32 mocks.

This tests menu interactions, async handoff semantics, cancel/error behaviour,
RGB channel ordering and original-element targeting without requiring Windows.
"""
from pathlib import Path
from tempfile import TemporaryDirectory
import subprocess

root = Path(__file__).resolve().parents[1]
menu = (root / 'src/menu.cpp').read_text(encoding='utf-8')
tick = (root / 'src/game_tick.cpp').read_text(encoding='utf-8')
header = (root / 'include/refunct/features/menu.hpp').read_text(encoding='utf-8')
state = (root / 'src/storage.cpp').read_text(encoding='utf-8')

assert 'void PollHudColourPicker();' in header
assert 'PollHudColourPicker(); // Apply picked HUD RGB' in tick
assert 'g_hud_custom_colours[target] = result;' in menu
assert 'g_hud_colour_modes[target] = 10;' in menu
assert 'SaveSettings();' in menu
assert 'if (result != kHudPickerNoResult) s_hud_picker_target = kHudElementCount;' in menu
assert 's_hud_picker_target = s_hud_style_element;' in menu
assert 's_hud_picker_result.load(std::memory_order_acquire) != kHudPickerNoResult' in menu
assert 'std::array<bool, kHudElementCount> s_hud_use_windows_picker{};' in menu
assert 'CycleHudCustomColourMethod();' in menu
assert 'else if (i == 2) OpenHudCustomColourMethod();' in menu
assert 'HudCustomColourMethodLabel(),' in menu
assert '#include <commdlg.h>' in menu
assert 'ChooseColorW(&choice)' in menu
assert 'CommDlgExtendedError() ? kHudPickerError' in menu
# Existing INI field names and persistence mechanisms must remain unchanged.
for key in ('ColourMode', 'CustomRGB', 'SizePercent'):
    assert f'L"{key}"' in state
assert 's_hud_use_windows_picker' not in state

rgb_func = menu.split('std::wstring HudRgbString(uint32_t rgb) {', 1)[1].split('// Choose the input method independently', 1)[0]
worker = 'std::array<bool, kHudElementCount> s_hud_use_windows_picker{};' + menu.split('std::array<bool, kHudElementCount> s_hud_use_windows_picker{};', 1)[1].split('void CycleHudColour(int direction) {', 1)[0]
selector = menu.split('std::wstring HudCustomColourMethodLabel() {', 1)[1].split('// UI-only method selection', 1)[0]
crosshair_picker = 'std::wstring CrosshairColourLabel() {' + menu.split('std::wstring CrosshairColourLabel() {',1)[1].split('void AdjustCrosshairRow(',1)[0]
poll = menu.split('void PollHudColourPicker() {', 1)[1].split('std::wstring MenuSelectionDescription(', 1)[0]
assert 'g_hud_' not in worker.split('DWORD WINAPI HudColourPickerThread(LPVOID) {', 1)[1].split('void OpenHudColourPicker()', 1)[0], 'Worker wrote HUD state'

mock_cpp = r'''
#include <array>
#include "refunct/crosshair.hpp"
using namespace refunct;
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <string>
#include <vector>
#include <iostream>
using DWORD=uint32_t;
using LPVOID=void*;
using HWND=void*;
using HANDLE=void*;
using COLORREF=uint32_t;
#define WINAPI
constexpr uint32_t CC_RGBINIT=1, CC_FULLOPEN=2, CC_ANYCOLOR=4;
constexpr size_t kHudElementCount=3;
const wchar_t* kHudPositionLabels[3]={L"Timer", L"Speed", L"FPS"};
size_t s_hud_style_element=0;
int s_tile_hex_axis=-1;
std::wstring s_tile_hex_input;
std::array<uint32_t,kHudElementCount> g_hud_custom_colours{0xFFFFFFu,0xABCDEFu,0x224466u};
std::array<uint8_t,kHudElementCount> g_hud_colour_modes{0,5,2};
struct CHOOSECOLORW {
 size_t lStructSize{}; HWND hwndOwner{}; COLORREF* lpCustColors{};
 COLORREF rgbResult{}; uint32_t Flags{};
};
static bool should_choose=true, create_thread_fails=false;
static uint32_t chosen=0, seen_initial=0;
static DWORD extended_error=0;
static int picker_calls=0, settings_saves=0;
std::vector<std::wstring> notifications;
uint32_t RGB(uint32_t r,uint32_t g,uint32_t b) { return r | (g<<8) | (b<<16); }
uint32_t GetRValue(uint32_t x) {return x&255u;}
uint32_t GetGValue(uint32_t x) {return (x>>8)&255u;}
uint32_t GetBValue(uint32_t x) {return (x>>16)&255u;}
HWND GetForegroundWindow() {return reinterpret_cast<HWND>(1);}
DWORD GetCurrentProcessId() {return 123;}
DWORD GetWindowThreadProcessId(HWND, DWORD* id) {*id=123;return 1;}
DWORD CommDlgExtendedError() { return extended_error; }
bool ChooseColorW(CHOOSECOLORW* choice) {
 ++picker_calls;
 assert(choice->lStructSize==sizeof(CHOOSECOLORW));
 assert(choice->lpCustColors);
 assert(choice->Flags==(CC_RGBINIT|CC_FULLOPEN|CC_ANYCOLOR));
 seen_initial=(GetRValue(choice->rgbResult)<<16)|(GetGValue(choice->rgbResult)<<8)|GetBValue(choice->rgbResult);
 if (!should_choose) return false;
 choice->rgbResult=RGB((chosen>>16)&255,(chosen>>8)&255,chosen&255);
 return true;
}
HANDLE CreateThread(void*, size_t, DWORD (WINAPI *fn)(LPVOID), void*, DWORD, DWORD*) {
 if (create_thread_fails) return nullptr;
 // A synchronous stub is sufficient: the atomic mailbox remains pending
 // until the next game-tick PollHudColourPicker call.
 fn(nullptr);
 return reinterpret_cast<HANDLE>(1);
}
void CloseHandle(HANDLE) {}
void Notify(const std::wstring& text) {notifications.push_back(text);}
void SaveSettings() {++settings_saves;}
uint32_t ghost_rgb=0x45EBFFu;
void SetPBGhostColourRgb(uint32_t rgb) {ghost_rgb=rgb;}
template<size_t N> int swprintf_s(wchar_t (&buffer)[N], const wchar_t* format, unsigned int number) {
 return std::swprintf(buffer,N,format,number);
}
'''
code = mock_cpp + '\nstd::wstring HudRgbString(uint32_t rgb) {' + rgb_func + '\n' + worker + '\n' + crosshair_picker + '\n' + 'std::wstring HudCustomColourMethodLabel() {' + selector + '\n' + 'void PollHudColourPicker() {' + poll + '''
int main() {
    // Per-element arrow mode, with user-visible label and HEX editor.
    s_hud_style_element=1;
    assert(HudCustomColourMethodLabel().find(L"HEX #ABCDEF")!=std::wstring::npos);
    CycleHudCustomColourMethod();
    assert(HudCustomColourMethodLabel().find(L"Windows picker")!=std::wstring::npos);

    // Correct RGB order, original target preserved after navigating away.
    chosen=0x12AB34;
    OpenHudCustomColourMethod();
    assert(seen_initial==0xABCDEF);
    assert(g_hud_colour_modes[1]==5);
    assert(settings_saves==0);
    s_hud_style_element=2;
    PollHudColourPicker();
    assert(g_hud_custom_colours[1]==0x12AB34);
    assert(g_hud_colour_modes[1]==10);
    assert(g_hud_custom_colours[2]==0x224466);
    assert(settings_saves==1);
    assert(s_hud_picker_target==kHudElementCount);

    // Cancellation must not mutate colour or mark a preset Custom.
    s_hud_style_element=2;
    CycleHudCustomColourMethod();
    should_choose=false;
    extended_error=0;
    OpenHudCustomColourMethod();
    PollHudColourPicker();
    assert(g_hud_custom_colours[2]==0x224466);
    assert(g_hud_colour_modes[2]==2);
    assert(settings_saves==1);

    // An actual dialog failure notifies the user but saves nothing.
    extended_error=87;
    OpenHudCustomColourMethod();
    PollHudColourPicker();
    assert(settings_saves==1);
    assert(!notifications.empty());
    assert(notifications.back().find(L"failed to open")!=std::wstring::npos);

    // A result pending on the game thread cannot be replaced by a new picker.
    should_choose=true;
    chosen=0x040506;
    OpenHudCustomColourMethod();
    int calls=picker_calls;
    OpenHudCustomColourMethod();
    assert(picker_calls==calls);
    PollHudColourPicker();
    assert(g_hud_custom_colours[2]==0x040506);
    assert(settings_saves==2);

    // A Win32 CreateThread failure must release the busy flag.
    create_thread_fails=true;
    OpenHudCustomColourMethod();
    assert(!s_hud_picker_open.load());
    assert(settings_saves==2);
    create_thread_fails=false;
    // Ghost picker uses the same worker but must target ghost RGB, not HUD.
    s_hud_picker_target=kGhostColourPickerTarget;
    s_hud_picker_result.store(0x1188EEu);
    PollHudColourPicker();
    assert(ghost_rgb==0x1188EEu);
    assert(g_hud_custom_colours[2]==0x040506u);
    assert(settings_saves==2);

    // Arrow back to six-digit HEX editor (default mode for each new element).
    CycleHudCustomColourMethod();
    assert(HudCustomColourMethodLabel().find(L"HEX #040506")!=std::wstring::npos);
    OpenHudCustomColourMethod();
    assert(s_tile_hex_axis==9);
    assert(s_tile_hex_input.empty());
    assert(settings_saves==2);
    // Crosshair uses the same asynchronous result mailbox, not the HUD or ghost RGB.
    assert(CrosshairColourLabel().find(L"HEX #FFFFFF")!=std::wstring::npos);
    OpenCrosshairColour(); assert(s_tile_hex_axis==19);
    s_crosshair_use_windows_picker=true;
    chosen=0xBB1020u;
    OpenCrosshairColour(); assert(seen_initial==0xFFFFFFu);
    assert(g_crosshair.rgb==0xFFFFFF);
    PollHudColourPicker(); assert(g_crosshair.rgb==0xBB1020 && g_crosshair.style==kCrosshairDiamond);
    assert(ghost_rgb==0x1188EEu && g_hud_custom_colours[2]==0x040506u);
    assert(settings_saves==3);
    should_choose=false; extended_error=0;
    OpenCrosshairColour(); PollHudColourPicker();
    assert(g_crosshair.rgb==0xBB1020 && settings_saves==3);
    std::cout << "PASS HUD Windows picker: RGB, target, cancel, errors, pending, HEX mode\\n";
}
'''
with TemporaryDirectory(prefix='hud_picker_test_') as td:
    source=Path(td)/'hud_picker_test.cpp'
    exe=Path(td)/'hud_picker_test'
    source.write_text(code)
    proc=subprocess.run(['g++','-std=c++20','-Wall','-Wextra','-Werror','-I',str(root/'include'),str(source),'-o',str(exe)],capture_output=True,text=True,timeout=30)
    assert proc.returncode==0,proc.stderr
    proc=subprocess.run([str(exe)],capture_output=True,text=True,timeout=8)
    assert proc.returncode==0,proc.stdout+proc.stderr
    print(proc.stdout.strip())

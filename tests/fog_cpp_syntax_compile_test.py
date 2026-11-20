"""Compile the unchanged fog implementation against an isolated API surface.

This is a host C++20 syntax/type check, not a Win32 ABI or in-game test.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/fog.cpp').read_text()
assert source.startswith('#include "refunct/internal_api.hpp"\n#include <commdlg.h>\n')
source = source.split('\n', 2)[2]
stub = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <string>
#include <utility>

using DWORD = unsigned long;
using ULONGLONG = unsigned long long;
using LPVOID = void*;
using HANDLE = void*;
using COLORREF = unsigned int;
#define WINAPI
#define RGB(r,g,b) ((COLORREF)((r) | ((g) << 8) | ((b) << 16)))
#define swprintf_s(buf, fmt, ...) swprintf((buf), sizeof(buf)/sizeof(*(buf)), (fmt), __VA_ARGS__)
#define CC_ANYCOLOR 1
#define CC_FULLOPEN 2
#define CC_RGBINIT 4
struct CHOOSECOLORW {
    unsigned long lStructSize = 0;
    COLORREF* lpCustColors = nullptr;
    COLORREF rgbResult = 0;
    unsigned long Flags = 0;
};
bool ChooseColorW(CHOOSECOLORW*);
DWORD CommDlgExtendedError();
unsigned int GetRValue(COLORREF);
unsigned int GetGValue(COLORREF);
unsigned int GetBValue(COLORREF);
HANDLE CreateThread(void*,int,DWORD(WINAPI*)(LPVOID),void*,int,void*);
void CloseHandle(HANDLE);
ULONGLONG GetTickCount64();
int GetPrivateProfileIntW(const wchar_t*,const wchar_t*,int,const wchar_t*);
DWORD GetPrivateProfileStringW(const wchar_t*,const wchar_t*,const wchar_t*,wchar_t*,DWORD,const wchar_t*);
bool QueueSettingsValue(const wchar_t*,const wchar_t*,const wchar_t*);

namespace refunct {
struct MyCharacter { int v; };
struct FNameView { uint32_t index, number; };
struct LinearColor { float r,g,b,a; };
struct UObjectHeader { void* vtable; int32_t flags,index; void* class_ptr; FNameView name; void* outer; };
struct UFieldHeader { UObjectHeader object; void* next; };
struct UStructHeader { UFieldHeader field; void* super_struct; void* children; };
struct GlobalObjectItem { UObjectHeader* object; int32_t flags,cluster_index,serial_number; };
struct GlobalObjectArray { int32_t first_gc_index,last_non_gc_index,max_objects_not_considered_by_gc; uint8_t flag; uint8_t padding[3]; GlobalObjectItem* objects; int32_t max_elements,num_elements; };
extern void* g_game_module;
extern void (*g_process_event)(void*,void*,void*);
extern std::wstring g_settings_path;
constexpr uintptr_t kGlobalObjectArrayRva=0x01F046D0;
bool IsReadable(const void*,size_t);
void Notify(const std::wstring&);
void Log(const wchar_t*);
std::wstring UnrealName(FNameView);
int32_t FindClassPropertyOffset(void*,const wchar_t*);
void* FindReflectedFunction(void*,const wchar_t*);
enum class FogRegion : uint8_t { General = 0, Directional = 1 };
bool IsFogColourEnabled(FogRegion);
void SetFogColourEnabled(FogRegion,bool);
std::wstring GetFogColourHex(FogRegion);
void SetFogColourHex(FogRegion,uint32_t);
void ResetFogColours();
void OpenFogColourPicker(FogRegion);
void UpdateFogColours(MyCharacter*);
}
'''
with tempfile.TemporaryDirectory() as tmp:
    cpp = Path(tmp) / 'fog_syntax.cpp'
    cpp.write_text(stub + '\n' + source)
    subprocess.run(['g++', '-std=c++20', '-fsyntax-only', '-Wall', '-Wextra',
                    '-Werror', str(cpp)], timeout=30, check=True)
print('Fog C++20 isolated syntax/type check passed')

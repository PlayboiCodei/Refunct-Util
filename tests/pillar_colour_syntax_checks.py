"""Compile real pillar module against a small fake Win32/UE4 API surface.

This does not replace a Visual Studio Win32 build or an in-game test.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
module = (root / 'src/pillars.cpp').read_text()
assert module.startswith('#include "refunct/internal_api.hpp"\n#include "refunct/colour_work_budget.hpp"\n#include <commdlg.h>\n')
module = module.split('\n', 3)[3]

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
#include <unordered_map>
#include <vector>
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
struct CHOOSECOLORW { unsigned long lStructSize=0; COLORREF* lpCustColors=nullptr;
   COLORREF rgbResult=0; unsigned long Flags=0; };
bool ChooseColorW(CHOOSECOLORW*);
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
struct UObjectPointerArray { void** data; int32_t count; int32_t capacity; };
struct GlobalObjectItem { UObjectHeader* object; int32_t flags,cluster_index,serial_number; };
struct GlobalObjectArray { int32_t first_gc_index,last_non_gc_index,max_objects_not_considered_by_gc;
    uint8_t flag; uint8_t padding[3]; GlobalObjectItem* objects; int32_t max_elements,num_elements; };
struct ColourWorkBudget { explicit ColourWorkBudget(unsigned); bool expired() const {return false;} };
extern void* g_game_module;
extern void (*g_process_event)(void*,void*,void*);
extern std::wstring g_settings_path;
extern bool g_restart_requested, g_full_level_reload_requested;
constexpr uintptr_t kGlobalObjectArrayRva=0x01F046D0;
bool IsReadable(const void*,size_t);
void Notify(const std::wstring&);
void Log(const wchar_t*);
std::wstring UnrealName(FNameView);
int32_t FindClassPropertyOffset(void*,const wchar_t*);
void* FindReflectedFunction(void*,const wchar_t*);
void* ReadReflectedObject(void*,const wchar_t*);
}
'''
with tempfile.TemporaryDirectory() as tmp:
    source = Path(tmp) / 'pillars.cpp'
    source.write_text(stub + '\n' + module)
    subprocess.run(['g++','-std=c++20','-Wall','-Wextra','-Werror','-fsyntax-only',str(source)],
                   timeout=30,check=True)
print('PASS: real pillar C++20 implementation passes isolated syntax/type check')

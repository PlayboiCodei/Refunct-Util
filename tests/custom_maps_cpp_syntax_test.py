"""Strict C++20 source syntax check using minimal Windows/Refunct ABI declarations.

This deliberately does not claim an MSVC Win32 build or native-engine correctness.
"""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
src=(root/'src/maps.cpp').read_text(encoding='utf8')
src=src[src.index('namespace refunct {'):]
prelude=r'''
#include "refunct/custom_map_format.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
namespace refunct {
using DWORD=unsigned long;
using HANDLE=void*;
using LONGLONG=long long;
struct ULARGE_INTEGER { long long QuadPart; };
using LARGE_INTEGER=ULARGE_INTEGER;
struct WIN32_FIND_DATAW { DWORD dwFileAttributes; wchar_t cFileName[260]; };
constexpr DWORD FILE_ATTRIBUTE_DIRECTORY=0x10, FILE_ATTRIBUTE_REPARSE_POINT=0x400;
constexpr DWORD GENERIC_READ=0x80000000u, FILE_SHARE_READ=1, OPEN_EXISTING=3, FILE_ATTRIBUTE_NORMAL=0x80;
#define INVALID_HANDLE_VALUE ((HANDLE)(long)-1)
struct UObjectHeader { void* vtable; int flags,index;void* class_ptr; struct {uint32_t index,number;} name; void* outer; };
struct UObjectPointerArray { void** data; int count,capacity; };
struct GlobalObjectItem { UObjectHeader* object;int flags,cluster_index,serial_number; };
struct GlobalObjectArray {int first_gc_index,last_non_gc_index,max_objects_not_considered_by_gc;uint8_t open_for_disregard_by_gc;uint8_t padding[3];GlobalObjectItem* objects;int max_elements,num_elements;};
struct Vec3 { float x,y,z; };
struct Rotator { float pitch,yaw,roll; };
struct LevelState { int level,platforms,cubes,buttons,resets; };
extern void* g_game_module;
extern std::wstring g_storage_dir;
extern void* g_practice_level_class;
extern std::array<void*,31> g_practice_level_actors;
extern bool g_recording_tainted,g_pb_cheats_allowed_for_run,g_run_pb_eligible,g_restart_requested;
constexpr uintptr_t kGlobalObjectArrayRva=0x01F046D0;
extern void(*g_process_event)(void*,void*,void*);
bool IsReadable(const void*,size_t);
bool IsWritable(const void*,size_t);
bool EnsureDirectory(const std::wstring&);
void* FindReflectedFunction(void*,const wchar_t*);
int32_t FindClassPropertyOffset(void*,const wchar_t*);
std::wstring UnrealName(decltype(UObjectHeader::name));
void Log(const wchar_t*);void Notify(const std::wstring&);
const LevelState* PracticeLevelState();
bool DiscoverPracticeButtons();
bool RequestFullLevelReload();
HANDLE CreateFileW(const wchar_t*,DWORD,DWORD,void*,DWORD,DWORD,void*);
bool GetFileSizeEx(HANDLE,LARGE_INTEGER*);
bool ReadFile(HANDLE,void*,DWORD,DWORD*,void*);
bool CloseHandle(HANDLE);
HANDLE FindFirstFileW(const wchar_t*,WIN32_FIND_DATAW*);
bool FindNextFileW(HANDLE,WIN32_FIND_DATAW*);
bool FindClose(HANDLE);
int _wcsicmp(const wchar_t*,const wchar_t*);
bool QueryPerformanceFrequency(LARGE_INTEGER*);
bool QueryPerformanceCounter(LARGE_INTEGER*);
}
'''
with tempfile.TemporaryDirectory() as td:
    f=Path(td)/'maps.cpp';f.write_text(prelude+'\n'+src,encoding='utf-8')
    proc=subprocess.run(['clang++','-std=c++20','-Wall','-Wextra','-Werror',
                         '-fsyntax-only','-I',str(root/'include'),str(f)],
                        capture_output=True,text=True,timeout=60)
    if proc.returncode:
        print(proc.stderr[-13000:]);raise SystemExit(proc.returncode)
print('PASS native maps.cpp source typechecked as C++20 against ABI stubs')

"""Compile the actual cube implementation against minimal portable ABI doubles.

This detects C++ type errors but is NOT an MSVC Win32 or live-engine test.
"""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
src=(root/'src/cubes.cpp').read_text(encoding='utf-8')
code=src[src.index('namespace refunct {'):]
prelude=r'''
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include "refunct/ghost_replay_cube_contact.hpp"
namespace refunct {
using ULONGLONG = unsigned long long;
struct FNameView { uint32_t index, number; };
struct Vec3 { float x,y,z; };
struct UObjectHeader { void* class_ptr; FNameView name; int index; };
struct GlobalObjectItem { UObjectHeader* object; int flags, cluster_index, serial_number; };
struct GlobalObjectArray { GlobalObjectItem* objects; int max_elements, num_elements; };
constexpr uintptr_t kGlobalObjectArrayRva=0;
extern void* g_game_module;
ULONGLONG GetTickCount64();
struct UObjectPointerArray { void** data; int count,capacity; };
struct MovementComponent { void* cls; };
struct MyCharacter { MovementComponent* movement; };
struct LevelState { int level,cubes,resets; };
constexpr int kTotalCubes=18;
constexpr int kPracticeClusterCount=31;
enum class CubeWorldAction : unsigned char { DisableAll, CollectAll, ShowAll, ShowAboveWater, RestoreDefault };
enum class GhostReplayCubeStatus { Pending, Ready, Unavailable };
enum class CheatAction : unsigned char { Cubes };
extern void (*g_process_event)(void*,void*,void*);
extern std::array<void*,kPracticeClusterCount> g_practice_level_actors;
extern void* g_practice_level_class;
extern bool g_recording_tainted,g_run_pb_eligible;
bool IsReadable(const void*, size_t);
void* FindReflectedFunction(void*,const wchar_t*);
int32_t FindClassPropertyOffset(void*,const wchar_t*);
std::wstring UnrealName(FNameView);
const LevelState* PracticeLevelState();
bool DiscoverPracticeButtons();
void Notify(const std::wstring&);
void Log(const wchar_t*);
void MarkCheatAction(CheatAction);
void ResetWorldCubeCache();
}
'''
with tempfile.TemporaryDirectory() as td:
    source=Path(td)/'cubes.cpp'
    source.write_text(prelude+'\n'+code,encoding='utf-8')
    p=subprocess.run(['clang++','-std=c++20','-Wall','-Wextra','-Werror',
                      '-I',str(root/'include'),'-fsyntax-only',str(source)],capture_output=True,text=True,timeout=30)
    if p.returncode:
        print(p.stderr[-8000:])
        raise SystemExit(p.returncode)
print('PASS: actual native cube source type-checks as C++20')

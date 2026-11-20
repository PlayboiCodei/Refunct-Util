"""Compile and exercise the real world-object-removal module with simulated UE4 APIs.

This tests state capture, reversible actor writes, category isolation,
settings persistence, and taint without requiring Win32 or the game process.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
COMPILER = shutil.which("clang++") or shutil.which("g++")
if not COMPILER:
    raise SystemExit("SKIP: C++ compiler unavailable")

STUB = r'''
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
using ULONGLONG = unsigned long long;
using DWORD = unsigned long;
namespace refunct {
struct MyCharacter {};
struct FNameView { const wchar_t* name = L""; };
struct UObjectHeader { FNameView name{}; void* class_ptr=nullptr; void* outer=nullptr; int32_t flags=0; int32_t index=0; };
struct UStructHeader { UObjectHeader object{}; void* children=nullptr; void* super_struct=nullptr; };
struct UFieldHeader { UObjectHeader object{}; void* next=nullptr; };
struct GlobalObjectItem { UObjectHeader* object=nullptr; int32_t flags=0; int32_t serial_number=0; };
struct GlobalObjectArray { GlobalObjectItem* objects=nullptr; int32_t num_elements=0; int32_t max_elements=0; };
constexpr uintptr_t kGlobalObjectArrayRva = 128;
constexpr uintptr_t kGlobalWorldRva = 64;
extern wchar_t* g_game_module;
extern std::wstring g_settings_path;
extern bool g_recording_tainted, g_pb_cheats_allowed_for_run, g_run_pb_eligible;
extern bool g_restart_requested, g_full_level_reload_requested;
extern void (*g_process_event)(void*, void*, void*);
inline bool IsReadable(const void* value, size_t) { return value != nullptr; }
std::wstring UnrealName(FNameView name);
void* FindReflectedFunction(void* object, const wchar_t* name);
int32_t FindClassPropertyOffset(void* function, const wchar_t* property);
int GetPrivateProfileIntW(const wchar_t*, const wchar_t*, int fallback, const wchar_t*);
bool QueueSettingsValue(const wchar_t*,const wchar_t*,const wchar_t*);
ULONGLONG GetTickCount64();
void Notify(const std::wstring&);
void Log(const wchar_t*);
bool CustomMapApplying();
}
'''
BUDGET = r'''
#pragma once
namespace refunct { class ColourWorkBudget {
public: explicit ColourWorkBudget(int) {} bool expired() const { return false; }
}; }
'''
MAIN = r'''
#include "refunct/internal_api.hpp"
#include "refunct/features/world_object_removal.hpp"
#include <cassert>
#include <memory>
using namespace refunct;
namespace refunct {
wchar_t* g_game_module = nullptr;
std::wstring g_settings_path = L"stub.ini";
bool g_recording_tainted = false, g_pb_cheats_allowed_for_run = false,
     g_run_pb_eligible = true, g_restart_requested = false,
     g_full_level_reload_requested = false;
void (*g_process_event)(void*, void*, void*) = nullptr;
ULONGLONG now = 1000;
std::unordered_map<std::wstring, std::wstring> persisted;
std::wstring UnrealName(FNameView n) { return n.name ? n.name : L""; }
ULONGLONG GetTickCount64() {return now;}
void Notify(const std::wstring&) {}
void Log(const wchar_t*) {}
bool CustomMapApplying() { return false; }
int GetPrivateProfileIntW(const wchar_t*,const wchar_t* key,int fallback,const wchar_t*) {
 auto it=persisted.find(key); return it==persisted.end() ? fallback : std::stoi(it->second);
}
bool QueueSettingsValue(const wchar_t*,const wchar_t* key,const wchar_t* value) {
 persisted[key]=value;return 1;
}
struct Function {
 UStructHeader struct_header{};
 UFieldHeader field{};
};
UObjectHeader bool_type{{L"BoolProperty"}};
std::unordered_map<std::wstring,std::unique_ptr<Function>> functions;
void* FindReflectedFunction(void*, const wchar_t* name) {
 if(std::wstring(name)==L"IsHidden") return nullptr; // Real UE4 C++ method isn't a UFUNCTION.
 auto& fn=functions[name];
 if(!fn) {
  fn=std::make_unique<Function>();
  const std::wstring s(name);
  const wchar_t* prop=s==L"SetActorHiddenInGame" ? L"bNewHidden"
      : s==L"SetActorEnableCollision" ? L"bNewActorEnableCollision" : L"ReturnValue";
  fn->field.object.name={prop};
  fn->field.object.class_ptr=&bool_type;
  fn->struct_header.children=&fn->field;
 }
 return &fn->struct_header;
}
struct NativeActor {
 UObjectHeader header{};
 bool hidden=false, collision=true;
 bool reject_hidden=false, reject_collision=false;
 int hidden_writes=0, collision_writes=0;
};
int32_t FindClassPropertyOffset(void*,const wchar_t* prop) {
 return std::wstring(prop)==L"bHidden" ? static_cast<int32_t>(offsetof(NativeActor,hidden)) : 0;
}
struct HiddenProperty {
 UFieldHeader field{};
 std::array<uint8_t,160> storage{};
 HiddenProperty() {
  field.object.name={L"bHidden"};
  field.object.class_ptr=&bool_type;
  // Simulated UE4 UBoolProperty descriptor at the x86 metadata offset.
  uint8_t* d=reinterpret_cast<uint8_t*>(this)+0x50;
  d[0]=1;d[1]=0;d[2]=1;d[3]=0xFF;
 }
};
void Process(void* object,void* function,void* params) {
 auto& a=*static_cast<NativeActor*>(object);
 auto* fn=static_cast<Function*>(function);
 const auto name=UnrealName(fn->field.object.name);
 auto& x=*static_cast<uint8_t*>(params);
 if(name==L"bNewHidden") {if(!a.reject_hidden) a.hidden = x!=0; ++a.hidden_writes;}
 else if(name==L"bNewActorEnableCollision") {if(!a.reject_collision) a.collision=x!=0; ++a.collision_writes;}
 else if(function == functions[L"IsHidden"].get()) x=a.hidden;
 else if(function == functions[L"GetActorEnableCollision"].get()) x=a.collision;
}
}
int main() {
 g_process_event=&Process;
 std::array<char,1024> module{};
 g_game_module=reinterpret_cast<wchar_t*>(module.data());
 void* world=reinterpret_cast<void*>(0x10);
 std::memcpy(module.data()+kGlobalWorldRva,&world,sizeof(world));
 HiddenProperty hidden_descriptor{};
 UStructHeader tube_cls{},button_cls{},tile_cls{},misc_cls{};
 tube_cls.object.name={L"BP_TravelPipe_C"};
 button_cls.object.name={L"BP_Button_C"};
 tile_cls.object.name={L"BP_IslandChunk_C"};
 misc_cls.object.name={L"SomeOther_C"};
 tube_cls.children=&hidden_descriptor;
 button_cls.children=&hidden_descriptor;
 tile_cls.children=&hidden_descriptor;
 NativeActor tube{}, button{}, tile{}, other{};
 tube.header={{L"BP_TravelPipe_C_1"},&tube_cls,nullptr,0,1};
 button.header={{L"BP_Button_C_1"},&button_cls,nullptr,0,2};
 tile.header={{L"BP_IslandChunk_C_1"},&tile_cls,nullptr,0,3};
 other.header={{L"Other"},&misc_cls,nullptr,0,4};
 button.hidden=true; button.collision=false; // Progress-locked button.
 GlobalObjectItem slots[5]{};
 slots[1]={&tube.header,0,100};
 slots[2]={&button.header,0,100};
 slots[3]={&tile.header,0,100};
 slots[4]={&other.header,0,100};
 GlobalObjectArray arr{slots,5,5};
 std::memcpy(module.data()+kGlobalObjectArrayRva,&arr,sizeof(arr));
 MyCharacter player{};
 assert(!AreAllWorldObjectsRemoved());
 SetWorldObjectRemoved(WorldObjectKind::Tubes,true);
 assert(g_recording_tainted && !g_run_pb_eligible);
 tube.reject_hidden=true;
 UpdateWorldObjectRemovals(&player);
 // Rejected actor setter must not be treated as a completed removal.
 assert(!tube.hidden && !tube.collision);
 tube.reject_hidden=false;
 now+=3000;
 UpdateWorldObjectRemovals(&player);
 assert(tube.hidden && !tube.collision);
 assert(!other.hidden && other.collision);
 assert(!tile.hidden && tile.collision);
 assert(button.hidden && !button.collision);
 now+=2500;
 SetWorldObjectRemoved(WorldObjectKind::Buttons,true);
 UpdateWorldObjectRemovals(&player);
 assert(button.hidden && !button.collision);
 now+=2500;
 SetWorldObjectRemoved(WorldObjectKind::Buttons,false);
 UpdateWorldObjectRemovals(&player);
 assert(button.hidden && !button.collision); // Preserve locked state.
 now+=2500;
 SetWorldObjectRemoved(WorldObjectKind::Tubes,false);
 UpdateWorldObjectRemovals(&player);
 assert(!tube.hidden && tube.collision);
 assert(!AreAllWorldObjectsRemoved());
 SetAllWorldObjectsRemoved(true);
 for(int i=0;i<3;++i){now+=2500; UpdateWorldObjectRemovals(&player);}
 assert(AreAllWorldObjectsRemoved() && tile.hidden && !tile.collision);
 ResetWorldObjectRemovals();
 for(int i=0;i<3;++i){now+=2500; UpdateWorldObjectRemovals(&player);}
 assert(!tube.hidden && tube.collision && !tile.hidden && tile.collision);
 assert(button.hidden && !button.collision);
 assert(!other.hidden && other.collision);
 now+=1000; UpdateWorldObjectRemovals(&player);
 assert(persisted[L"Tubes"]==L"0" && persisted[L"Tiles"]==L"0");
 return 0;
}
'''
with tempfile.TemporaryDirectory() as folder:
    temp = Path(folder)
    (temp / "refunct").mkdir()
    (temp / "refunct" / "internal_api.hpp").write_text(STUB)
    (temp / "refunct" / "state.hpp").write_text('#include "refunct/internal_api.hpp"\n')
    (temp / "refunct" / "colour_work_budget.hpp").write_text(BUDGET)
    main = temp / "main.cpp"
    main.write_text(MAIN)
    exe = temp / "test_world_removal"
    subprocess.run([COMPILER, "-std=c++20", "-O0", "-Wall", "-Wextra", "-I", str(temp),
                    "-I", str(ROOT / "include"), str(ROOT / "src/world_object_removal.cpp"),
                    str(main), "-o", str(exe)], check=True, timeout=45)
    subprocess.run([str(exe)], check=True, timeout=20)
print("PASS: real world-object removal module runtime mock")

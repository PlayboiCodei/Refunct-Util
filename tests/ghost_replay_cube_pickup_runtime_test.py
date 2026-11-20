"""Compile the actual cube module and exercise its replay-owned native pickup path."""
from pathlib import Path
import shutil, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
source=(root/'src/cubes.cpp').read_text()
impl=source[source.index('namespace refunct {'):]
prelude=r'''
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include "refunct/ghost_replay_cube_contact.hpp"
namespace refunct {
using ULONGLONG=unsigned long long;
struct FNameView {uint32_t index,number;};
struct Vec3 {float x,y,z;};
struct UObjectHeader {void* class_ptr;FNameView name;int index;};
struct GlobalObjectItem {UObjectHeader* object;int flags,cluster_index,serial_number;};
struct GlobalObjectArray {GlobalObjectItem* objects;int max_elements,num_elements;};
constexpr uintptr_t kGlobalObjectArrayRva=0;
struct UObjectPointerArray {void** data;int count,capacity;};
struct MovementComponent {void* cls;void* primitive;};
struct MyCharacter {MovementComponent* movement;};
struct LevelState {int level=0,cubes=0,resets=1;};
constexpr int kTotalCubes=18;
enum class CubeWorldAction : unsigned char {DisableAll,CollectAll,ShowAll,ShowAboveWater,RestoreDefault};
enum class GhostReplayCubeStatus {Pending,Ready,Unavailable};
enum class CheatAction : unsigned char {Cubes};
UObjectHeader cube_class{},root_class{},trigger_class{},primitive{};
struct Trigger {UObjectHeader header;Vec3 position;float radius;uint8_t collision;bool sphere;};
struct Actor {UObjectHeader header;void* mesh;void* trigger;uint8_t picked;};
struct Root {UObjectHeader header;UObjectPointerArray collectibles;};
std::array<Actor,18> actors{};
std::array<Trigger,18> triggers{};
std::array<Root,31> roots{};
std::array<void*,18> actor_ptrs{};
std::array<void*,31> g_practice_level_actors{};
void* g_practice_level_class=&root_class;
std::array<GlobalObjectItem,36> items{};
GlobalObjectArray registry{items.data(),36,36};
void* g_game_module=&registry;
MovementComponent movement{&primitive,&primitive};MyCharacter player{&movement};
LevelState state{};
ULONGLONG clock_ms=1000;
ULONGLONG GetTickCount64(){return clock_ms;}
int calls=0,pickups=0,cheats=0;
bool g_recording_tainted=false,g_run_pb_eligible=true;
const LevelState* PracticeLevelState(){return &state;}
bool IsReadable(const void* ptr,size_t){return ptr!=nullptr;}
std::wstring UnrealName(FNameView){return L"BP_PowerCore_C";}
bool DiscoverPracticeButtons(){return true;}
void Notify(const std::wstring&){}
void Log(const wchar_t*){}
void MarkCheatAction(CheatAction){++cheats;}
void ResetWorldCubeCache();
enum Fn : uintptr_t {COLLISION=1,PICKUP,LOCATION,SPHERE,BOUNDS};
void* FindReflectedFunction(void* object,const wchar_t* name){
    const std::wstring n(name);
    if(n==L"GetCollisionEnabled")return reinterpret_cast<void*>(COLLISION);
    if(n.find(L"BndEvt__Trigger")==0)return reinterpret_cast<void*>(PICKUP);
    if(n==L"K2_GetComponentLocation")return reinterpret_cast<void*>(LOCATION);
    if(n==L"GetScaledSphereRadius")return static_cast<Trigger*>(object)->sphere?reinterpret_cast<void*>(SPHERE):nullptr;
    if(n==L"GetActorBounds")return reinterpret_cast<void*>(BOUNDS);
    return nullptr; // Replay must not depend on World visibility/mesh control APIs.
}
int32_t FindClassPropertyOffset(void*,const wchar_t* name){
    const std::wstring n(name);
    if(n==L"Collectibles")return offsetof(Root,collectibles);
    if(n==L"Mesh")return offsetof(Actor,mesh);
    if(n==L"Trigger")return offsetof(Actor,trigger);
    if(n==L"IsPickedUp")return offsetof(Actor,picked);
    if(n==L"UpdatedPrimitive")return offsetof(MovementComponent,primitive);
    if(n==L"ReturnValue"||n==L"OverlappedComponent")return 0;
    if(n==L"OtherActor")return 16;
    if(n==L"OtherComp")return 32;
    if(n==L"OtherBodyIndex")return 48;
    if(n==L"bOnlyCollidingComponents")return 0;
    if(n==L"Origin")return 8;
    if(n==L"BoxExtent")return 24;
    return -1;
}
void dispatch(void* object,void* fn,void* data){
    ++calls;
    auto* b=static_cast<uint8_t*>(data);
    switch(reinterpret_cast<uintptr_t>(fn)){
    case COLLISION:b[0]=static_cast<Trigger*>(object)->collision;break;
    case LOCATION:std::memcpy(b,&static_cast<Trigger*>(object)->position,sizeof(Vec3));break;
    case SPHERE:std::memcpy(b,&static_cast<Trigger*>(object)->radius,sizeof(float));break;
    case BOUNDS:{auto* t=static_cast<Trigger*>(static_cast<Actor*>(object)->trigger);
        const Vec3 extent{t->radius,t->radius,t->radius};
        std::memcpy(b+8,&t->position,sizeof(Vec3));std::memcpy(b+24,&extent,sizeof(Vec3));break;}
    case PICKUP:{auto* a=static_cast<Actor*>(object);void* p=nullptr;
        std::memcpy(&p,b,sizeof(p));assert(p==&primitive);
        std::memcpy(&p,b+16,sizeof(p));assert(p==a);
        std::memcpy(&p,b+32,sizeof(p));assert(p==a->trigger);
        assert(!a->picked);a->picked=1;++state.cubes;++pickups;break;}
    default:assert(false);
    }
}
void (*g_process_event)(void*,void*,void*)=&dispatch;
}
'''
main=r'''
int main(){using namespace refunct;using namespace refunct::ghost_replay_cube_contact;
    const Region sphere{{0,0,0},{20,20,20},true};
    assert(Crossed({-100,0,0},{100,0,0},sphere));
    assert(Crossed({0,0,95},{0,0,95},sphere));
    assert(!Crossed({-100,0,130},{100,0,130},sphere));
    assert(!Crossed({53,53,0},{53,53,0},sphere));
    assert(Crossed({0,0,-150},{0,0,150},sphere));
    assert(!Continuous({0,0,0},{5000,0,0},true));
    assert(!Continuous({0,0,0},{100,0,0},false));
    for(int i=0;i<18;++i){
        actors[i].header={&cube_class,{},i};triggers[i].header={&trigger_class,{},i+18};
        actors[i].trigger=&triggers[i];actor_ptrs[i]=&actors[i];
        triggers[i].position={float(i*500),0,0};triggers[i].radius=20;
        triggers[i].collision=1;triggers[i].sphere=true;
        items[i]={&actors[i].header,0,0,1};items[i+18]={&triggers[i].header,0,0,1};
    }
    for(int i=0;i<31;++i){roots[i].header.class_ptr=&root_class;g_practice_level_actors[i]=&roots[i];}
    roots[0].collectibles={actor_ptrs.data(),18,18};
    triggers[1].radius=0; // both sphere and fallback invalid, other cubes remain usable
    triggers[2].sphere=false; // collision AABB fallback
    actors[3].trigger=nullptr; // missing trigger must not poison all cubes
    triggers[4].position.z=-1000; // section will rise later
    triggers[5].collision=0;
    for(int i=0;i<17;++i)assert(PrepareGhostReplayCubes(&player)==GhostReplayCubeStatus::Pending);
    assert(PrepareGhostReplayCubes(&player)==GhostReplayCubeStatus::Ready);
    assert(s_replay_cubes.valid==16&&!s_cubes.ready&&!GhostReplayCubesChanged());
    const int before=calls;
    TouchGhostReplayCubes(&player,{0,900,0},{100,900,0},true);assert(calls==before);
    TouchGhostReplayCubes(&player,{-100,0,0},{100,0,0},true);assert(pickups==1&&state.cubes==1);
    TouchGhostReplayCubes(&player,{-100,0,0},{100,0,0},true);assert(pickups==1);
    TouchGhostReplayCubes(&player,{900,0,0},{1100,0,0},true);assert(pickups==2&&actors[2].picked);
    triggers[4].position.z=0;
    TouchGhostReplayCubes(&player,{2000,0,150},{2000,0,150},true);assert(pickups==2);
    TouchGhostReplayCubes(&player,{1900,0,0},{2100,0,0},true);assert(pickups==3&&actors[4].picked);
    TouchGhostReplayCubes(&player,{2500,0,0},{2500,0,0},true);assert(pickups==3);
    TouchGhostReplayCubes(&player,{2800,0,0},{3200,0,0},false);assert(pickups==3); // seek, not a swept contact
    TouchGhostReplayCubes(&player,{2800,0,0},{3200,0,0},true);assert(pickups==4&&actors[6].picked);
    ++items[7].serial_number; // reject recycled native actor
    TouchGhostReplayCubes(&player,{3500,0,0},{3500,0,0},true);assert(pickups==4);
    ++state.resets;TouchGhostReplayCubes(&player,{4000,0,0},{4000,0,0},true);assert(pickups==4);
    assert(GhostReplayCubesChanged()&&cheats==0&&g_run_pb_eligible&&!g_recording_tainted);
    ResetGhostReplayCubes();assert(!GhostReplayCubesChanged());
    TouchGhostReplayCubes(&player,{4500,0,0},{4500,0,0},true);assert(pickups==4);
}
'''
with tempfile.TemporaryDirectory() as td:
    f=Path(td)/'mock.cpp'; out=Path(td)/'mock'
    f.write_text(prelude+impl+main)
    subprocess.run([shutil.which('clang++') or 'g++','-std=c++20','-O1','-Wall','-Wextra','-Werror','-I',str(root/'include'),str(f),'-o',str(out)],check=True,timeout=30)
    subprocess.run([str(out)],check=True,timeout=15)
ghost=(root/'src/ghost.cpp').read_text()
stop=ghost[ghost.index('void StopGhostReplayInternal() {'):ghost.index('// Spawn the camera using',ghost.index('void StopGhostReplayInternal() {'))]
assert stop.index('GhostReplayCubesChanged()')<stop.index('ResetGhostReplayCubes()')
assert 'g_restart_requested = true' in stop and 'g_run_pb_eligible = false' in stop
for fn in ('void SeekGhostReplaySeconds','void RestartGhostReplay'):
    block=ghost[ghost.index(fn):ghost.index('\nvoid ',ghost.index(fn)+1)]
    assert 'GhostReplayCubesChanged()' in block
assert 'TouchGhostReplayCubes(player,' in ghost and 'PrepareGhostReplayCubes(player)' in ghost
assert 'src/cubes.cpp' in (root/'CMakeLists.txt').read_text()
print('PASS: native cube callback, swept capsule contact, rising sections, partial discovery, invalid identity, seek, counters and cleanup')

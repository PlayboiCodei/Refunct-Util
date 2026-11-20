"""Native C++ mocks for the *real* UE4 tile/lift preparation state machines."""
from pathlib import Path
import shutil, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
compiler=shutil.which('g++') or shutil.which('clang++')
assert compiler

def compile_test(name, source):
    with tempfile.TemporaryDirectory(prefix=name) as td:
        f=Path(td)/'mock.cpp';out=Path(td)/'mock'
        f.write_text(source,encoding='utf8')
        subprocess.run([compiler,'-std=c++20','-O1','-Wall','-Wextra','-Werror',
                        '-I',str(root/'include'),str(f),'-o',str(out)],
                       check=True,timeout=100)
        subprocess.run([str(out)],check=True,timeout=15)

common=r'''
#include <cassert>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include "refunct/ghost_replay_tile_contact.hpp"
namespace refunct {
using ULONGLONG=unsigned long long;
struct Vec3 {float x,y,z;};
struct MyCharacter {};
struct LevelState {int resets=1;};
struct UObjectHeader { void* class_ptr=nullptr; };
int g_native_calls=0;
uint64_t tick=1000;
uint64_t GetTickCount64(){return tick;}
void Log(const wchar_t*){}
bool IsReadable(const void* p,size_t){return p!=nullptr;}
void add(void*,MyCharacter*){}
void remove(void*,MyCharacter*){}
using BasedCharacterFn=void(*)(void*,MyCharacter*);
BasedCharacterFn g_original_add_based_character=&add;
BasedCharacterFn g_add_based_character=&add;
BasedCharacterFn g_remove_based_character=&remove;
LevelState state{};MyCharacter player{};
MyCharacter* g_player=&player;
const LevelState* PracticeLevelState(){return &state;}
void proc(void*,void*,void* args) {
    auto* buf=static_cast<uint8_t*>(args);
    const Vec3 origin{float(100 + 103*g_native_calls++),0,0};
    const Vec3 extent{20,20,10};
    std::memcpy(buf,&origin,sizeof(origin));
    std::memcpy(buf+16,&extent,sizeof(extent));
}
using ProcessEventFn=void(*)(void*,void*,void*);
ProcessEventFn g_process_event=&proc;
void* FindReflectedFunction(void*,const wchar_t*){return reinterpret_cast<void*>(1);}
'''
# Native tile registration, bounds validation and 4/tick scan.
s=(root/'src/tiles.cpp').read_text(encoding='utf8')
cache=s[s.index('struct GhostReplayTileCache {'):s.index('// Snapshot only the mutable Color',s.index('struct GhostReplayTileCache {'))]
prep=s[s.index('GhostReplayTileStatus PrepareGhostReplayTiles(MyCharacter*'):s.index('void TouchGhostReplayTiles(MyCharacter*')]
tile=common+r'''
constexpr int kTotalColourableTiles=251;
struct TileBatch {
 bool active=false;int count=0;int color_offset=-1,base_mesh_offset=-1;
 void* tile_class=reinterpret_cast<void*>(1);
 std::array<void*,kTotalColourableTiles> actors{};
} s_tiles;
UObjectHeader actors[kTotalColourableTiles]{},meshes[kTotalColourableTiles]{},mesh_class{};
int mesh_calls=0;
bool CollectNativeTileLayout(MyCharacter*,const LevelState&,TileBatch& result) {
 result.count=kTotalColourableTiles;
 for(int i=0;i<kTotalColourableTiles;++i) result.actors[i]=&actors[i];
 return true;
}
bool TileActorValid(void* obj,void*){return obj!=nullptr;}
void* ReadObjectProperty(void* actor,const wchar_t*) {
 const int i=static_cast<int>(static_cast<UObjectHeader*>(actor)-actors);
 return (i>0 && i<kTotalColourableTiles-1) ? &meshes[i] : nullptr;
}
bool PropertyMatches(void*,const wchar_t* name,const wchar_t*,int& offset) {
 const std::wstring n(name);
 offset=n==L"Color" ? 0x02C4 : n==L"BaseMesh" ? 0x02BC : n==L"Max" || n==L"BoxExtent" ? 16 : 0;
 return true;
}
enum class GhostReplayTileStatus {Pending,Ready,Unavailable};
void mesh_proc(void* object,void* fn,void* data) {
 auto* b=static_cast<uint8_t*>(data);
 const auto which=reinterpret_cast<uintptr_t>(fn);
 Vec3 value{};
 ++g_native_calls;
 if(which==1) { // actor fallback; tile 0 valid, last tile invalid
   const int i=static_cast<int>(static_cast<UObjectHeader*>(object)-actors);
   value={float(i*250+100),0,0};
   std::memcpy(b,&value,sizeof(value));
   value=i==kTotalColourableTiles-1 ? Vec3{} : Vec3{20,20,10};
   std::memcpy(b+16,&value,sizeof(value));return;
 }
 ++mesh_calls;
 const int i=static_cast<int>(static_cast<UObjectHeader*>(object)-meshes);
 if(which==2) {
   value={-20,-30,-10}; std::memcpy(b,&value,sizeof(value));
   value={20,30,10};std::memcpy(b+16,&value,sizeof(value));return;
 }
 if(which==3) value={float(i*250+100),0,-500};
 if(which==4) value={0,90,0};
 if(which==5) value={2,1,1};
 std::memcpy(b,&value,sizeof(value));
}
void* tile_fn(void*,const wchar_t* name) {
 const std::wstring n(name);
 return reinterpret_cast<void*>(n==L"GetActorBounds" ? 1 : n==L"GetLocalBounds" ? 2 :
 n==L"K2_GetComponentLocation" ? 3 : n==L"K2_GetComponentRotation" ? 4 : 5);
}
#define FindReflectedFunction tile_fn
'''+cache+prep+r'''
} // namespace refunct
int main() {
 using namespace refunct;
 g_process_event=&mesh_proc;
 for(auto& mesh:meshes) mesh.class_ptr=&mesh_class;
 for(int n=0;n<250;++n) {
    const int before=g_native_calls;
    assert(PrepareGhostReplayTiles(&player)==GhostReplayTileStatus::Pending);
    assert(s_ghost_replay_tiles.collected==n+1);
    assert(g_native_calls-before<=5);
 }
 assert(PrepareGhostReplayTiles(&player)==GhostReplayTileStatus::Ready);
 assert(s_ghost_replay_tiles.valid_regions==250);
 assert(s_ghost_replay_tiles.mesh_regions==249);
 assert(s_ghost_replay_tiles.regions[1].extent.x>29.9f);
 assert(s_ghost_replay_tiles.regions[1].extent.y>39.9f);
 assert(!ghost_replay_tile_contact::Valid(s_ghost_replay_tiles.regions.back()));
 ghost_replay_tile_contact::Bounds current{};
 const int before=g_native_calls;
 assert(ReadGhostReplayTileBounds(&actors[1],1,current));
 assert(g_native_calls==before+1); // only the live mesh location is refreshed
 assert(current.center.z==-500.f);
 assert(PrepareGhostReplayTiles(&player)==GhostReplayTileStatus::Ready);
 return 0;
}
'''
compile_test('replay_tile_prepare_',tile)

# Lift actor registry, native LiftBase class filter, bounded discovery.
s=(root/'src/platforms.cpp').read_text(encoding='utf8')
cache=s[s.index('struct ReplayLiftContact {'):s.index('} // namespace',s.index('struct ReplayLiftContact {'))]
prep=s[s.index('GhostReplayLiftStatus PrepareGhostReplayLifts(MyCharacter*'):s.index('void UpdateGhostReplayLifts(MyCharacter*')]
lift=common+r'''
constexpr size_t kMaximumTargets=256;
struct GlobalObjectItem {UObjectHeader* object=nullptr;int flags=0,serial_number=0;};
struct GlobalObjectArray {int num_elements=0;std::array<GlobalObjectItem,4> objects{};};
GlobalObjectArray registry{};
const GlobalObjectArray* Objects(){return &registry;}
bool NamedPlatform(void* p) {return p==registry.objects[0].object || p==registry.objects[2].object;}
bool InheritsClass(void*,const wchar_t*){return true;}
struct ColourWorkBudget {explicit ColourWorkBudget(int){} bool expired()const{return false;}};
int32_t PlatformParam(void*,const wchar_t* name,const wchar_t*,int) {
 return std::wstring(name)==L"Origin" ? 0 : 16;
}
enum class GhostReplayLiftStatus {Pending,Ready,Unavailable};
'''+cache+r'''
void ResetGhostReplayLifts() {s_replay_lifts={};}
'''+prep+r'''
} // namespace refunct
int main() {
 using namespace refunct;
 UObjectHeader objects[4]{};
 registry.num_elements=4;
 for(int i=0;i<4;++i)registry.objects[i]={objects+i,0,i+5};
 assert(PrepareGhostReplayLifts(&player)==GhostReplayLiftStatus::Ready);
 assert(s_replay_lifts.count==2);
 assert(s_replay_lifts.entries[0].serial==5);
 assert(s_replay_lifts.entries[1].serial==7);
 assert(PrepareGhostReplayLifts(&player)==GhostReplayLiftStatus::Ready);
 return 0;
}
'''
compile_test('replay_lift_prepare_',lift)
print('PASS: production tile bounds preparation (independent mesh and actor bounds with partial failure, bounded per tick), lift UE4 actor discovery')

"""Compile the production replay lift entry/exit/cleanup functions with native mocks."""
from pathlib import Path
import shutil,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
source=(root/'src/platforms.cpp').read_text(encoding='utf8')
a=source.index('void ResetGhostReplayLifts() {')
b=source.index('GhostReplayLiftStatus PrepareGhostReplayLifts(',a)
reset=source[a:b]
a=source.index('void UpdateGhostReplayLifts(MyCharacter* player,')
b=source.index('bool IsPlatformCustomColourEnabled()',a)
update=source[a:b]
assert 'add(item.actor,player);' in update
assert 'g_remove_based_character(item.actor,player);' in update
assert 'if (item.based && ReplayLiftValid(item,objects))' in reset
mock=r'''
#include <cassert>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include "refunct/ghost_replay_tile_contact.hpp"
namespace refunct {
struct Vec3 {float x,y,z;};
struct MyCharacter {};
struct LevelState {int resets=1;};
struct Actor {ghost_replay_tile_contact::Bounds bounds;};
struct GlobalObjectItem {void* object=nullptr;int serial_number=0;};
struct GlobalObjectArray {int num_elements=0;std::array<GlobalObjectItem,2> objects{};};
struct ReplayLiftContact {void* actor=nullptr;int registry_index=-1,serial=0;bool based=false;ghost_replay_tile_contact::Bounds bounds{};};
struct ReplayLiftRuntime {MyCharacter* player=nullptr;int resets=-1,count=0,contacts_started=0;bool ready=false;std::array<ReplayLiftContact,2> entries{};} s_replay_lifts;
LevelState state{};GlobalObjectArray registry{};
bool g_full_level_reload_requested=false,g_restart_requested=false;
MyCharacter* g_player=nullptr;
void Log(const wchar_t*) {}
bool IsReadable(const void* p,std::size_t){return p!=nullptr;}
int add_calls=0,remove_calls=0;
void add(void*,MyCharacter*){++add_calls;}
void remove(void*,MyCharacter*){++remove_calls;}
using BasedCharacterFn=void(*)(void*,MyCharacter*);
BasedCharacterFn g_original_add_based_character=&add;
BasedCharacterFn g_add_based_character=&add;
BasedCharacterFn g_remove_based_character=&remove;
const LevelState* PracticeLevelState(){return &state;}
const GlobalObjectArray* Objects(){return &registry;}
bool ReplayLiftValid(const ReplayLiftContact& entry,const GlobalObjectArray* objects){
 return entry.actor && objects && entry.registry_index>=0 &&
     entry.registry_index<objects->num_elements &&
     objects->objects[entry.registry_index].object==entry.actor &&
     objects->objects[entry.registry_index].serial_number==entry.serial;
}
bool ReadReplayLiftBounds(void* p,ghost_replay_tile_contact::Bounds& value){value=static_cast<Actor*>(p)->bounds;return true;}
'''+reset+update+r'''
}
int main() {
 using namespace refunct;
 MyCharacter user{};g_player=&user;Actor lift{{{100,0,0},{40,40,10}}};
 registry.num_elements=1;registry.objects[0]={&lift,7};
 s_replay_lifts.player=&user;s_replay_lifts.resets=state.resets;
 s_replay_lifts.ready=true;s_replay_lifts.count=1;
 s_replay_lifts.entries[0]={&lift,0,7,false,lift.bounds};
 UpdateGhostReplayLifts(&user,{0,0,98},{120,0,98},true);
 assert(add_calls==1 && remove_calls==0 && s_replay_lifts.entries[0].based);
 UpdateGhostReplayLifts(&user,{120,0,98},{120,0,98},true);
 assert(add_calls==1 && remove_calls==0); // held contact, not a rapid on/off
 lift.bounds.center.z=20;
 UpdateGhostReplayLifts(&user,{120,0,98},{120,0,118},true);
 assert(add_calls==1 && remove_calls==0); // ghost rides the moving platform
 UpdateGhostReplayLifts(&user,{120,0,118},{250,0,118},true);
 // Swept crossing still intersects; next frame fully off releases.
 UpdateGhostReplayLifts(&user,{250,0,118},{300,0,118},true);
 assert(remove_calls==1 && !s_replay_lifts.entries[0].based);
 UpdateGhostReplayLifts(&user,{300,0,118},{120,0,118},true);
 assert(add_calls==2);
 ResetGhostReplayLifts();
 assert(remove_calls==2); // stop must release owned based contact
 assert(s_replay_lifts.count==0);
 // Old world/restart must not release a stale actor pointer.
 s_replay_lifts.player=&user;s_replay_lifts.resets=1;
 s_replay_lifts.count=1;s_replay_lifts.entries[0]={&lift,0,7,true,lift.bounds};
 state.resets=2;
 ResetGhostReplayLifts();
 assert(remove_calls==2);
 return 0;
}
'''
compiler=shutil.which('g++') or shutil.which('clang++')
assert compiler
with tempfile.TemporaryDirectory(prefix='replay_lifts_') as td:
 src=Path(td)/'main.cpp';out=Path(td)/'test'
 src.write_text(mock,encoding='utf8')
 subprocess.run([compiler,'-std=c++20','-O2','-Wall','-Wextra','-Werror',
                 '-I',str(root/'include'),str(src),'-o',str(out)],check=True,timeout=80)
 subprocess.run([str(out)],check=True,timeout=15)
print('PASS: lift enter/hold/ride/leave, stop cleanup, stale-world safety')

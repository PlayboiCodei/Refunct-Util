"""Compile the actual swept-footprint helper and the native tile contact loop.
Uses a fake 251-tile Refunct LevelState; no shipping binary is executed.
"""
from pathlib import Path
import shutil, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
source=(root/'src/tiles.cpp').read_text(encoding='utf8')
a=source.index('void TouchGhostReplayTiles(MyCharacter* player,')
b=source.index('bool ButtonTileSelectionDisabledPreference()',a)
impl=source[a:b]
assert 'native(actor,player);' in impl
assert 'g_remove_based_character(actor,player);' in impl
assert 'ReadGhostReplayTileBounds(actor,i,current)' in impl
assert 'progress->resets != replay.resets' in impl
mock=r'''
#include <cassert>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include "refunct/ghost_replay_tile_contact.hpp"
namespace refunct {
constexpr int kTotalColourableTiles=251;
struct Vec3 {float x,y,z;};
struct MyCharacter {};
struct LevelState { int platforms=0,resets=1; };
struct Actor {int colour=0;ghost_replay_tile_contact::Bounds bounds{};};
struct TileBatch {
 bool active=false;
 int color_offset=0;
 void* tile_class=reinterpret_cast<void*>(1);
 std::array<void*,kTotalColourableTiles> actors{};
} s_tiles;
struct GhostReplayTileCache {
 MyCharacter* player=nullptr; int resets=-1;
 bool unavailable=false,contact_issued=false;int collected=0,contact_count=0;
 TileBatch layout{};
 std::array<ghost_replay_tile_contact::Bounds,kTotalColourableTiles> regions{};
 std::array<bool,kTotalColourableTiles> attempted{};
 std::array<uint8_t,kTotalColourableTiles> contact_attempts{};
 std::array<uint64_t,kTotalColourableTiles> retry_after{};
} s_ghost_replay_tiles;
LevelState state{};
uint64_t GetTickCount64(){return 1000;}
int added=0,removed=0;
void add(void* ptr,MyCharacter*) {++added;auto& a=*static_cast<Actor*>(ptr);if(!a.colour){a.colour=1;++state.platforms;}}
void remove(void*,MyCharacter*) {++removed;}
using BasedCharacterFn=void(*)(void*,MyCharacter*);
BasedCharacterFn g_original_add_based_character=&add;
BasedCharacterFn g_add_based_character=&add;
BasedCharacterFn g_remove_based_character=&remove;
const LevelState* PracticeLevelState(){return &state;}
bool ReadTileColor(void* ptr,int,int32_t& result,void*) {result=static_cast<Actor*>(ptr)->colour;return true;}
void Log(const wchar_t*){}
bool ReadGhostReplayTileBounds(void* ptr,int,ghost_replay_tile_contact::Bounds& box){box=static_cast<Actor*>(ptr)->bounds;return true;}
void NotifyNativeTileContactCompleted(void*,MyCharacter*){}
'''+impl+r'''
}
int main() {
 using namespace refunct;
 using namespace refunct::ghost_replay_tile_contact;
 Bounds tile{{100,0,0},{20,20,10}};
 assert(Valid(tile));
 Bounds transformed{};
 assert(TransformMeshBounds({-10,-20,-5},{10,20,5},{0,90,0},{2,3,1},transformed));
 assert(std::abs(transformed.extent.x-60)<0.01f);
 assert(std::abs(transformed.extent.y-20)<0.01f);
 assert(std::abs(transformed.extent.z-5)<0.01f);
 assert(TransformMeshBounds({0,0,0},{10,20,5},{0,0,0},{-2,1,1},transformed));
 assert(transformed.center.x==-10 && transformed.extent.x==10);
 assert(!TransformMeshBounds({0,0,0},{10,20,5},{0,0,0},{0,1,1},transformed));
 assert(Crossed({0,0,98},{200,0,98},tile));
 assert(!Crossed({0,0,400},{200,0,400},tile));
 assert(!Crossed({0,300,98},{200,300,98},tile));
 assert(!Crossed({-5000,0,98},{5000,0,98},tile)); // teleport ignored
 assert(!Crossed({0,0,98},{0,0,98},tile));
 assert(CrossedXY({0,0,400},{200,0,400},tile));
 assert(!Valid({{0,0,0},{900,10,10}}));
 MyCharacter user{};
 Actor actors[kTotalColourableTiles]{};
 for(int i=0;i<kTotalColourableTiles;++i) {
   actors[i].bounds={{float(100+i*1000),0,0},{20,20,10}};
   s_ghost_replay_tiles.layout.actors[i]=&actors[i];
   s_ghost_replay_tiles.regions[i]=actors[i].bounds;
 }
 s_ghost_replay_tiles.player=&user;
 s_ghost_replay_tiles.resets=state.resets;
 s_ghost_replay_tiles.collected=kTotalColourableTiles;
 TouchGhostReplayTiles(&user,{0,0,98},{200,0,98},true);
 assert(added==1 && removed==1 && actors[0].colour==1 && state.platforms==1);
 assert(s_ghost_replay_tiles.contact_issued);
 TouchGhostReplayTiles(&user,{0,0,98},{200,0,98},true);
 assert(added==1); // never touch same tile twice
 // Same XY but underwater: cannot touch until the game lifts the actual tile.
 actors[1].bounds.center.z=-500;
 TouchGhostReplayTiles(&user,{1050,0,98},{1150,0,98},true);
 assert(added==1 && actors[1].colour==0);
 actors[1].bounds.center.z=0;
 TouchGhostReplayTiles(&user,{1050,0,98},{1150,0,98},true);
 assert(added==2 && actors[1].colour==1);
 // Stationary contact also selects the tile. A seek does not sweep the path.
 TouchGhostReplayTiles(&user,{2050,0,98},{3100,0,98},false);
 assert(added==3 && actors[3].colour==1 && actors[2].colour==0);
 // Other run/reset rejects the cached actor pointers.
 ++state.resets;
 TouchGhostReplayTiles(&user,{2050,0,98},{2150,0,98},true);
 assert(added==3);
 return 0;
}
'''
compiler=shutil.which('g++') or shutil.which('clang++')
assert compiler
with tempfile.TemporaryDirectory(prefix='replay_tiles_') as td:
 src=Path(td)/'main.cpp';out=Path(td)/'test'
 src.write_text(mock,encoding='utf8')
 subprocess.run([compiler,'-std=c++20','-O2','-Wall','-Wextra','-Werror',
                 '-I',str(root/'include'),str(src),'-o',str(out)],check=True,timeout=80)
 subprocess.run([str(out)],check=True,timeout=15)
print('PASS: native tile contacts, current raised height, footprint sweep, no duplicate, reset safety')

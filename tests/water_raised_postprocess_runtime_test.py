"""Compile the real raised-water PostWater override against mock UE4 state.

Checks strict native bitfield metadata validation, camera-level activation,
restoration, invalidation and preservation of neighbouring packed flags.
No Windows x86 build or in-game result is implied.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
water = (root / 'src/water.cpp').read_text(encoding='utf-8')


def function(signature: str) -> str:
    pos = water.index(signature)
    while True:
        brace = water.index('{', pos)
        semi = water.find(';', pos, brace)
        if semi < 0:
            begin = pos
            break
        pos = water.index(signature, semi + 1)
    level = 0
    for i in range(brace, len(water)):
        level += (water[i] == '{') - (water[i] == '}')
        if level == 0:
            return water[begin:i + 1]
    raise AssertionError(signature)

signatures = (
    'bool UnderwaterResolveRaisedFlags(',
    'uint8_t* UnderwaterRaisedFlagAddress(',
    'void UnderwaterRestoreRaisedEffect(',
    'bool UnderwaterCameraInRaisedLayer(',
    'void UnderwaterUpdateRaisedEffect(',
)
camera_helpers = ('bool RaisedWaterCameraOwnedBy(',
                  'bool RaisedWaterFindCamera(',
                  'bool RaisedWaterRenderedViewLocation(')
actual = '\n'.join(function(sig) for sig in camera_helpers + signatures)
actual = water[water.index('struct RaisedWaterCameraTarget {'):water.index('bool RaisedWaterCameraOwnedBy(')] + water[water.index('struct RaisedWaterViewTarget {'):water.index('bool RaisedWaterRenderedViewLocation(')] + actual
assert 'FixedWaterlineIsLive(player, objects)' in water
assert 'RaisedWaterRestoreVirtualVolume(player, objects, native_waterline_owned);' in water
assert 'UnderwaterUpdateRaisedEffect(player, objects);' in water
assert 'raised_effect_needed' in function('void UpdateUnderwaterColours(')
assert 'UnderwaterRestoreRaisedEffect(WaterObjects());' in water

harness = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <iostream>
#include <string>
#include <vector>
using byte=uint8_t;
struct Vec3{float x=0,y=0,z=0;};
struct UObjectHeader{uint32_t flags=0;int32_t index=0;void* class_ptr=nullptr;void* outer=nullptr;};
struct CameraComponent{UObjectHeader object{};Vec3 location{};};
struct PlayerController{UObjectHeader object{};};
struct MyCharacter{UObjectHeader object{};PlayerController* controller=nullptr;};
struct GlobalObjectItem{UObjectHeader* object=nullptr;int32_t serial_number=0;};
struct GlobalObjectArray{GlobalObjectItem* objects=nullptr;int32_t num_elements=0;};
struct UnderwaterBoolField{const uint8_t* descriptor=nullptr;int32_t property_offset=-1;
 bool name_found=false;bool type_correct=false;};
struct UnderwaterBoolBits{int32_t address_offset=-1;uint8_t mask=0,field_size=0;};
struct UnderwaterTintTarget {
 void* actor=nullptr;int32_t index=-1,serial=0;
 int32_t unbound_offset=-1;uint8_t unbound_mask=0;
 int32_t enabled_offset=-1;uint8_t enabled_mask=0;
 bool original_unbound=false,original_enabled=false,raised_effect_owned=false;
};
UnderwaterTintTarget s_underwater_target{};
bool s_raised_postprocess_warning_logged=false,s_raised_postprocess_logged=false;
bool g_flying=false,g_noclip=false,g_restart_requested=false,g_full_level_reload_requested=false;
struct Config{int height_offset=400;};
Config settings{};
struct Actor{UObjectHeader object;uint8_t water_flags=0x80;};
Actor post{};
GlobalObjectItem items[3]{};
GlobalObjectArray objects{items,3};
CameraComponent camera{};
PlayerController controller{};
struct CameraManager{UObjectHeader object{};Vec3 location{};};
CameraManager manager{};
MyCharacter player{};
bool no_swimming=false,no_tint=false,reflected=true,has_camera=true,has_manager=false;
using ProcessEventFn = void(*)(void*,void*,void*);
void CameraProcessEvent(void* obj,void*,void* args){
 if(obj==&manager) std::memcpy(args,&manager.location,sizeof(manager.location));
}
ProcessEventFn g_process_event = CameraProcessEvent;
void* FindReflectedFunction(void* obj,const wchar_t* name){
 return obj==&manager && std::wcscmp(name,L"GetCameraLocation")==0 ? obj : nullptr;
}
int WaterParam(void* fn,const wchar_t* name,const wchar_t* kind){
 return fn==&manager && std::wcscmp(name,L"ReturnValue")==0 &&
   std::wcscmp(kind,L"StructProperty")==0 ? 0 : -1;
}
uint64_t fake_tick=0;
using ULONGLONG=unsigned long long;
ULONGLONG GetTickCount64(){return fake_tick;}
std::wstring WaterObjectName(void* candidate) {
 if(candidate==&camera) return L"FirstPersonCamera";
 return L"other";
}
int logs=0;
Config& WaterConfig(){return settings;}
bool WaterNoSwimming(){return no_swimming;}
bool WaterNoTint(){return no_tint;}
bool UnderwaterTargetValid(const GlobalObjectArray* ptr){
 return ptr && s_underwater_target.actor==&post &&
 s_underwater_target.index==0 && s_underwater_target.serial==ptr->objects[0].serial_number &&
 ptr->objects[0].object==&post.object;
}
UnderwaterBoolField UnderwaterFindBoolField(void*,const wchar_t* name){
 const bool valid=std::wcscmp(name,L"bUnbound")==0 || std::wcscmp(name,L"bEnabled")==0;
 return {reinterpret_cast<const uint8_t*>(name),int32_t(offsetof(Actor,water_flags)),valid,valid&&reflected};
}
bool UnderwaterReadBoolBits(const UnderwaterBoolField& field,size_t layout,UnderwaterBoolBits& out){
 if(!field.type_correct || layout!=0x50) return false;
 const wchar_t* name=reinterpret_cast<const wchar_t*>(field.descriptor);
 out={field.property_offset, uint8_t(std::wcscmp(name,L"bEnabled")==0?1:2),1};
 return true;
}
bool UnderwaterAdjacentBoolBits(const UnderwaterBoolBits& lhs,const UnderwaterBoolBits& rhs){
 return lhs.address_offset==rhs.address_offset &&
 ((lhs.mask==1&&rhs.mask==2)||(lhs.mask==2&&rhs.mask==1));
}
bool IsWritable(const void* ptr,size_t){return ptr!=nullptr;}
bool IsReadable(const void* ptr,size_t){return ptr!=nullptr;}
void Log(const wchar_t*){++logs;}
bool RaisedWaterOriginalSurface(const GlobalObjectArray*,float& z){z=-64;return true;}
void* ReadReflectedObject(void* obj,const wchar_t* name){
 if(obj==&controller && std::wcscmp(name,L"PlayerCameraManager")==0)
   return has_manager?&manager:nullptr;
 return has_camera?&camera:nullptr;
}
bool WaterIsA(void* obj,const wchar_t* name){
 return (obj==&camera&&std::wcscmp(name,L"CameraComponent")==0) ||
        (obj==&manager&&std::wcscmp(name,L"PlayerCameraManager")==0);
}
bool WaterGetLocation(void*,Vec3& vec){vec=camera.location;return true;}
''' + '\n' + actual + r'''
int main(){
 auto check=[](bool state,const char* msg){if(!state){std::cerr<<msg<<'\n';std::exit(1);}};
 items[0]={&post.object,17};post.object.class_ptr=&post;
 items[1]={&camera.object,19};camera.object.index=1;camera.object.outer=&player;
 items[2]={&manager.object,29};manager.object.index=2;
 player.object.index=22;player.controller=&controller;
 s_underwater_target.actor=&post;s_underwater_target.index=0;s_underwater_target.serial=17;
 camera.location.z=150;
 UnderwaterUpdateRaisedEffect(&player,&objects);
 check(s_underwater_target.raised_effect_owned,"raised effect not owned");
 check(post.water_flags==0x83,"PostWater bitflags not enabled safely");
 check(logs==1,"activation not logged once");
 // No reflected BP camera property: use validated player-owned component.
 camera.location.z=345;
 UnderwaterUpdateRaisedEffect(&player,&objects);
 has_camera=false;fake_tick=6000;
 UnderwaterUpdateRaisedEffect(&player,&objects);
 check(!s_underwater_target.raised_effect_owned,"above-water flag not restored");
 camera.location.z=150;
 UnderwaterUpdateRaisedEffect(&player,&objects);
 check(s_underwater_target.raised_effect_owned,"owned camera scan failed");
 has_camera=true;
 camera.location.z=345; // Above top - hysteresis grace.
 UnderwaterUpdateRaisedEffect(&player,&objects);
 check(!s_underwater_target.raised_effect_owned,"effect not restored above water");
 check(post.water_flags==0x80,"packed flags not restored");
 camera.location.z=150;
 UnderwaterUpdateRaisedEffect(&player,&objects);
 check(s_underwater_target.raised_effect_owned,"re-entry not working");
 no_swimming=true;
 UnderwaterUpdateRaisedEffect(&player,&objects);
 check(post.water_flags==0x80 && !s_underwater_target.raised_effect_owned,
       "remove swimming should restore postprocess");
 no_swimming=false;
 camera.location.z=150;
 UnderwaterUpdateRaisedEffect(&player,&objects);
 no_tint=true;
 UnderwaterUpdateRaisedEffect(&player,&objects);
 check(post.water_flags==0x80 && !s_underwater_target.raised_effect_owned,
       "remove tint should restore PostWater flags");
 no_tint=false;
 reflected=false;
 s_underwater_target.unbound_offset=-1;
 s_underwater_target.enabled_offset=-1;
 s_underwater_target.unbound_mask=0;
 s_underwater_target.enabled_mask=0;
 UnderwaterUpdateRaisedEffect(&player,&objects);
 check(post.water_flags==0x80 && !s_underwater_target.raised_effect_owned,
       "unsafe reflection wrote flags");
 reflected=true;
 UnderwaterUpdateRaisedEffect(&player,&objects);
 check(s_underwater_target.raised_effect_owned,"re-enable after valid reflection failed");
 ++items[0].serial_number; // Native actor destroyed; don't write reused object.
 const auto flags_before=post.water_flags;
 UnderwaterUpdateRaisedEffect(&player,&objects);
 check(post.water_flags==flags_before,"stale actor was modified");
 check(!s_underwater_target.raised_effect_owned,"stale reference not dropped");
 items[0].serial_number=17;
 // Original PostWater was already unbound; preserve its original bit on exit.
 post.water_flags=0x82;
 camera.location.z=150;
 UnderwaterUpdateRaisedEffect(&player,&objects);
 check(post.water_flags==0x83,"originally unbound PostWater not enabled");
 camera.location.z=345;
 UnderwaterUpdateRaisedEffect(&player,&objects);
 check(post.water_flags==0x82,"preexisting flag lost on restore");
 // CameraManager is a better source than component ownership. This also
 // covers a rendered camera with no reflected pawn component/property.
 has_camera=false;
 has_manager=true;
 fake_tick=22000;
 manager.location.z=150;
 UnderwaterUpdateRaisedEffect(&player,&objects);
 check(s_underwater_target.raised_effect_owned,"rendered camera manager failed to activate tint");
 manager.location.z=345;
 UnderwaterUpdateRaisedEffect(&player,&objects);
 check(!s_underwater_target.raised_effect_owned && post.water_flags==0x82,
       "camera manager exit failed to restore preexisting bitflags");
 manager.location.z=150;
 UnderwaterUpdateRaisedEffect(&player,&objects);
 check(s_underwater_target.raised_effect_owned,"camera manager re-entry failed");
 ++items[2].serial_number;
 UnderwaterUpdateRaisedEffect(&player,&objects);
 check(!s_underwater_target.raised_effect_owned && post.water_flags==0x82,
       "stale camera manager unexpectedly kept PostWater active");
 check(UnderwaterCameraInRaisedLayer(323,-64,336,false),"inside range rejected");
 check(!UnderwaterCameraInRaisedLayer(330,-64,336,false),"entry hysteresis missing");
 check(UnderwaterCameraInRaisedLayer(338,-64,336,true),"exit hysteresis missing");
 std::cout<<"PASS: raised PostWater camera activation and safe restoration\n";
}
'''
compiler = shutil.which('g++') or shutil.which('clang++')
if not compiler:
    raise SystemExit('Missing portable C++ compiler')
with tempfile.TemporaryDirectory(prefix='waterpp_') as td:
    source = Path(td) / 'test.cpp'
    source.write_text(harness, encoding='utf-8')
    exe = Path(td) / 'test'
    built = subprocess.run([compiler,'-std=c++20','-Wall','-Wextra','-Werror',
                            '-O0',str(source),'-o',str(exe)],capture_output=True,text=True,timeout=30)
    assert built.returncode == 0, built.stderr
    ran = subprocess.run([str(exe)],capture_output=True,text=True,timeout=10)
    assert ran.returncode == 0, ran.stdout + ran.stderr
    print(ran.stdout.strip())

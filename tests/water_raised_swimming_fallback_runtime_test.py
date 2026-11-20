"""Compile the actual raised-water fallback against simulated UE4 movement/volume objects.

Tests native water flag ownership, collision-mode stability, feature priority,
original waterline, removal, and stale global UObject references.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'src/water.cpp').read_text(encoding='utf-8')


def function(signature: str) -> str:
    # A function may have a forward declaration before its definition.
    pos = source.index(signature)
    while True:
        brace = source.index('{', pos)
        semicolon = source.find(';', pos, brace)
        if semicolon < 0:
            definition_start = pos
            pos = brace
            break
        pos = source.index(signature, semicolon + 1)
    depth = 0
    for i in range(pos, len(source)):
        depth += (source[i] == '{') - (source[i] == '}')
        if depth == 0:
            return source[source.rfind('\n', 0, definition_start) + 1:i + 1]
    raise AssertionError('Unclosed function ' + signature)


signatures = (
    'bool RaisedWaterSetMovementMode(',
    'void RaisedWaterRestoreVirtualVolume(',
    'void* RaisedWaterDefaultVolume(',
    'bool RaisedWaterOriginalSurface(',
    'void UpdateRaisedWaterFallback(',
)
implementation = '\n\n'.join(function(s) for s in signatures)
assert 'UpdateRaisedWaterFallback(player);' in source
assert 'WaterNoSwimming()' in implementation
assert 'DefaultPhysicsVolume' in implementation
assert 's_height_volume_warning_logged' in implementation
assert 's_height_volume_success_logged' in implementation
assert 'void UpdateWaterSwimming(MyCharacter* player)' in source

mock = r'''
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
using byte = uint8_t;
struct Vec3 {float x,y,z;};
struct UObjectHeader { uint32_t flags=0; int32_t index=0; void* class_ptr=nullptr; void* outer=nullptr; };
struct SceneComponent {Vec3 location{};};
struct MovementComponent {uint8_t movement_mode=3;};
struct MyCharacter {UObjectHeader header{}; SceneComponent* root_component=nullptr; MovementComponent* movement=nullptr;};
struct GlobalObjectItem {UObjectHeader* object=nullptr; int32_t serial_number=0;};
struct GlobalObjectArray {GlobalObjectItem* objects=nullptr; int32_t num_elements=0;};
struct SwimVolumeTarget {void* actor=nullptr; int32_t index=-1; int32_t serial=0; int32_t flag_offset=-1; uint8_t flag_mask=0;};
struct WaterTarget {void* actor=nullptr; int32_t index=-1; int32_t serial=0; bool original_location_known=false; bool height_modified=false; Vec3 original_location{};};
struct WaterSettings {int height_offset=500;};
struct RaisedWaterVirtualVolume { SwimVolumeTarget volume{}; bool active=false; bool owns_swimming_mode=false; };
RaisedWaterVirtualVolume s_raised_water{};
SwimVolumeTarget s_raised_default_hint{};
int32_t s_raised_default_scan_cursor=0;
bool s_raised_prearm_logged=false;
bool s_raised_prearm_lookup_warned=false;
bool s_raised_water_logged=false;
bool s_raised_water_warning_logged=false;
bool s_height_volume_warning_logged=true;
bool s_height_volume_success_logged=false;
bool g_flying=false, g_noclip=false, g_restart_requested=false, g_full_level_reload_requested=false;
std::vector<WaterTarget> s_targets;
WaterSettings g_settings{};
GlobalObjectItem items[3]{};
GlobalObjectArray objects{items,3};
struct NativeFixedWaterline {bool modified=false;};
NativeFixedWaterline s_fixed_waterline{};
bool FixedWaterlineIsLive(MyCharacter*,const GlobalObjectArray*) {return true;}
struct MockVolume {UObjectHeader object; uint8_t flag=0xC0; bool is_default=false;};
MockVolume atmosphere{}, water{};
SceneComponent root{};
MovementComponent move{};
MyCharacter character{{},&root,&move};
void* active_volume = &atmosphere;
bool remove_swimming=false;
int mode_calls=0;
std::vector<std::wstring> logs;
using ULONGLONG = unsigned long long;
ULONGLONG GetTickCount64(){return 0;}
std::wstring WaterObjectName(void* actor){return actor==&atmosphere ? L"DefaultPhysicsVolume_0" : L"WaterVolume_1";}
using ProcessEvent = void(*)(void*,void*,void*);
void ProcessEventMock(void*,void* fn,void* args) {
    if(fn==reinterpret_cast<void*>(1)) std::memcpy(args,&active_volume,sizeof(active_volume));
    else if(fn==reinterpret_cast<void*>(2)) {move.movement_mode=static_cast<uint8_t*>(args)[0];++mode_calls;}
}
ProcessEvent g_process_event=ProcessEventMock;
void* FindReflectedFunction(void*,const wchar_t* name) {
    if(std::wcscmp(name,L"GetPhysicsVolume")==0) return reinterpret_cast<void*>(1);
    if(std::wcscmp(name,L"SetMovementMode")==0) return reinterpret_cast<void*>(2);
    return nullptr;
}
int WaterParam(void* fn, const wchar_t*, const wchar_t*) {return fn ? 0:-1;}
bool IsReadable(const void* ptr, size_t) {return ptr!=nullptr;}
int registry_reads=0;
const GlobalObjectArray* WaterObjects() {++registry_reads;return &objects;}
WaterSettings& WaterConfig() {return g_settings;}
bool WaterNoSwimming() {return remove_swimming;}
void Log(const wchar_t* msg) {logs.emplace_back(msg);}
bool WaterIsA(void* actor, const wchar_t* class_name) {
    if (std::wcscmp(class_name,L"DefaultPhysicsVolume")==0)
        return actor==&atmosphere && atmosphere.is_default;
    if (std::wcscmp(class_name,L"PhysicsVolume")==0)
        return actor==&atmosphere || actor==&water;
    return false;
}
bool WaterIsTargetValid(const WaterTarget& t,const GlobalObjectArray* a) {
    return t.actor && t.index>=0 && t.index<a->num_elements &&
        a->objects[t.index].object==t.actor &&
        a->objects[t.index].serial_number==t.serial;
}
bool SwimVolumeValid(const SwimVolumeTarget& v,const GlobalObjectArray* a) {
    return v.actor && v.index>=0 && v.index<a->num_elements &&
        a->objects[v.index].object==v.actor &&
        a->objects[v.index].serial_number==v.serial;
}
uint8_t* SwimFlagAddress(const SwimVolumeTarget& v,const GlobalObjectArray* a) {
    return SwimVolumeValid(v,a) && v.flag_offset==int(offsetof(MockVolume,flag)) ?
           &static_cast<MockVolume*>(v.actor)->flag : nullptr;
}
bool SwimResolveWaterFlag(void* actor,int32_t& offset,uint8_t& mask) {
    if (actor!=&atmosphere) return false;
    offset=int(offsetof(MockVolume,flag)); mask=0x02;return true;
}
void* SwimCurrentPhysicsVolume(MyCharacter*) {return active_volume;}
'''

main = r'''
int main() {
    atmosphere.object.index=0; atmosphere.is_default=true;
    water.object.index=1;
    character.header.outer=reinterpret_cast<void*>(0x99);
    atmosphere.object.outer=character.header.outer;
    water.object.outer=character.header.outer;
    items[0]={&atmosphere.object,33};items[1]={&water.object,51};
    s_targets.push_back(WaterTarget{&atmosphere,0,33,true,true,{0,0,0}});
    root.location={0,0,250};
    g_settings.height_offset=0;
    for(int i=0;i<480;++i) UpdateRaisedWaterFallback(&character);
    assert(registry_reads==0);
    g_settings.height_offset=500;
    s_height_volume_success_logged=true;
    UpdateRaisedWaterFallback(&character);
    assert(registry_reads==0);
    s_height_volume_success_logged=false;
    UpdateRaisedWaterFallback(&character);
    assert(s_raised_water.active && move.movement_mode==4 &&
           atmosphere.flag==0xC2 && mode_calls==1);
    // FixedWaterHeight owns the surface boundary, but PhysSwimming still
    // needs the native water flag. Retain the flag without fighting native
    // movement-mode transitions every frame.
    s_fixed_waterline.modified=true;
    move.movement_mode=3;
    const int mode_calls_before=mode_calls;
    UpdateRaisedWaterFallback(&character);
    assert(s_raised_water.active && atmosphere.flag==0xC2 &&
           move.movement_mode==3 && mode_calls==mode_calls_before);
    // Leaving the raised surface restores only the flag; never force falling
    // when the Refunct waterline may still be processing the transition.
    root.location.z=501;
    UpdateRaisedWaterFallback(&character);
    assert(!s_raised_water.active && atmosphere.flag==0xC0 &&
           move.movement_mode==3 && mode_calls==mode_calls_before);
    s_fixed_waterline.modified=false;
    root.location.z=250;
    UpdateRaisedWaterFallback(&character);
    assert(s_raised_water.active && move.movement_mode==4);
    // Collision cannot make the default active volume cease to be water;
    // one recovery of movement mode still preserves the volume water flag.
    move.movement_mode=3;
    atmosphere.flag=0xC0; // Engine unexpectedly cleared the temporary water bit.
    UpdateRaisedWaterFallback(&character);
    assert(move.movement_mode==4 && atmosphere.flag==0xC2);
    // Dry air above the raised surface must restore only our one flag.
    root.location.z=550;
    UpdateRaisedWaterFallback(&character);
    assert(!s_raised_water.active && atmosphere.flag==0xC0 && move.movement_mode==3);
    // Remove Swimming takes precedence and restores the overridden bit.
    root.location.z=250;
    UpdateRaisedWaterFallback(&character);
    assert(s_raised_water.active);
    remove_swimming=true;
    UpdateRaisedWaterFallback(&character);
    assert(!s_raised_water.active && atmosphere.flag==0xC0);
    remove_swimming=false;
    // Disabled for lower water (cannot physically shrink a static brush).
    g_settings.height_offset=-100;
    UpdateRaisedWaterFallback(&character);
    assert(!s_raised_water.active);
    g_settings.height_offset=500;
    // Arm the destination before the native volume -> default volume handoff.
    // Simulated original surface is Z=0 and the native volume ends there.
    active_volume=&water;
    root.location.z=-50;
    move.movement_mode=4;
    const int before_prearm=mode_calls;
    UpdateRaisedWaterFallback(&character);
    assert(s_raised_water.active && atmosphere.flag==0xC2 &&
           water.flag==0xC0 && mode_calls==before_prearm &&
           s_raised_prearm_logged);
    // Changing the active native volume must NOT produce a dry-air transition:
    // destination already carries bWaterVolume before physics changes volume.
    active_volume=&atmosphere;
    root.location.z=0;
    UpdateRaisedWaterFallback(&character);
    assert(s_raised_water.active && atmosphere.flag==0xC2 &&
           move.movement_mode==4);
    root.location.z=250;
    UpdateRaisedWaterFallback(&character);
    assert(s_raised_water.active && atmosphere.flag==0xC2);
    // Below the old boundary, restore only our bit, preserving native water.
    active_volume=&water;
    root.location.z=-180;
    UpdateRaisedWaterFallback(&character);
    assert(!s_raised_water.active && atmosphere.flag==0xC0 &&
           water.flag==0xC0 && move.movement_mode==4);
    // No cached destination: discover it by verified same-level UObject identity.
    s_raised_default_hint = {};
    s_raised_default_scan_cursor = 0;
    active_volume=&water;
    root.location.z=-60;
    UpdateRaisedWaterFallback(&character);
    assert(s_raised_water.active && atmosphere.flag==0xC2 &&
           move.movement_mode==4);
    // Stale/default actors from another level must not be adopted.
    root.location.z=-180;
    UpdateRaisedWaterFallback(&character);
    assert(!s_raised_water.active && atmosphere.flag==0xC0);
    s_raised_default_hint = {};
    s_raised_default_scan_cursor = 0;
    atmosphere.object.outer = reinterpret_cast<void*>(0x55);
    root.location.z=-60;
    UpdateRaisedWaterFallback(&character);
    assert(!s_raised_water.active && atmosphere.flag==0xC0);
    atmosphere.object.outer = character.header.outer;
    active_volume=&atmosphere;
    root.location.z=250;
    // An unchanged default volume with no validated UObject identity is ignored.
    ++items[0].serial_number;
    UpdateRaisedWaterFallback(&character);
    assert(!s_raised_water.active && atmosphere.flag==0xC0);
    items[0].serial_number=33;
    UpdateRaisedWaterFallback(&character);
    assert(s_raised_water.active);
    // On reload, stale handles must be discarded without writing reused memory.
    ++items[0].serial_number;
    const uint8_t flag_before=atmosphere.flag;
    g_settings.height_offset=0;
    UpdateRaisedWaterFallback(&character);
    assert(!s_raised_water.active && atmosphere.flag==flag_before);
    std::cout<<"PASS raised-water default-volume fallback, collision, restoration, and stale-handle checks\n";
}
'''
compiler = shutil.which('g++') or shutil.which('clang++')
assert compiler, 'C++ compiler needed for simulation'
with tempfile.TemporaryDirectory(prefix='waterheight_') as temp:
    src = Path(temp) / 'test.cpp'
    binary = Path(temp) / 'test'
    src.write_text(mock + '\n' + implementation + '\n' + main, encoding='utf-8')
    compile_result = subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra',
                                     '-Werror', '-O2', str(src), '-o', str(binary)],
                                    capture_output=True, text=True)
    assert compile_result.returncode == 0, compile_result.stderr
    run = subprocess.run([str(binary)], capture_output=True, text=True)
    assert run.returncode == 0, run.stderr + run.stdout
    print(run.stdout.strip())

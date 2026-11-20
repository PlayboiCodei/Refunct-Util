"""Compile the native World->Water disable-swimming code with fake UE4 objects.

This checks volume-bit preservation, actor identity, collision transitions,
and fail-closed metadata behaviour. It is NOT an in-game verification.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'src/water.cpp').read_text(encoding='utf-8')


def extract_function(signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth = 0
    for i in range(brace, len(source)):
        if source[i] == '{':
            depth += 1
        elif source[i] == '}':
            depth -= 1
            if depth == 0:
                return source[start:i + 1]
    raise AssertionError('Unclosed function: ' + signature)


functions = [
    'bool SwimResolveWaterFlag(void* actor, int32_t& offset, uint8_t& mask)',
    'bool SwimVolumeValid(const SwimVolumeTarget& target,',
    'uint8_t* SwimFlagAddress(const SwimVolumeTarget& target,',
    'void SwimRestoreVolumes()',
    'bool SwimDisableVolume(void* actor, const GlobalObjectArray* objects)',
    'void* SwimCurrentPhysicsVolume(MyCharacter* player)',
    'void SwimScanVolumes(const GlobalObjectArray* objects)',
    'void UpdateWaterSwimming(MyCharacter* player)',
]
for name in functions:
    assert source.count(name) == 1, name

mock = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>
#include <unordered_map>
#include <vector>
#include <cstddef>
using ULONGLONG = unsigned long long;
constexpr size_t kScanBatch = 2048;
constexpr ULONGLONG kScanCpuBudgetMs = 2;
struct UObjectHeader { int flags = 0; int index = -1; void* class_ptr = nullptr; };
struct MovementComponent : UObjectHeader { uint8_t movement_mode = 4; };
struct SceneComponent : UObjectHeader {};
struct MyCharacter { SceneComponent* root_component = nullptr; MovementComponent* movement = nullptr; };
struct PhysicsVolume { UObjectHeader header; uint8_t volume_flags = 3; };
struct GlobalObjectItem { UObjectHeader* object = nullptr; int serial_number = 0; };
struct GlobalObjectArray { GlobalObjectItem* objects = nullptr; int num_elements = 0; };
struct SwimVolumeTarget {
    void* actor = nullptr;
    int32_t index = -1;
    int32_t serial = 0;
    int32_t flag_offset = -1;
    uint8_t flag_mask = 0;
};
std::vector<SwimVolumeTarget> s_swim_volumes;
MyCharacter* s_swim_player = nullptr;
int32_t s_swim_scan_cursor = -1;
ULONGLONG s_swim_next_scan = 0;
bool s_swim_resolution_warned = false;
constexpr size_t kMaxSwimVolumes = 64;
struct UnderwaterBoolField { bool type_correct = true; bool is_water = false; };
struct UnderwaterBoolBits { int32_t address_offset = -1; uint8_t mask = 0; uint8_t field_size = 4; };
int g_bad_metadata = 0;
UnderwaterBoolField UnderwaterFindBoolField(void*, const wchar_t* name) {
    return {g_bad_metadata == 0, wcscmp(name, L"bWaterVolume") == 0};
}
bool UnderwaterReadBoolBits(const UnderwaterBoolField& field, size_t offset, UnderwaterBoolBits& out) {
    if (offset != 0x50) return false;
    out = {static_cast<int32_t>(offsetof(PhysicsVolume, volume_flags)),
           static_cast<uint8_t>(field.is_water ? 0x01 : 0x02), 4};
    return true;
}
bool UnderwaterAdjacentBoolBits(const UnderwaterBoolBits& left, const UnderwaterBoolBits& right) {
    return left.address_offset == right.address_offset && left.mask == 1 && right.mask == 2;
}
ULONGLONG s_now = 10000;
ULONGLONG GetTickCount64() { return s_now; }
void* kPhysicsClass = reinterpret_cast<void*>(0x1);
GlobalObjectItem s_items[2]{};
GlobalObjectArray s_object_array{s_items, 2};
bool g_objects_available = true;
const GlobalObjectArray* WaterObjects() { return g_objects_available ? &s_object_array : nullptr; }
bool IsReadable(const void* p, size_t) { return p != nullptr; }
bool IsWritable(const void* p, size_t) { return p != nullptr; }
bool WaterIsA(void* object, const wchar_t* name) {
    return static_cast<UObjectHeader*>(object)->class_ptr == kPhysicsClass &&
           wcscmp(name, L"PhysicsVolume") == 0;
}
int warning_count = 0;
void Log(const wchar_t*) { ++warning_count; }
void Notify(const std::wstring&) {}
bool g_restart_requested = false;
bool g_full_level_reload_requested = false;
bool g_flying = false;
struct WaterSettings { bool swimming_disabled = false; bool remove_all = false; int height_offset = 0; };
struct FixedWaterline { MyCharacter* player = nullptr; } s_fixed_waterline;
struct RaisedWater { bool active = false; } s_raised_water;
int fixed_calls=0, fallback_calls=0;
WaterSettings s_config{};
WaterSettings& WaterConfig() { return s_config; }
bool WaterNoSwimming() {return s_config.remove_all || s_config.swimming_disabled;}
PhysicsVolume* g_volume = nullptr;
void* FindReflectedFunction(void*, const wchar_t* method) {
    if (wcscmp(method,L"GetPhysicsVolume") == 0) return reinterpret_cast<void*>(0x11);
    if (wcscmp(method,L"SetMovementMode") == 0) return reinterpret_cast<void*>(0x22);
    return nullptr;
}
int32_t WaterParam(void* fn,const wchar_t* property,const wchar_t* kind) {
    if (fn == reinterpret_cast<void*>(0x11) &&
        wcscmp(property,L"ReturnValue") == 0 &&
        wcscmp(kind,L"ObjectProperty") == 0) return 0;
    if (fn == reinterpret_cast<void*>(0x22) &&
        wcscmp(property,L"NewMovementMode") == 0 &&
        wcscmp(kind,L"ByteProperty") == 0) return 0;
    return -1;
}
void NativeProcessEvent(void* object,void* fn,void* args) {
    if (fn == reinterpret_cast<void*>(0x11))
        std::memcpy(args, &g_volume, sizeof(g_volume));
    else if (fn == reinterpret_cast<void*>(0x22))
        static_cast<MovementComponent*>(object)->movement_mode = static_cast<uint8_t*>(args)[0];
}
auto g_process_event = &NativeProcessEvent;
void UpdateNativeFixedWaterline(MyCharacter*) {++fixed_calls;} // tested independently
void UpdateRaisedWaterFallback(MyCharacter*) {++fallback_calls;} // verified by dedicated fallback test
'''

test = r'''
int main() {
    PhysicsVolume volume{};
    volume.header.class_ptr = kPhysicsClass;
    volume.header.index = 0;
    s_items[0] = {&volume.header, 1};
    MovementComponent movement{};
    SceneComponent component{};
    MyCharacter player{&component,&movement};
    g_volume = &volume;
    // Idle water (pre and post tick) should invoke none of the physics helpers.
    for(int i=0;i<480;++i) UpdateWaterSwimming(&player);
    assert(fixed_calls==0 && fallback_calls==0);
    // Each condition requiring physics or restoration bypasses the idle guard.
    s_fixed_waterline.player=&player; UpdateWaterSwimming(&player);
    s_fixed_waterline.player=nullptr;
    s_raised_water.active=true; UpdateWaterSwimming(&player);
    s_raised_water.active=false;
    s_config.height_offset=400; UpdateWaterSwimming(&player);
    s_config.height_offset=0;
    assert(fixed_calls==3 && fallback_calls==3);
    s_config.swimming_disabled = true;
    UpdateWaterSwimming(&player);
    assert(movement.movement_mode == 3);
    assert(volume.volume_flags == 2); // Never corrupt adjacent bit.
    assert(s_swim_volumes.size() == 1);
    // Simulate collision trying to enter swimming again: the water volume
    // remains neutralized, so our next tick returns the player to falling.
    movement.movement_mode = 4;
    UpdateWaterSwimming(&player);
    assert(movement.movement_mode == 3 && volume.volume_flags == 2);
    assert(s_swim_volumes.size() == 1);
    // Turning OFF restores exactly the original native water bit.
    s_config.swimming_disabled = false;
    g_objects_available = false;
    UpdateWaterSwimming(&player);
    assert(volume.volume_flags == 2 && s_swim_volumes.size() == 1);
    g_objects_available = true;
    UpdateWaterSwimming(&player);
    assert(volume.volume_flags == 3 && s_swim_volumes.empty());
    // Fly should be left entirely alone.
    s_config.swimming_disabled = true;
    s_swim_scan_cursor = 0;
    movement.movement_mode = 5;
    g_flying = true;
    UpdateWaterSwimming(&player);
    assert(movement.movement_mode == 5);
    g_flying = false;
    s_config.swimming_disabled = false;
    UpdateWaterSwimming(&player);
    assert(volume.volume_flags == 3);
    // Bad reflection metadata: fail closed, do not write to native memory.
    s_config.swimming_disabled = true;
    s_swim_scan_cursor = 0;
    g_bad_metadata = 1;
    movement.movement_mode = 4;
    UpdateWaterSwimming(&player);
    assert(volume.volume_flags == 3 && movement.movement_mode == 4);
    assert(warning_count >= 1);
    g_bad_metadata = 0;
    // Reload replaces the native actor at a different serial. Discard the
    // old handle rather than restoring into a potentially reused address.
    s_swim_scan_cursor = 0;
    UpdateWaterSwimming(&player);
    assert(volume.volume_flags == 2);
    s_items[0].serial_number = 2;
    s_config.swimming_disabled = false;
    UpdateWaterSwimming(&player);
    assert(s_swim_volumes.empty());
    return 0;
}
'''
compiler = shutil.which('g++') or shutil.which('clang++')
if compiler:
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp)/'test.cpp'
        exe = Path(tmp)/'test'
        src.write_text(mock+'\n'+'\n'.join(extract_function(x) for x in functions)+'\n'+test,
                       encoding='utf-8')
        command=[compiler,'-std=c++20','-Wall','-Wextra','-Werror',str(src),'-o',str(exe)]
        result=subprocess.run(command,capture_output=True,text=True,timeout=60)
        assert result.returncode==0, result.stdout+result.stderr
        result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=15)
        assert result.returncode==0, result.stdout+result.stderr
    print('PASS: native water-volume flags, collision transition, restoration, metadata failure and actor reload')
else:
    print('SKIP: compiled native water-volume simulation (C++ compiler unavailable)')

"""Compile the actual water-physics height helpers against mock UE objects.

These tests verify identity checks, reversible offsets, and coexistence with
Remove Swimming. They do not prove that Refunct permits moving its brush
volumes at runtime: only a Windows/in-game test can verify that.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
water = (ROOT / 'src/water.cpp').read_text(encoding='utf-8')
menu = (ROOT / 'src/menu.cpp').read_text(encoding='utf-8')


def extract(signature):
    start = water.index(signature)
    brace = water.index('{', start)
    depth = 0
    for offset in range(brace, len(water)):
        depth += (water[offset] == '{') - (water[offset] == '}')
        if depth == 0:
            return water[start:offset + 1]
    raise AssertionError('Unclosed function ' + signature)


signatures = [
    'bool WaterHeightVolumeValid(',
    'bool WaterHeightIsNativeWater(',
    'bool WaterHeightRestoreNative(',
    'void WaterHeightApplyVolume(',
    'void WaterHeightDiscoverAndApply(',
    'void UpdateWaterPhysicsHeight()',
]
functions = '\n\n'.join(extract(s) for s in signatures)
assert 'UpdateWaterPhysicsHeight();' in water
assert 'WaterHeightDiscoverAndApply(objects, height)' in water
assert 'verified swimming volume' in menu
assert 'surface + supported physics/raised-water fallback' in water

mock = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstddef>
#include <cwchar>
#include <string>
#include <vector>
#include <unordered_map>
#include <iostream>
using ULONGLONG = unsigned long long;
constexpr size_t kScanBatch = 2048;
constexpr ULONGLONG kScanCpuBudgetMs = 2;
struct Vec3 {float x, y, z;};
struct UObjectHeader { uint32_t flags = 0; int32_t index = -1; void* class_ptr = nullptr; };
struct SceneComponent { UObjectHeader header; Vec3 location{0,0,-50}; Vec3 scale{1,1,1}; uint8_t mobility=0; bool fail_next=false; bool reject_mobility=false; };
struct PhysicsVolume { UObjectHeader header; SceneComponent* root = nullptr; uint8_t flags = 1; };
struct GlobalObjectItem { UObjectHeader* object = nullptr; int32_t serial_number = 0; };
struct GlobalObjectArray { GlobalObjectItem* objects; int32_t num_elements; };
struct SwimVolumeTarget { void* actor = nullptr; int32_t index = -1; int32_t serial = 0; };
struct WaterHeightVolumeTarget {
    void* actor = nullptr; int32_t actor_index = -1; int32_t actor_serial = 0;
    void* root = nullptr; int32_t root_index = -1; int32_t root_serial = 0;
    Vec3 original_location{}; Vec3 original_scale{1,1,1};
    float baseline_bottom_z=0, baseline_height=0;
    uint8_t original_mobility=0; bool geometry_known=false;
    bool mobility_owned=false, scale_modified=false;
    bool height_modified = false; ULONGLONG retry_after = 0;
};
std::vector<SwimVolumeTarget> s_swim_volumes;
std::vector<WaterHeightVolumeTarget> s_height_volumes;
size_t s_height_volume_apply_cursor = 0;
int32_t s_height_volume_scan_cursor = -1;
ULONGLONG s_height_volume_next_scan = 0;
ULONGLONG s_height_volume_next_update = 0;
bool s_height_volume_warning_logged = false;
bool s_height_volume_success_logged = false;
constexpr size_t kMaxHeightVolumes = 64;
constexpr size_t kHeightApplyBatch = 4;
ULONGLONG s_now = 10000;
ULONGLONG GetTickCount64() {return s_now;}
constexpr uintptr_t kScene = 1;
constexpr uintptr_t kPhysics = 2;
bool g_is_default_volume = false;
GlobalObjectItem g_items[2];
GlobalObjectArray g_objects{g_items, 2};
PhysicsVolume g_volume;
SceneComponent g_root;
bool g_restart_requested = false;
bool g_full_level_reload_requested = false;
int g_height = 0;
struct WaterSettings {int height_offset = 0;};
WaterSettings g_settings{};
WaterSettings& WaterConfig(){return g_settings;}
const GlobalObjectArray* WaterObjects(){return &g_objects;}
bool IsReadable(const void* p, size_t){return p != nullptr;}
bool WaterIsA(void* object, const wchar_t* name){
    if(!object) return false;
    const auto* h = static_cast<const UObjectHeader*>(object);
    return (g_is_default_volume && wcscmp(name,L"DefaultPhysicsVolume")==0 &&
            h->class_ptr == reinterpret_cast<void*>(kPhysics)) ||
           (h->class_ptr == reinterpret_cast<void*>(kPhysics) && wcscmp(name,L"PhysicsVolume")==0) ||
           (h->class_ptr == reinterpret_cast<void*>(kScene) &&
            (wcscmp(name,L"SceneComponent")==0 || wcscmp(name,L"BrushComponent")==0));
}
void* ReadReflectedObject(void* actor, const wchar_t* prop){
    if(wcscmp(prop, L"RootComponent")!=0) return nullptr;
    return static_cast<PhysicsVolume*>(actor)->root;
}
bool SwimVolumeValid(const SwimVolumeTarget& s, const GlobalObjectArray* objects){
    return s.index >= 0 && s.index < objects->num_elements &&
        objects->objects[s.index].object == s.actor &&
        objects->objects[s.index].serial_number == s.serial;
}
bool SwimResolveWaterFlag(void* actor, int32_t& offset, uint8_t& mask){
    if(!actor) return false;
    offset = offsetof(PhysicsVolume, flags); mask=1; return true;
}
bool WaterHeightReadMobility(void* root, uint8_t& out){
    if(!root) return false;
    out=static_cast<SceneComponent*>(root)->mobility; return true;
}
bool WaterHeightSetMobility(void* root, uint8_t value){
    if(!root) return false;
    auto* s=static_cast<SceneComponent*>(root);
    if(s->reject_mobility) return false;
    s->mobility=value; return true;
}
bool WaterHeightGetScale(void* root, Vec3& value){
    if(!root)return false;
    value=static_cast<SceneComponent*>(root)->scale;return true;
}
bool WaterHeightSetScale(void* root, const Vec3& value){
    if(!root)return false;
    static_cast<SceneComponent*>(root)->scale=value;return true;
}
bool WaterHeightGetBounds(void*,float& bottom,float& height){
    // This is the actual world-space brush AABB after root scale and pose.
    height=900.0f * g_root.scale.z;
    bottom=g_root.location.z-height*0.5f;
    return true;
}
bool WaterGetLocation(void* root, Vec3& out){if(!root) return false;
    out=static_cast<SceneComponent*>(root)->location;return true;}
bool WaterSetLocation(void* root, const Vec3& out){
    if(!root) return false;
    auto* s = static_cast<SceneComponent*>(root);
    s->location=out;
    if(s->fail_next){s->fail_next=false;return false;}
    return true;
}
int logs = 0;
std::wstring WaterObjectName(void*){return L"MockVolume";}
void Log(const wchar_t*){++logs;}
void Notify(const std::wstring&){}
void reset() {
    g_volume = {}; g_root = {};
    g_volume.header.index = 0; g_volume.header.class_ptr = reinterpret_cast<void*>(kPhysics);
    g_root.header.index = 1; g_root.header.class_ptr = reinterpret_cast<void*>(kScene);
    g_root.location={0,0,-50}; g_volume.root=&g_root;
    g_items[0]={reinterpret_cast<UObjectHeader*>(&g_volume), 11};
    g_items[1]={reinterpret_cast<UObjectHeader*>(&g_root), 19};
    s_height_volumes.clear(); s_swim_volumes.clear();
    s_height_volume_scan_cursor=0; s_height_volume_next_scan=0; s_height_volume_apply_cursor=0;
    s_height_volume_warning_logged=false; s_height_volume_success_logged=false; s_height_volume_next_update=0;
    s_now=10000;g_settings={}; g_restart_requested=false;g_full_level_reload_requested=false;
}
'''

tests = r'''
int main(){
    reset();
    g_settings.height_offset=200;
    UpdateWaterPhysicsHeight();
    assert(s_height_volumes.size()==1 && (std::fabs(g_root.location.z - (50)) < 0.01f));
    float lower=0, depth=0;
    assert(WaterHeightGetBounds(&g_volume,lower,depth));
    assert(std::fabs(lower-(-500.f)) < 0.01f); // Bottom stays fixed.
    assert(std::fabs(depth-1100.f) < 0.01f); // Top extends by 200.
    assert(g_root.mobility==2); // Collision brush is Movable while modified.
    // Changing the offset is relative to the *original* water location.
    g_settings.height_offset=300;
    s_now+=90;
    UpdateWaterPhysicsHeight();
    assert((std::fabs(g_root.location.z - (100)) < 0.01f));
    g_settings.height_offset=0;
    s_now+=90;
    UpdateWaterPhysicsHeight();
    assert((std::fabs(g_root.location.z - (-50)) < 0.01f) && s_height_volumes.empty());
    assert(g_root.mobility==0 && std::fabs(g_root.scale.z-1)<0.01f);
    // The removal toggle can clear the water bit without losing its identity.
    reset();
    g_volume.flags=0;
    s_swim_volumes.push_back({&g_volume,0,11});
    g_settings.height_offset=100;
    UpdateWaterPhysicsHeight();
    assert((std::fabs(g_root.location.z - (0)) < 0.01f) && s_height_volumes.size()==1);
    g_settings.height_offset=0;
    s_now+=90;
    UpdateWaterPhysicsHeight();
    assert((std::fabs(g_root.location.z - (-50)) < 0.01f));
    // The global default atmosphere must NEVER be treated as the movable
    // native water brush, even if the raised-water fallback temporarily sets
    // its bWaterVolume flag to keep UE4 swimming physics stable.
    reset();
    g_is_default_volume = true;
    g_settings.height_offset=200;
    UpdateWaterPhysicsHeight();
    assert((std::fabs(g_root.location.z - (-50)) < 0.01f) && s_height_volumes.empty());
    g_is_default_volume = false;
    // Non-water physics volumes must not be moved.
    reset();
    g_volume.flags=0;
    g_settings.height_offset=200;
    UpdateWaterPhysicsHeight();
    assert((std::fabs(g_root.location.z - (-50)) < 0.01f) && s_height_volumes.empty());
    // Failed verification after a real native move must retain the baseline.
    reset();
    g_root.fail_next=true;
    g_settings.height_offset=200;
    UpdateWaterPhysicsHeight();
    assert((std::fabs(g_root.location.z - (-50)) < 0.01f));
    assert(s_height_volumes.size()==1);
    g_settings.height_offset=0;
    s_now+=2001;
    UpdateWaterPhysicsHeight();
    assert((std::fabs(g_root.location.z - (-50)) < 0.01f) && s_height_volumes.empty());
    // A stale UObjectArray serial must never be used to move another actor.
    reset();
    g_settings.height_offset=200;
    UpdateWaterPhysicsHeight();
    g_items[0].serial_number=12;
    g_settings.height_offset=400;
    s_now+=90;
    s_height_volume_scan_cursor=-1;
    s_height_volume_next_scan=s_now+100000;
    UpdateWaterPhysicsHeight();
    assert((std::fabs(g_root.location.z - (50)) < 0.01f) && s_height_volumes.empty());
    // Recycled scene-root serials are rejected independently of actor serials.
    reset();
    g_settings.height_offset=200;
    UpdateWaterPhysicsHeight();
    g_items[1].serial_number=21;
    s_now+=90;
    g_settings.height_offset=300;
    s_height_volume_scan_cursor=-1;
    s_height_volume_next_scan=s_now+100000;
    UpdateWaterPhysicsHeight();
    assert((std::fabs(g_root.location.z - (50)) < 0.01f) && s_height_volumes.empty());
    // Cooked builds that reject SetMobility must not move the physics brush.
    reset();
    g_root.reject_mobility=true;
    g_settings.height_offset=200;
    UpdateWaterPhysicsHeight();
    assert((std::fabs(g_root.location.z - (-50)) < 0.01f));
    assert(g_root.mobility==0 && std::fabs(g_root.scale.z-1)<0.01f);
    // Restart request suspends movement until the game is stable.
    reset();
    g_restart_requested=true;g_settings.height_offset=200;
    UpdateWaterPhysicsHeight();
    assert((std::fabs(g_root.location.z - (-50)) < 0.01f));
    std::cout<<"PASS: native brush expands from fixed bottom and restores original geometry\n";
}
'''
compiler = shutil.which('g++') or shutil.which('clang++')
if not compiler:
    print('SKIP: no C++20 compiler found')
else:
    with tempfile.TemporaryDirectory() as directory:
        source = Path(directory) / 'water_height.cpp'
        binary = Path(directory) / 'test'
        source.write_text(mock + '\n' + functions + '\n' + tests, encoding='utf-8')
        compiled = subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror',
                                   str(source), '-o', str(binary)],
                                  capture_output=True, text=True, timeout=25)
        assert compiled.returncode == 0, compiled.stdout + compiled.stderr
        run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
        assert run.returncode == 0, run.stdout + run.stderr
        print(run.stdout.strip())

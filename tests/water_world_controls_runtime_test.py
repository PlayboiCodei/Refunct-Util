"""Compile the actual water geometry and no-swim helpers against fake UE objects.

Does not claim to validate Refunct's native reflected APIs. The mock verifies
restoration and failure handling without launching the game.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import textwrap

ROOT = Path(__file__).resolve().parents[1]
water = (ROOT / 'src/water.cpp').read_text(encoding='utf-8')
menu = (ROOT / 'src/menu.cpp').read_text(encoding='utf-8')
records = (ROOT / 'src/records.cpp').read_text(encoding='utf-8')
header = (ROOT / 'include/refunct/features/water.hpp').read_text(encoding='utf-8')

for symbol in ('IsWaterRemoved', 'SetWaterRemoved', 'IsWaterSwimmingDisabled',
               'SetWaterSwimmingDisabled', 'GetWaterHeightOffset',
               'SetWaterHeightOffset', 'AdjustWaterHeightOffset',
               'UpdateWaterSwimming'):
    assert symbol in water and symbol in header, symbol
assert 'SetWaterHeightOffset(0);' in menu
assert 'SetWaterSwimmingDisabled(false);' in menu
assert 'SetWaterRemoved(false);' in menu
assert 'Water  >' in menu and 'Remove water surface only' in menu
assert 'WaterRemoved, WaterSwimmingOff, WaterHeight' in records
assert 'set(CheatBit::WaterHeight, GetWaterHeightOffset() != 0)' in records
assert 'WaterGameplay' in water and 'HeightOffset' in water
assert 'WaterIsTargetValid(target, objects)' in water
assert 'WaterIsOceanMaterial(material)' in water
assert 'target.height_modified = true;' in water
assert 'target.original_location_known = true;' in water


def extract_function(start):
    index = water.index(start)
    first_brace = water.index('{', index)
    depth = 0
    for i in range(first_brace, len(water)):
        if water[i] == '{':
            depth += 1
        elif water[i] == '}':
            depth -= 1
            if depth == 0:
                return water[index:i + 1]
    raise AssertionError(f'Unclosed function: {start}')

geometry = extract_function('void WaterApplyWorldGeometry(WaterTarget& target, bool remove, int height)')
mock = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <cwchar>
using ULONGLONG = unsigned long long;
struct Vec3 { float x, y, z; };
struct WaterTarget {
    void* actor = nullptr;
    bool original_visible = true;
    bool visibility_modified = false;
    Vec3 original_location{};
    bool original_location_known = false;
    bool height_modified = false;
    ULONGLONG geometry_retry_after = 0;
    ULONGLONG geometry_verify_after = 0;
    bool last_removed = false;
    int last_height_offset = 0;
};
struct Surface {
    bool visible = true;
    Vec3 location{0, 0, 15};
    bool fail_next_move = false;
    bool fail_next_visibility = false;
};
ULONGLONG g_time = 10000;
ULONGLONG GetTickCount64() { return g_time; }
bool WaterGetVisible(void* a, bool& b) { b = static_cast<Surface*>(a)->visible; return true; }
bool WaterSetVisible(void* a, bool b) {
    auto& surface = *static_cast<Surface*>(a);
    surface.visible = b;
    if (surface.fail_next_visibility) {
        surface.fail_next_visibility = false;
        return false;
    }
    return true;
}
bool WaterGetLocation(void* a, Vec3& b) { b = static_cast<Surface*>(a)->location; return true; }
bool WaterSetLocation(void* a, const Vec3& b) {
    auto& s = *static_cast<Surface*>(a);
    s.location = b;
    if (s.fail_next_move) { s.fail_next_move = false; return false; }
    return true;
}
struct MovementComponent { uint8_t movement_mode = 4; };
struct MyCharacter { MovementComponent* movement = nullptr; };
struct WaterSettings { bool swimming_disabled = false; };
WaterSettings g_config;
WaterSettings& WaterConfig() { return g_config; }
bool g_flying = false;
bool IsReadable(void* p, size_t) { return p != nullptr; }
void Log(const wchar_t*) {}
void Notify(const wchar_t*) {}
void WaterGeometryUnsupported(const wchar_t*) {}
void* FindReflectedFunction(void*, const wchar_t*) { return reinterpret_cast<void*>(0x1234); }
int32_t WaterParam(void*, const wchar_t* name, const wchar_t* type) {
    return wcscmp(name, L"NewMovementMode") == 0 && wcscmp(type, L"ByteProperty") == 0 ? 0 : -1;
}
void NativeSetMovementMode(void* object, void*, void* arguments) {
    static_cast<MovementComponent*>(object)->movement_mode =
        static_cast<uint8_t*>(arguments)[0];
}
auto g_process_event = &NativeSetMovementMode;
'''
test = r'''
int main() {
    Surface surface{};
    WaterTarget target{};
    target.actor = &surface;
    WaterApplyWorldGeometry(target, true, 200);
    assert(!surface.visible && surface.location.z == 215);
    assert(target.visibility_modified && target.height_modified);
    WaterApplyWorldGeometry(target, false, 300);
    assert(surface.visible && surface.location.z == 315);
    WaterApplyWorldGeometry(target, false, 0);
    assert(surface.visible && surface.location.z == 15);
    assert(!target.height_modified && !target.original_location_known);
    // A native setter can move the mesh but report failure. A subsequent
    // reset must still use the *original* location, never the failed value.
    surface.fail_next_move = true;
    WaterApplyWorldGeometry(target, false, -100);
    assert(surface.location.z == -85);
    assert(target.height_modified && target.original_location.z == 15);
    g_time += 1001;
    WaterApplyWorldGeometry(target, false, 0);
    assert(surface.location.z == 15 && !target.height_modified);
    // SetVisibility can mutate the component but fail its readback.
    // Restoring OFF must still know the original visibility was true.
    surface.fail_next_visibility = true;
    WaterApplyWorldGeometry(target, true, 0);
    assert(!surface.visible && target.visibility_modified);
    g_time += 1001;
    WaterApplyWorldGeometry(target, false, 0);
    assert(surface.visible && !target.visibility_modified);
    return 0;
}
'''
compiler = shutil.which('g++') or shutil.which('clang++')
if compiler:
    with tempfile.TemporaryDirectory() as tmp:
        source = Path(tmp) / 'test.cpp'
        binary = Path(tmp) / 'test'
        source.write_text(mock + '\n' + geometry + '\n' + test, encoding='utf-8')
        result = subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror',
                                 str(source), '-o', str(binary)],
                                capture_output=True, text=True, timeout=30)
        assert result.returncode == 0, result.stdout + result.stderr
        result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=15)
        assert result.returncode == 0, result.stdout + result.stderr
    print('PASS: compiled water geometry restoration and failure retry mock')
else:
    print('SKIP: compiled mock (C++ compiler unavailable); source integration checks passed')

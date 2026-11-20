"""Exercise the actual Clouds/Stars visibility update body using a C++20 mock.

This tests state transitions and retries, not native UE4 execution.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/sky.cpp').read_text(encoding='utf-8')
start = source.index('void UpdateSkyEffectVisibility(size_t index) {')
end = source.index('void UpdateDiscVisibility()', start)
function = source[start:end]

harness = r'''
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
using ULONGLONG = unsigned long long;
struct SkyScalar {
    int name = 0;
    bool name_known = false;
    float original = 0;
    float last_written = 0;
    bool modified = false;
    bool has_original = false;
};
constexpr size_t kSkyEffectCount = 2;
const wchar_t* kEffectParameterNames[kSkyEffectCount] = {
    L"Cloud opacity", L"Stars brightness"
};
struct Settings {
    std::array<bool, kSkyEffectCount> effect_visible{{true, true}};
} s_settings;
struct Target {
    void* mid = nullptr;
    std::array<SkyScalar, kSkyEffectCount> effects{};
} s_target;
std::array<ULONGLONG, kSkyEffectCount> s_effect_retry_after{};
std::array<int, kSkyEffectCount> s_stages{{-1, -1}};
std::array<float, kSkyEffectCount> native{{0.8f, 3.0f}};
std::array<int, kSkyEffectCount> writes{{0, 0}};
std::array<bool, kSkyEffectCount> accept_write{{true, true}};
ULONGLONG GetTickCount64() { return 5000; }
void EffectStage(size_t index, int stage, const wchar_t*) { s_stages[index] = stage; }
bool ResolveEffectScalarName(void*, SkyScalar& scalar, const wchar_t* param) {
    const bool cloud = param[0] == L'C';
    scalar.name = cloud ? 1 : 2;
    scalar.name_known = true;
    return true;
}
bool GetMIDScalar(void*, int name, float& result) {
    result = native[static_cast<size_t>(name - 1)];
    return true;
}
bool SetMIDScalar(void*, int name, float value) {
    const size_t index = static_cast<size_t>(name - 1);
    ++writes[index];
    if (!accept_write[index]) return false;
    native[index] = value;
    return true;
}
'''
harness += function
harness += r'''
int main() {
    // The default state leaves both native sky parameters entirely alone.
    UpdateSkyEffectVisibility(0);
    UpdateSkyEffectVisibility(1);
    assert(writes[0] == 0 && writes[1] == 0);
    s_settings.effect_visible[0] = false;
    UpdateSkyEffectVisibility(0);
    assert(native[0] == 0 && native[1] == 3);
    assert(s_target.effects[0].modified && s_target.effects[0].has_original);
    assert(!s_target.effects[1].modified);
    UpdateSkyEffectVisibility(0);
    assert(s_target.effects[0].original == 0.8f);
    s_settings.effect_visible[0] = true;
    accept_write[0] = false;
    UpdateSkyEffectVisibility(0);
    assert(s_target.effects[0].modified && native[0] == 0);
    accept_write[0] = true;
    UpdateSkyEffectVisibility(0);
    assert(native[0] == 0.8f && !s_target.effects[0].modified);
    const int count = writes[0];
    UpdateSkyEffectVisibility(0);
    assert(writes[0] == count);

    // Stars are independent and use their own saved brightness.
    s_settings.effect_visible[1] = false;
    UpdateSkyEffectVisibility(1);
    assert(native[1] == 0 && native[0] == 0.8f);
    s_settings.effect_visible[1] = true;
    UpdateSkyEffectVisibility(1);
    assert(native[1] == 3 && !s_target.effects[1].modified);

    // Native day/night updates may restore a different value themselves.
    s_settings.effect_visible[1] = false;
    UpdateSkyEffectVisibility(1);
    native[1] = 2.0f;
    s_settings.effect_visible[1] = true;
    UpdateSkyEffectVisibility(1);
    assert(native[1] == 2.0f && !s_target.effects[1].modified);

    // No zero snapshot should be taken if the stars were already absent.
    s_target.effects[1] = {};
    native[1] = 0;
    s_settings.effect_visible[1] = false;
    UpdateSkyEffectVisibility(1);
    assert(!s_target.effects[1].has_original);
    s_settings.effect_visible[1] = true;
    UpdateSkyEffectVisibility(1);
    assert(!s_target.effects[1].modified);
    assert(native[1] == 0);
    return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory) / 'sky_parts_runtime.cpp'
    exe = Path(directory) / 'sky_parts_runtime'
    path.write_text(harness)
    subprocess.run(['g++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-O2',
                    str(path), '-o', str(exe)], check=True, timeout=25)
    subprocess.run([str(exe)], check=True, timeout=10)
print('Sky Clouds/Stars OFF/ON runtime mock, restoration retry, and isolation passed')

"""Compile and exercise the real fog visibility/update functions with mocked UE4.

The native game, process memory, and Windows ABI are not exercised here.
"""
from pathlib import Path
from tempfile import TemporaryDirectory
import subprocess

root = Path(__file__).resolve().parents[1]
fog = (root/'src/fog.cpp').read_text(encoding='utf-8')
menu = (root/'src/menu.cpp').read_text(encoding='utf-8')
records = (root/'src/records.cpp').read_text(encoding='utf-8')
header = (root/'include/refunct/features/fog.hpp').read_text(encoding='utf-8')


def function_source(signature):
    start = fog.index(signature)
    opening = fog.index('{', start)
    depth = 0
    for i in range(opening, len(fog)):
        depth += (fog[i] == '{') - (fog[i] == '}')
        if depth == 0:
            return fog[start:i + 1]
    raise RuntimeError(f'Unterminated function: {signature}')


for token in (
    'L"FogDensity"', 'L"SetFogDensity"', 'L"FloatProperty"',
    'SetterFloatArgument', 'WriteDensity(0.0f)', 'ReadDensity(read_back)',
    'L"GeneralVisible", L"DirectionalVisible"',
    's_settings.visible[i] = GetPrivateProfileIntW(kSection, kVisibleKeys[i], 1,',
    'QueueSettingsValue(kSection, kVisibleKeys[i],',
    'if (s_settings.enabled[i] || hide_directional)',
    'LinearColor{0.0f, 0.0f, 0.0f, current.a}',
):
    assert token in fog, token
for token in (
    'Fog visible:', 'Directional fog visible:',
    'SetFogVisible(FogRegion::General', 'SetFogVisible(FogRegion::Directional',
    'ResetFogVisibility();',
):
    assert token in menu, token
for token in (
    'IsFogVisible(FogRegion::General)', 'IsFogVisible(FogRegion::Directional)',
    'Fog hidden', 'Directional fog hidden',
):
    assert token in records, token
for token in ('IsFogVisible(FogRegion region)', 'SetFogVisible(FogRegion region',
              'ResetFogVisibility()'):
    assert token in header, token
assert 'MarkCheatAction(' not in fog

harness = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstddef>
using ULONGLONG = unsigned long long;
constexpr size_t kRegionCount = 2;
struct LinearColor { float r,g,b,a; };
struct MyCharacter { int valid; };
struct GlobalObjectArray { int unused; };
struct FogValue {
    LinearColor original{};
    LinearColor last_written{};
    uint32_t desired_rgb = 0xFFFFFFFFu;
    LinearColor desired_colour{};
    bool modified = false;
    ULONGLONG retry_at = 0;
};
struct FogDensity {
    int property_offset = 0;
    void* setter = reinterpret_cast<void*>(1);
    int arg_offset = 0;
    float original = 0.0f;
    bool modified = false;
    ULONGLONG retry_at = 0;
};
struct FogTarget {
    void* component = reinterpret_cast<void*>(1);
    std::array<FogValue, 2> values{};
    FogDensity density{};
} s_target;
struct FogSettings {
    bool loaded = true;
    std::array<bool, 2> enabled{};
    std::array<bool, 2> visible{{true, true}};
    std::array<uint32_t, 2> colours{{0, 0}};
} s_settings;
int32_t s_scan_cursor = -1;
ULONGLONG s_next_scan = 0;
ULONGLONG now = 10000;
float nativeDensity = 0.15f;
std::array<LinearColor, 2> nativeColours{{
    {0.6f, 0.7f, 0.8f, 1.0f}, {0.8f, 0.6f, 0.4f, 1.0f}
}};
int writesDensity = 0, readsDensity = 0, conversions = 0;
std::array<int, 2> writesColour{};
bool acceptDensity = true;
std::array<bool, 2> acceptColour{{true, true}};
ULONGLONG GetTickCount64() { return now; }
void PollPicker() {}
FogSettings& Settings() { return s_settings; }
void FlushSettings() {}
bool IsReadable(const void* ptr, size_t) { return ptr != nullptr; }
GlobalObjectArray globals{};
const GlobalObjectArray* Objects() { return &globals; }
bool TargetLive(const GlobalObjectArray*) { return true; }
void ResetTarget() {}
void FindFog(const GlobalObjectArray*) {}
void ReportDensity(int,const wchar_t*) {}
void Report(size_t,int,const wchar_t*) {}
bool ReadDensity(float& result) { ++readsDensity; result = nativeDensity; return true; }
bool WriteDensity(float desired) {
    ++writesDensity;
    if (!acceptDensity) return false;
    nativeDensity = desired;
    return true;
}
bool ReadColour(const FogValue& value, LinearColor& result) {
    auto index = static_cast<size_t>(&value - s_target.values.data());
    result = nativeColours[index];
    return true;
}
bool WriteColour(const FogValue& value, const LinearColor& desired) {
    auto index = static_cast<size_t>(&value - s_target.values.data());
    ++writesColour[index];
    if (!acceptColour[index]) return false;
    nativeColours[index] = desired;
    return true;
}
LinearColor FromHex(uint32_t) { ++conversions; return {0.2f, 0.3f, 0.4f, 1.0f}; }
bool Similar(const LinearColor& a, const LinearColor& b) {
    return std::fabs(a.r-b.r)<0.002f && std::fabs(a.g-b.g)<0.002f &&
           std::fabs(a.b-b.b)<0.002f;
}
'''
harness += function_source('void UpdateFogDensityVisibility() {')
harness += '\n'
harness += function_source('void UpdateFogColours(MyCharacter* player) {')
harness += r'''
int main() {
    MyCharacter player{1};
    UpdateFogColours(&player);
    assert(writesDensity == 0 && writesColour[0] == 0 && writesColour[1] == 0);

    // General fog OFF: disable density, leave colour / directional untouched.
    s_settings.visible[0] = false;
    UpdateFogColours(&player);
    assert(nativeDensity == 0.0f && s_target.density.modified);
    assert(writesColour[0] == 0 && writesColour[1] == 0);
    assert(std::fabs(s_target.density.original - 0.15f) < 0.00001f);
    nativeDensity = 0.3f;  // Engine overwrites hidden density.
    UpdateFogColours(&player);
    assert(nativeDensity == 0.0f); // Suppression re-applied.
    assert(std::fabs(s_target.density.original - 0.3f) < 0.00001f);

    // Restore failure keeps pending state and retries once the timer expires.
    s_settings.visible[0] = true;
    acceptDensity = false;
    UpdateFogColours(&player);
    assert(s_target.density.modified && nativeDensity == 0.0f);
    acceptDensity = true;
    UpdateFogColours(&player);
    assert(s_target.density.modified);
    now += 1000;
    UpdateFogColours(&player);
    assert(!s_target.density.modified && std::fabs(nativeDensity - 0.3f)<0.00001f);

    // Directional OFF removes its light-facing colour without losing general fog.
    s_settings.visible[1] = false;
    UpdateFogColours(&player);
    assert(Similar(nativeColours[1], {0,0,0,1}));
    assert(std::fabs(nativeDensity - 0.3f) < 0.00001f);
    assert(!s_target.values[0].modified);
    assert(s_target.values[1].modified);
    // Custom directional colour must not override the visibility toggle.
    s_settings.enabled[1] = true;
    UpdateFogColours(&player);
    assert(Similar(nativeColours[1], {0,0,0,1}));
    s_settings.visible[1] = true;
    UpdateFogColours(&player);
    assert(Similar(nativeColours[1], {0.2f,0.3f,0.4f,1}));
    const int before_reads=readsDensity, before_conversions=conversions;
    for(int i=0;i<240;++i) UpdateFogColours(&player);
    assert(readsDensity==before_reads && conversions==before_conversions);
    s_settings.colours[1]=0xABCDEF; UpdateFogColours(&player);
    assert(conversions==before_conversions+1);
    // Disabling the colour override restores the original engine colour.
    s_settings.enabled[1] = false;
    acceptColour[1] = false;
    UpdateFogColours(&player);
    assert(s_target.values[1].modified);
    acceptColour[1] = true;
    UpdateFogColours(&player);
    assert(s_target.values[1].modified); // guarded retry
    now += 1000;
    UpdateFogColours(&player);
    assert(!s_target.values[1].modified);
    assert(Similar(nativeColours[1], {0.8f,0.6f,0.4f,1}));

    // Independent settings: both hidden, restore one while other stays hidden.
    s_settings.visible = {false, false};
    UpdateFogColours(&player);
    assert(nativeDensity == 0.0f && Similar(nativeColours[1], {0,0,0,1}));
    s_settings.visible[0] = true;
    UpdateFogColours(&player);
    assert(std::fabs(nativeDensity - 0.3f)<0.00001f);
    assert(Similar(nativeColours[1], {0,0,0,1}));
    s_settings.visible[1] = true;
    UpdateFogColours(&player);
    assert(Similar(nativeColours[1], {0.8f,0.6f,0.4f,1}));
    return 0;
}
'''
with TemporaryDirectory() as temp:
    src = Path(temp)/'fog_runtime.cpp'
    exe = Path(temp)/'fog_runtime'
    src.write_text(harness, encoding='utf-8')
    subprocess.run(['g++','-std=c++20','-Wall','-Wextra','-Werror',
                    '-pedantic',str(src),'-o',str(exe)],check=True,timeout=30)
    subprocess.run([str(exe)],check=True,timeout=10)
print('PASS: actual fog update functions — visibility, colour composition, retries and restoration')

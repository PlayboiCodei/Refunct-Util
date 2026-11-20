"""Compile and exercise the *real* ActiveCustomisationLabels() body with mocks.

This tests display logic and safety boundaries without claiming in-game UE4
validation; all runtime getters in the harness are simulated settings values.
"""
from pathlib import Path
from tempfile import TemporaryDirectory
import re
import shutil
import subprocess

root = Path(__file__).resolve().parents[1]
records = (root / 'src/records.cpp').read_text(encoding='utf-8')
menu = (root / 'src/menu.cpp').read_text(encoding='utf-8')
hud = (root / 'src/hud.cpp').read_text(encoding='utf-8')
source_before = records.split('std::vector<std::wstring> ActiveCustomisationLabels()', 1)[0]
actual = records.split('std::vector<std::wstring> ActiveCustomisationLabels()', 1)[1]
start = records.index('std::vector<std::wstring> ActiveCustomisationLabels()')
opening = records.index('{', start)
depth = 0
closing = -1
for i in range(opening, len(records)):
    depth += (records[i] == '{') - (records[i] == '}')
    if depth == 0:
        closing = i + 1
        break
assert closing > opening
function_body = records[start:closing]

# Verify all independent options in the Customisation > Sky and Water menus
# have display settings in the authoritative labels function.
required = {
    'SkyRegion::Zenith': 'Sky zenith colour',
    'SkyRegion::Horizon': 'Sky horizon colour',
    'SkyRegion::Sun': 'Sky sun colour',
    'SkyRegion::Clouds': 'Sky cloud colour',
    'IsSkyDiscVisible()': 'Moon/Sun disc hidden',
    'SkyEffect::Clouds': 'Clouds hidden',
    'SkyEffect::Stars': 'Stars hidden',
    'FogRegion::General': 'Fog haze colour',
    'FogRegion::Directional': 'Directional fog colour',
    'IsFogVisible(FogRegion::General)': 'Fog hidden',
    'IsFogVisible(FogRegion::Directional)': 'Directional fog hidden',
    'AreShadowsVisible()': 'Directional shadows hidden',
    'IsShadowAmbientColourEnabled()': 'Shadow ambient colour',
    'IsWaterCustomColourEnabled()': 'Water surface colour',
    'IsUnderwaterCustomColourEnabled()': 'Underwater colour',
    'IsUnderwaterTintRemoved()': 'Underwater tint removed',
    'AreWaterSplashesRemoved()': 'Water splashes removed',
    'IsCustomTimeLocked()': 'Time of day locked',
    'GetCustomTimeSpeedPercent()': 'Time of day speed',
}
for api, label in required.items():
    assert api in function_body, f'missing HUD getter: {api}'
    assert f'L"{label}' in function_body, f'missing HUD label: {label}'
    assert api in menu, f'option not present in menu: {api}'

existing_cosmetics = (
    'TileSelectedColour', 'TileUnselectedColour', 'TileRandomColour',
    'WallSelectedColour', 'WallUnselectedColour',
    'ButtonSelectedColour', 'ButtonPressedColour',
    'BorderSelectedColour', 'BorderPressedColour',
    'BeaconHidden', 'BeaconColour', 'TubeColour', 'PlatformColour',
    'PillarColour', 'PillarRemoved', 'JumpPadColour', 'TimeOfDay',
)
mask_body = records.split('constexpr uint64_t kCustomisationMask =', 1)[1].split(';', 1)[0]
label_body = records.split('std::vector<std::wstring> LabelsForMask(uint64_t active)', 1)[1].split('} // namespace', 1)[0]
for bit in existing_cosmetics:
    assert f'CheatMask(CheatBit::{bit})' in mask_body, bit
    assert f'CheatBit::{bit}' in label_body, bit
assert 'CheatMask(CheatBit::PillarRemoved)' in mask_body
assert 'CheatMask(CheatBit::PillarVisibility)' not in mask_body
assert 'CheatBit::WaterRemoved' not in mask_body
assert 'CheatBit::Fly' not in mask_body
assert 'LabelsForMask(ActiveCustomisationMask() &' in function_body
assert '~CheatMask(CheatBit::WaterColour)' in function_body
assert 'const auto customisations = ActiveCustomisationLabels();' in hud
assert 'customisations.size()' in hud
assert not re.search(r'\b(Objects|ProcessEvent|GetActor|MarkCheatAction|Log|SaveSettings)\s*\(', function_body)

compiler = shutil.which('g++') or shutil.which('clang++')
assert compiler, 'No portable C++ compiler available for function-level runtime test'

harness = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>
struct { int style = 0; } g_crosshair;

// Existing visual mask is deliberately separate from the new per-setting
// labels. The harness exercises the actual function extracted from records.cpp.
enum class CheatBit { WaterColour = 0, ExistingVisual = 1 };
using uint64_t = std::uint64_t;
constexpr uint64_t CheatMask(CheatBit bit) { return 1ULL << static_cast<int>(bit); }
uint64_t selectedMask = 0;
uint64_t ActiveCustomisationMask() { return selectedMask; }
std::vector<std::wstring> LabelsForMask(uint64_t mask) {
    std::vector<std::wstring> out;
    if (mask & CheatMask(CheatBit::WaterColour)) out.emplace_back(L"Water colours");
    if (mask & CheatMask(CheatBit::ExistingVisual)) out.emplace_back(L"Button colours");
    return out;
}

enum class SkyRegion { Zenith = 0, Horizon = 1, Sun = 2, Clouds = 3 };
enum class SkyEffect { Clouds = 0, Stars = 1 };
enum class FogRegion { General = 0, Directional = 1 };
bool surface = false, underwater = false, tint = false, splashes = false;
bool discVisible = true, customTime = false, locked = false;
bool skyEffectVisible[2]{true, true};
bool sky[4]{}, fog[2]{};
bool fogVisible[2]{true, true};
bool shadowVisible=true, shadowTint=false;
int surfaceIntensity = 100, underwaterIntensity = 100, timeSpeed = 100;
bool IsWaterCustomColourEnabled() { return surface; }
bool IsUnderwaterCustomColourEnabled() { return underwater; }
int GetWaterColourIntensityPercent() { return surfaceIntensity; }
int GetUnderwaterColourIntensityPercent() { return underwaterIntensity; }
bool IsUnderwaterTintRemoved() { return tint; }
bool AreWaterSplashesRemoved() { return splashes; }
bool IsSkyColourEnabled(SkyRegion region) { return sky[static_cast<int>(region)]; }
bool IsSkyDiscVisible() { return discVisible; }
bool IsSkyEffectVisible(SkyEffect effect) { return skyEffectVisible[static_cast<int>(effect)]; }
bool IsFogColourEnabled(FogRegion region) { return fog[static_cast<int>(region)]; }
bool IsFogVisible(FogRegion region) { return fogVisible[static_cast<int>(region)]; }
bool AreShadowsVisible() { return shadowVisible; }
bool IsShadowAmbientColourEnabled() { return shadowTint; }
bool IsCustomTimeOfDayEnabled() { return customTime; }
bool IsCustomTimeLocked() { return locked; }
int GetCustomTimeSpeedPercent() { return timeSpeed; }
''' + function_body + r'''

static bool Has(const std::vector<std::wstring>& labels, const std::wstring& expected) {
    return std::find(labels.begin(), labels.end(), expected) != labels.end();
}
int main() {
    assert(ActiveCustomisationLabels().empty());
    g_crosshair.style = 1;
    assert(Has(ActiveCustomisationLabels(), L"Custom crosshair"));
    g_crosshair.style = 0;
    selectedMask = CheatMask(CheatBit::ExistingVisual);
    assert(Has(ActiveCustomisationLabels(), L"Button colours"));
    selectedMask = CheatMask(CheatBit::WaterColour);
    assert(ActiveCustomisationLabels().empty()); // stale combined water bit suppressed
    surface = true;
    assert(Has(ActiveCustomisationLabels(), L"Water surface colour"));
    assert(!Has(ActiveCustomisationLabels(), L"Underwater colour"));
    surfaceIntensity = 140;
    assert(Has(ActiveCustomisationLabels(), L"Water surface colour: 140% intensity"));
    underwater = true;
    assert(Has(ActiveCustomisationLabels(), L"Underwater colour"));
    underwaterIntensity = 0;
    assert(Has(ActiveCustomisationLabels(), L"Underwater colour: 0% intensity"));
    surface = false;
    assert(!Has(ActiveCustomisationLabels(), L"Water surface colour: 140% intensity"));
    tint = true;
    splashes = true;
    assert(Has(ActiveCustomisationLabels(), L"Underwater tint removed"));
    assert(Has(ActiveCustomisationLabels(), L"Water splashes removed"));
    for (int i = 0; i < 4; ++i) sky[i] = true;
    discVisible = false;
    skyEffectVisible[0] = skyEffectVisible[1] = false;
    fog[0] = fog[1] = true;
    fogVisible[0] = fogVisible[1] = false;
    shadowVisible=false; shadowTint=true;
    const auto full = ActiveCustomisationLabels();
    for (auto label : {L"Sky zenith colour", L"Sky horizon colour", L"Sky sun colour", L"Sky cloud colour",
                       L"Moon/Sun disc hidden", L"Clouds hidden", L"Stars hidden", L"Fog haze colour",
                       L"Directional fog colour", L"Fog hidden", L"Directional fog hidden", L"Directional shadows hidden", L"Shadow ambient colour"}) assert(Has(full, label));
    assert(!Has(full, L"Water colours"));
    locked = true;
    timeSpeed = 200;
    assert(!Has(ActiveCustomisationLabels(), L"Time of day locked"));
    customTime = true;
    assert(Has(ActiveCustomisationLabels(), L"Time of day locked"));
    assert(Has(ActiveCustomisationLabels(), L"Time of day speed: 200%"));
    customTime = false;
    sky[0] = sky[1] = sky[2] = sky[3] = false;
    discVisible = true;
    skyEffectVisible[0] = skyEffectVisible[1] = true;
    fog[0] = fog[1] = false;
    fogVisible[0] = fogVisible[1] = true;
    shadowVisible=true; shadowTint=false;
    underwater = tint = splashes = false;
    selectedMask = 0;
    assert(ActiveCustomisationLabels().empty()); // all reset
}
'''
with TemporaryDirectory() as temp:
    source = Path(temp) / 'test_cosmetic_labels.cpp'
    binary = Path(temp) / 'test_cosmetic_labels'
    source.write_text(harness, encoding='utf-8')
    subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', '-pedantic',
                    str(source), '-o', str(binary)], check=True, timeout=30)
    subprocess.run([str(binary)], check=True, timeout=10)

print('PASS: all world-cosmetic settings covered; real HUD-label function compiled and exercised')

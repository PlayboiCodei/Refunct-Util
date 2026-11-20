"""Compile the actual master water toggle and setting APIs against mock state.

Proves precedence, restoration bookkeeping, persistence intent, and that
per-setting user preferences survive toggling. Unreal rendering still needs
live in-game validation.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
water = (ROOT / 'src/water.cpp').read_text(encoding='utf-8')
menu = (ROOT / 'src/menu.cpp').read_text(encoding='utf-8')
records = (ROOT / 'src/records.cpp').read_text(encoding='utf-8')
header = (ROOT / 'include/refunct/features/water.hpp').read_text(encoding='utf-8')


def function(signature: str) -> str:
    start = water.index(signature)
    opening = water.index('{', start)
    level = 0
    for i in range(opening, len(water)):
        level += (water[i] == '{') - (water[i] == '}')
        if level == 0:
            return water[start:i + 1]
    raise AssertionError(f'Unterminated C++ function: {signature}')


for func in ('IsAllWaterRemoved', 'SetAllWaterRemoved'):
    assert func + '(' in header and func + '(' in water and func + '(' in menu
assert 'L"WaterGameplay", L"RemoveAll"' in water
assert 'write_gameplay(RemoveAll, L"RemoveAll", setting.remove_all' in water
assert 'set(CheatBit::WaterRemoved, IsWaterRemoved() || IsAllWaterRemoved())' in records
assert 'set(CheatBit::WaterSwimmingOff, IsWaterSwimmingDisabled() || IsAllWaterRemoved())' in records
assert 'SetAllWaterRemoved(false);' in menu
assert 'L"Remove water: "' in menu
assert 'case Page::WorldWaterColours: return L"Customisation / Water";' in menu
assert 'L"Water  >"' in menu

for statement in (
    'if (!WaterNoSwimming())',
    'if (WaterNoSplashes() || !s_splash_targets.empty())',
    'const bool world_active = WaterSurfaceHidden() || config.height_offset != 0;',
    'WaterApplyWorldGeometry(target, WaterSurfaceHidden(), config.height_offset);',
    'const bool tint_effective = WaterNoTint() || custom_effective;',
    'const LinearColor selected = WaterNoTint()',
):
    assert statement in water, statement

functions = [function(name) for name in (
    'bool WaterSurfaceHidden()', 'bool WaterNoSwimming()',
    'bool WaterNoTint()', 'bool WaterNoSplashes()',
    'bool IsAllWaterRemoved()', 'void SetAllWaterRemoved(bool enabled)',
    'bool IsWaterRemoved()', 'void SetWaterRemoved(bool enabled)',
    'bool IsWaterSwimmingDisabled()', 'void SetWaterSwimmingDisabled(bool enabled)',
    'bool IsUnderwaterTintRemoved()', 'void SetUnderwaterTintRemoved(bool enabled)',
    'bool AreWaterSplashesRemoved()', 'void SetWaterSplashesRemoved(bool enabled)',
)]

mock = r'''
#include <cassert>
#include <cstdint>
#include <string>
using ULONGLONG = unsigned long long;
struct WaterSettings {
    bool remove_all=false, removed=false, swimming_disabled=false;
    bool tint_removed=false, splashes_removed=false, underwater_custom=false;
    int underwater_intensity_percent=100;
};
WaterSettings settings{};
WaterSettings& WaterConfig(){return settings;}
enum WaterSaveField : uint16_t {
 SurfaceRemoved=1<<6, SwimmingDisabled=1<<7, RemoveTint=1<<9,
 RemoveSplashes=1<<10, RemoveAll=1<<11
};
int restore_swim=0, restore_splashes=0, notifications=0;
uint16_t saved=0;
bool s_world_geometry_warning_logged=false,s_swim_resolution_warned=false;
bool s_underwater_restore_pending=false;
int s_scan_cursor=-1,s_swim_scan_cursor=-1,s_splash_scan_cursor=-1;
ULONGLONG s_next_scan=9,s_next_update=9,s_swim_next_scan=9;
ULONGLONG s_underwater_next_scan=9,s_underwater_next_apply=9;
ULONGLONG s_splash_next_scan=9,s_splash_retry_after=9;
ULONGLONG s_fixed_waterline_retry_after=0;
void SwimRestoreVolumes(){++restore_swim;}
struct GlobalObjectArray {};
const GlobalObjectArray* WaterObjects(){ static GlobalObjectArray a; return &a; }
void WaterRestoreSplashTargets(const GlobalObjectArray*){++restore_splashes;}
void QueueWaterConfigSave(uint16_t flags){saved |= flags;}
void Notify(const std::wstring&){++notifications;}
void UnderwaterSettingsChanged(const std::wstring&, uint16_t flags){saved |= flags;}
'''
test = r'''
int main() {
 assert(!IsAllWaterRemoved());
 assert(!WaterSurfaceHidden() && !WaterNoSwimming() && !WaterNoTint() && !WaterNoSplashes());
 SetWaterRemoved(true);
 SetUnderwaterTintRemoved(true);
 SetWaterSplashesRemoved(true);
 SetWaterSwimmingDisabled(false);
 assert(settings.removed && settings.tint_removed && settings.splashes_removed);
 // Preserve these preferences, and verify that the other effects stay fully removed.
 SetAllWaterRemoved(true);
 assert(IsAllWaterRemoved());
 assert(WaterSurfaceHidden() && WaterNoSwimming() && WaterNoTint() && WaterNoSplashes());
 assert(saved & RemoveAll);
 assert(s_swim_scan_cursor==0 && s_splash_scan_cursor==0);
 // Turn individual options OFF during master override; effective removal must persist.
 SetWaterRemoved(false);
 SetUnderwaterTintRemoved(false);
 SetWaterSplashesRemoved(false);
 assert(!settings.removed && !settings.tint_removed && !settings.splashes_removed);
 assert(WaterSurfaceHidden() && WaterNoSwimming() && WaterNoTint() && WaterNoSplashes());
 assert(restore_splashes==0);
 // Master OFF restores vanilla effects only when the per-feature options are OFF.
 SetAllWaterRemoved(false);
 assert(!WaterSurfaceHidden() && !WaterNoSwimming() && !WaterNoTint() && !WaterNoSplashes());
 assert(s_scan_cursor==-1 && s_splash_scan_cursor==-1);
 assert(restore_swim>=1 && restore_splashes==1);
 assert(s_underwater_restore_pending);
 // Custom colours must resume automatically once the master toggle is OFF.
 settings.underwater_custom=true;
 SetAllWaterRemoved(true);
 SetAllWaterRemoved(false);
 assert(!s_underwater_restore_pending);
 // A saved single-feature option survives master enable/disable.
 SetWaterSwimmingDisabled(true);
 SetWaterSplashesRemoved(true);
 SetAllWaterRemoved(true);
 SetAllWaterRemoved(false);
 assert(settings.swimming_disabled && settings.splashes_removed);
 assert(WaterNoSwimming() && WaterNoSplashes());
 assert(s_swim_scan_cursor==0 && s_splash_scan_cursor==0);
 return 0;
}
'''
compiler = shutil.which('clang++') or shutil.which('g++')
if not compiler:
    print('SKIP: compiled all-water override test (compiler unavailable)')
else:
    with tempfile.TemporaryDirectory() as td:
        path = Path(td) / 'water_master.cpp'
        binary = Path(td) / 'water_master'
        path.write_text(mock + '\n'.join(functions) + test, encoding='utf-8')
        compile_result = subprocess.run(
            [compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', str(path), '-o', str(binary)],
            capture_output=True, text=True, timeout=45,
        )
        assert compile_result.returncode == 0, compile_result.stderr
        result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=15)
        assert result.returncode == 0, result.stderr
    print('PASS: compiled master removal, preference preservation, and restoration decisions')

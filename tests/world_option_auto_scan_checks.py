"""Compile the production World-option gate against an isolated C++20 mock.

Covers default state, representative persistent World settings, the explicit
button-enable/disable override and restoration, without Unreal dependencies.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
sections = (root / 'src/sections.cpp').read_text(encoding='utf-8')
start = sections.index('bool AnyWorldGameplayOptionActive() {')
end = sections.index('\n// Runs on the game thread, throttled.', start)
actual = sections[start:end]
assert 's_sections.button_controls_modified = !restore_default;' in sections
assert 's_sections.button_controls_modified = true;' in sections
assert 'if (!s_button_fix_manual_pending && !AnyWorldGameplayOptionActive())' in sections
assert 's_button_fix_auto_scan_active = false;' in sections

compiler = shutil.which('g++') or shutil.which('clang++')
if compiler is None:
    raise SystemExit('SKIP: C++20 compiler not found')

harness = r'''
#include <array>
#include <cassert>
#include <cstddef>
constexpr int kPracticeClusterCount = 31;
enum class SectionPlacement { Auto, Raised, Lowered };
enum class WorldObjectKind : unsigned char { Tubes, Buttons, JumpPads, Cubes, Platforms, Tiles, Count };
bool removed[6]{};
bool IsWorldObjectRemoved(WorldObjectKind kind) { return removed[static_cast<unsigned>(kind)]; }
struct SectionState {
    bool button_controls_modified = false;
    std::array<SectionPlacement, kPracticeClusterCount> modes{};
    std::array<float, kPracticeClusterCount> x_offsets{}, y_offsets{}, height_offsets{};
} s_sections;
bool g_buttons_sync_with_sections=false, g_animate_section_changes=true;
int g_player_fov_degrees=0;
bool g_air_jump_enabled=false, g_keep_last_button_momentum=false;
bool g_progress_rise_animation_enabled=true, g_preloaded_restart_active=false;
bool g_progress_restart_active=false;
bool section_speed=false, cube_override=false, button_tile_guard=false;
bool tile_guard=false, tube_speed=false, platform_speed=false;
bool pad_height=false, all_water_removed=false, water_removed=false;
bool swimming_disabled=false, custom_map=false;
int water_height=0;
bool IsSectionSpeedModified(){return section_speed;}
bool IsWorldCubeOverrideActive(){return cube_override;}
bool IsButtonTileSelectionDisabled(){return button_tile_guard;}
bool IsTileSelectionOnContactDisabled(){return tile_guard;}
bool IsTubeTravelSpeedModified(){return tube_speed;}
bool IsPlatformSpeedModified(){return platform_speed;}
bool IsJumpPadHeightModified(){return pad_height;}
bool IsAllWaterRemoved(){return all_water_removed;}
bool IsWaterRemoved(){return water_removed;}
bool IsWaterSwimmingDisabled(){return swimming_disabled;}
int GetWaterHeightOffset(){return water_height;}
bool CustomMapActive(){return custom_map;}
''' + actual + r'''
int main() {
    assert(!AnyWorldGameplayOptionActive());
    s_sections.button_controls_modified=true;
    assert(AnyWorldGameplayOptionActive());
    s_sections.button_controls_modified=false;
    assert(!AnyWorldGameplayOptionActive());
    g_buttons_sync_with_sections=true;
    assert(AnyWorldGameplayOptionActive());
    g_buttons_sync_with_sections=false;
    assert(!AnyWorldGameplayOptionActive());
    s_sections.modes[7]=SectionPlacement::Raised;
    assert(AnyWorldGameplayOptionActive());
    s_sections.modes[7]=SectionPlacement::Auto;
    s_sections.height_offsets[7]=12;
    assert(AnyWorldGameplayOptionActive());
    s_sections.height_offsets[7]=0;
    assert(!AnyWorldGameplayOptionActive());
    cube_override=true;
    assert(AnyWorldGameplayOptionActive());
    cube_override=false;
    custom_map=true;
    assert(AnyWorldGameplayOptionActive());
    custom_map=false;
    g_player_fov_degrees=115;
    assert(!AnyWorldGameplayOptionActive()); // FOV is a visual customisation.
    g_player_fov_degrees=0;
    water_height=50;
    assert(AnyWorldGameplayOptionActive());
    water_height=0;
    assert(!AnyWorldGameplayOptionActive());
    // Pillar removal belongs to Customisation, but the new World removal controls
    // explicitly enable the all-37-button monitoring gate.
    assert(!AnyWorldGameplayOptionActive());
    removed[0]=true;
    assert(AnyWorldGameplayOptionActive());
    removed[0]=false;
    assert(!AnyWorldGameplayOptionActive());
}
'''
with tempfile.TemporaryDirectory(prefix='refunct-world-gate-') as td:
    cpp = Path(td) / 'gate.cpp'
    exe = Path(td) / 'gate'
    cpp.write_text(harness, encoding='utf-8')
    subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', str(cpp), '-o', str(exe)],
                   check=True, capture_output=True, text=True)
    subprocess.run([str(exe)], check=True, capture_output=True, text=True)
print('PASS: production World-option gate, defaults, toggles and native button overrides')

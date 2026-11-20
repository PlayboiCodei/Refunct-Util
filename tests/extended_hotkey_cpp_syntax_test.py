"""Compile the exact Action enum and ExecuteAction switch against typed minimal stubs.

Does not replace an MSVC Win32 game build; catches misspelled APIs/enums and
invalid C++ introduced by hotkey expansions.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
header = (root / 'include/refunct/state.hpp').read_text(encoding='utf8')
menu = (root / 'src/menu.cpp').read_text(encoding='utf8')
enum = 'enum class Action : size_t {' + header.split('enum class Action : size_t {', 1)[1].split('};', 1)[0] + '};'
switch = menu.split('void ExecuteAction(Action action, MyCharacter* player) {', 1)[1].split('\n}\n\n// Keep the legacy page-local entry point', 1)[0]
code = r'''
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <string>
namespace refunct {
struct MyCharacter {};
'''+enum+r'''
enum class CubeWorldAction { DisableAll, CollectAll, ShowAll, ShowAboveWater, RestoreDefault };
std::atomic_bool g_menu_open{false}, g_rebinding{false};
int g_rebind_action{-1};
bool g_restart_requested{}, g_preloaded_hook_available{},g_preloaded_restart_requested{};
bool g_restore_flying_pending{},g_restore_noclip_pending{},g_flying{},g_noclip{};
bool g_show_coordinates{},g_show_timer{},g_show_game_time_hud{},g_show_local_time_hud{};
bool g_show_speed{},g_show_fps{},g_show_active_cheats{},g_show_active_customisations{};
bool g_show_enabled_modules{},g_show_completion_percent{},g_show_splits{};
bool g_show_pb_run_hud{},g_show_sum_of_best_hud{},g_show_last_button_split_hud{};
bool g_show_menu_descriptions{},g_teleport_target_initialized{};
struct Vec3 {float x,y,z;}; Vec3 g_teleport_target{};
int g_practice_selected_button{1}; float g_fly_speed{500};
void Notify(const std::wstring&);
void SaveSettings(); void SaveMovementModeSettings();
void RequestProgressRestart(); void RequestFullLevelReload();
void SetFlying(MyCharacter*,bool);void SetNoclip(MyCharacter*,bool);
void SavePosition(MyCharacter*);void RestorePosition(MyCharacter*);
void RequestPractice(int);void SaveCurrentCheckpoint(MyCharacter*);
void LoadSavedCheckpoint();void LoadLastButtonCheckpoint();
void LoadSecondLastButtonCheckpoint();void TeleportToLastPressedButton(MyCharacter*);
bool TeleportToCoordinates(MyCharacter*,const Vec3&,bool=false);
void ToggleAirJump();void TogglePreserveMomentum();
void CycleJumpAssistMode(bool,int);
bool IsCustomTimeOfDayEnabled();void SetCustomTimeOfDayEnabled(bool);
bool IsCustomTimeLocked();void SetCustomTimeLocked(bool);
void AdjustCustomTimeSpeedPercent(int);void AdjustCustomTimeMinutes(int);
void RefreshCustomMapFiles();bool RequestCustomMapRestore();
bool RequestCubeWorldAction(CubeWorldAction, MyCharacter*);
enum class HudElement {Cubes, Tiles, ButtonNumber, KeyDisplay, PracticeStatus,
                      RecordStatus, CompareTime, BestSegmentTime, PBSplitTime};
void ToggleHudElement(HudElement);
bool IsPBGhostEnabled();void SetPBGhostEnabled(bool);
bool IsPBGhostPassThroughEnabled();void SetPBGhostPassThroughEnabled(bool);
bool IsPBGhostFastestSectionsEnabled();void SetPBGhostFastestSectionsEnabled(bool);
bool StartGhostReplayViewer();bool IsGhostReplayRequested();
void ToggleGhostReplayPause();bool SelectWorldTiles(bool);
void ExecuteAction(Action action, MyCharacter* player) {
'''+switch+r'''
}
}
'''
with tempfile.TemporaryDirectory() as folder:
    path = Path(folder)/'hotkeys.cpp'
    path.write_text(code,encoding='utf8')
    result = subprocess.run(['clang++','-std=c++20','-Wall','-Wextra','-Werror','-fsyntax-only',str(path)],
                            capture_output=True,text=True,timeout=30)
    assert result.returncode == 0, result.stderr
print('PASS: native ExecuteAction and append-only Action enum compile as strict C++20')

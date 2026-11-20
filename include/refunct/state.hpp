#pragma once
// Process-wide data, kept in state.cpp. Only the game tick may mutate non-atomic state.
#include "refunct/game_abi.hpp"
#include "refunct/pure.hpp"
#include "refunct/section_compare.hpp"
#include "refunct/jump_modes.hpp"
namespace refunct {

extern CharacterTickFn g_original_tick;
extern NewGameEventFn g_original_new_game_event;
extern HudDrawFn g_original_hud;
extern SlateKeyFn g_original_keydown;
extern SlateKeyFn g_original_keyup;
extern HudDrawTextFn g_draw_text;
extern HudDrawRectFn g_draw_rect;
extern HudTextSizeFn g_text_size;
extern ViewportSizeFn g_viewport_size;
extern SetCollisionFn g_set_collision;
extern FNameAppendFn g_fname_append;
extern MemoryFreeFn g_memory_free;
extern ProcessEventFn g_process_event;
extern GetAccurateTimeFn g_get_accurate_time;
extern BasedCharacterFn g_add_based_character;
extern BasedCharacterFn g_remove_based_character;
// Trampoline for Refunct's native ALiftBase::AddBasedCharacter callback.
// Hook is optional; without it, native button gating must remain disabled.
extern BasedCharacterFn g_original_add_based_character;
extern std::atomic_bool g_section_button_contact_hook_available;
extern HMODULE g_module;
extern HMODULE g_game_module;
extern std::wstring g_settings_path;
// Independent diagnostic logging setting. Safe to read from any thread.
extern std::atomic_bool g_util_log_enabled;
extern std::atomic_bool g_startup_log_enabled; // Independent preference; master still applies.
extern std::wstring g_storage_dir;
extern std::wstring g_splits_dir;
extern std::wstring g_pb_path;
extern std::wstring g_active_profile;
extern std::vector<std::wstring> g_profiles;

// All UI and game writes happen on Refunct's character tick thread.
// Slate callbacks only enqueue key presses; atomics bridge the two callbacks.
extern std::array<std::atomic_uint8_t, 256> g_pending;
extern std::atomic_bool g_pending_any;
extern std::array<std::atomic_uint8_t, 256> g_suppress_keyup;
extern std::atomic_bool g_menu_open;
extern std::atomic_bool g_rebinding;
// Keyboard capture for the X/Y/Z numeric coordinate editor.
extern std::atomic_bool g_coordinate_editing;
extern Vec3 g_teleport_target;
extern bool g_teleport_target_initialized;

// Keep these IDs stable because they are also stored in the INI file.
enum class Action : size_t {
    Menu, Restart, Fly, Noclip, SavePosition, RestorePosition,
    Coordinates, Timer, SpeedUp, SpeedDown, ReplaySection,
    SaveState, LoadState, LoadLastButton, PreloadedRestart, RestartWithProgress,
    LoadSecondLastButton, FullLevelReload, // Existing action IDs never move.
    TeleportLastButtonGlobal,
    TeleportSavedCoordinates,
    ToggleAirJump,
    TogglePreserveMomentum,
    CycleScrollJump,
    CycleHoldJump,
    ToggleInGameTimeHUD,
    ToggleRealTimeHUD,
    ToggleSpeedHUD,
    ToggleFPSHUD,
    ToggleCheatsHUD,
    ToggleCustomisationsHUD,
    ToggleEnabledModulesHUD,
    ToggleCompletionHUD,
    ToggleSplitPopups,
    TogglePBRunHUD,
    ToggleSumOfBestHUD,
    ToggleLastSplitHUD,
    ToggleTimeOverride,
    ToggleTimeLock,
    IncreaseTimeSpeed,
    DecreaseTimeSpeed,
    IncreaseGameTime,
    DecreaseGameTime,
    RefreshMaps,
    RestoreOriginalMap,
    ToggleMenuDescriptions,
    SelectAllCubes,
    // New optional bindings. Defaults remain None to preserve gameplay keys.
    ToggleCubesHUD,
    ToggleTilesHUD,
    ToggleButtonNumberHUD,
    ToggleKeyDisplayHUD,
    TogglePracticeStatusHUD,
    ToggleRecordStatusHUD,
    ToggleComparisonHUD,
    ToggleBestSegmentHUD,
    TogglePBSplitHUD,
    TogglePBGhost,
    ToggleGhostMode,
    ToggleGhostFastestSections,
    ToggleGhostReplay,
    ToggleGhostReplayPause,
    SelectAllTiles,
    Count
};
constexpr size_t kActionCount = static_cast<size_t>(Action::Count);
struct ActionInfo { const wchar_t* label; const wchar_t* ini_name; int default_key; };
constexpr std::array<ActionInfo, kActionCount> kActions{{
    {L"Menu",             L"Menu",            VK_F5},
    {L"Restart run",      L"Restart",         'R'},
    {L"Flying",           L"Fly",             VK_F6},
    {L"Noclip",           L"Noclip",          VK_F7},
    {L"Save position",    L"SavePosition",    VK_F8},
    {L"Restore position", L"RestorePosition", VK_F9},
    {L"Coordinates",      L"Coordinates",     VK_F10},
    {L"Timer",            L"Timer",           VK_F3},
    {L"Faster flight",    L"SpeedUp",         VK_PRIOR},
    {L"Slower flight",    L"SpeedDown",       VK_NEXT},
    {L"Replay section",   L"ReplaySection",   VK_F2},
    {L"Save state",       L"SaveState",       VK_F4},
    {L"Load state",       L"LoadState",       VK_F1},
    {L"Load last button", L"LoadLastButton", 'B'},
    {L"Preloaded restart", L"PreloadedRestart", 'G'},
    {L"Restart with progress", L"RestartWithProgress", 'H'},
    {L"Load second last button", L"LoadSecondLastButton", 'N'},
    {L"Full level reload", L"FullLevelReload", 'L'},
    {L"Global last-button teleport", L"TeleportGlobalLastButton", 0},
    {L"Teleport to saved XYZ", L"TeleportSavedXYZ", 0},
    {L"Toggle air jump", L"ToggleAirJump", 0},
    {L"Toggle preserve momentum", L"ToggleMomentum", 0},
    {L"Cycle scroll-jump mode", L"CycleScrollJump", 0},
    {L"Cycle hold-jump mode", L"CycleHoldJump", 0},
    {L"Toggle in-game clock HUD", L"ToggleGameTimeHUD", 0},
    {L"Toggle real clock HUD", L"ToggleRealTimeHUD", 0},
    {L"Toggle speed HUD", L"ToggleSpeedHUD", 0},
    {L"Toggle FPS HUD", L"ToggleFPSHUD", 0},
    {L"Toggle Cheats Active HUD", L"ToggleCheatsHUD", 0},
    {L"Toggle Customisations HUD", L"ToggleCustomisationsHUD", 0},
    {L"Toggle enabled modules HUD", L"ToggleEnabledModulesHUD", 0},
    {L"Toggle completion HUD", L"ToggleCompletionHUD", 0},
    {L"Toggle split popups", L"ToggleSplitPopups", 0},
    {L"Toggle PB run HUD", L"TogglePBRunHUD", 0},
    {L"Toggle Sum of Best HUD", L"ToggleSumOfBestHUD", 0},
    {L"Toggle last-button split HUD", L"ToggleLastSplitHUD", 0},
    {L"Toggle custom time mode", L"ToggleTimeOverride", 0},
    {L"Toggle lock time", L"ToggleTimeLock", 0},
    {L"Increase time speed", L"TimeSpeedUp", 0},
    {L"Decrease time speed", L"TimeSpeedDown", 0},
    {L"Advance game time 15m", L"GameTimeForward", 0},
    {L"Rewind game time 15m", L"GameTimeBack", 0},
    {L"Refresh maps list", L"RefreshMaps", 0},
    {L"Restore original map", L"RestoreOriginalMap", 0},
    {L"Toggle menu help", L"ToggleMenuDescriptions", 0},
    {L"Select all cubes", L"SelectAllCubes", 0},
    {L"Toggle cubes HUD", L"ToggleCubesHUD", 0},
    {L"Toggle tiles HUD", L"ToggleTilesHUD", 0},
    {L"Toggle button number HUD", L"ToggleButtonNumberHUD", 0},
    {L"Toggle key display HUD", L"ToggleKeyDisplayHUD", 0},
    {L"Toggle practice HUD", L"TogglePracticeStatusHUD", 0},
    {L"Toggle record status HUD", L"ToggleRecordStatusHUD", 0},
    {L"Toggle comparison HUD", L"ToggleComparisonHUD", 0},
    {L"Toggle fastest segment HUD", L"ToggleBestSegmentHUD", 0},
    {L"Toggle PB split HUD", L"TogglePBSplitHUD", 0},
    {L"Toggle PB ghost", L"TogglePBGhost", 0},
    {L"Toggle ghost 2D / 3D", L"ToggleGhostMode", 0},
    {L"Toggle fastest-section ghost", L"ToggleGhostFastestSections", 0},
    {L"Start / stop ghost replay", L"ToggleGhostReplay", 0},
    {L"Pause / resume ghost replay", L"ToggleGhostReplayPause", 0},
    {L"Select all tiles", L"SelectAllTiles", 0},
}};
extern std::array<std::atomic_int, kActionCount> g_keys;

extern bool g_show_coordinates;
extern bool g_show_timer;
// Optional HUD diagnostics, OFF by default.
extern bool g_show_fps;
// Read-only HUD clocks; neither changes game time, speedrun time, or PB eligibility.
extern bool g_show_game_time_hud;
extern bool g_show_local_time_hud;
extern bool g_show_speed;
extern bool g_show_completion_percent;
// Read-only native button progress (0..37), independently toggleable.
extern bool g_show_button_number;
extern bool g_show_key_display;
// Conditional safety/practice rows remain on by default for existing users.
extern bool g_show_practice_status_hud;
extern bool g_show_record_status_hud;
extern bool g_show_enabled_modules;
extern bool g_show_active_cheats;
extern bool g_show_active_customisations;
// Separate short-lived popup: normal Notify messages remain visible too.
extern std::wstring g_cheat_popup_text;
extern ULONGLONG g_cheat_popup_expires;
// Show a short explanation for the currently highlighted F5 menu entry.
extern bool g_show_menu_descriptions;
// Independently position each persistent HUD component; split popups keep their own layout.
// Enum values are persisted in settings.ini. Append new placements to preserve saved positions.
enum class HudCorner : uint8_t {
    TopRight = 0, TopLeft = 1, BottomRight = 2, BottomLeft = 3, Center = 4
};
constexpr size_t kHudPlacementCount = static_cast<size_t>(HudCorner::Center) + 1;
enum class HudElement : size_t {
    Timer, PBRun, SumOfBest, LastButtonSplit, Coordinates, Cubes, Tiles,
    FPS, EnabledModules, PracticeStatus, RecordStatus, Speed, CompareTime,
    BestSegmentTime, PBSplitTime, CompletionPercent, ActiveCheats, ActiveCustomisations,
    // New IDs appended to preserve saved corner indices.
    GameTime, LocalTime, ButtonNumber, KeyDisplay, Count
};
constexpr size_t kHudElementCount = static_cast<size_t>(HudElement::Count);
constexpr std::array<const wchar_t*, kHudElementCount> kHudPositionKeys{{
    L"TimerPosition", L"PBRunPosition", L"SumOfBestPosition",
    L"LastButtonSplitPosition", L"CoordinatesPosition", L"CubesPosition",
    L"TilesPosition", L"FPSPosition", L"EnabledModulesPosition",
    L"PracticeStatusPosition", L"RecordStatusPosition", L"SpeedPosition",
    L"CompareTimePosition", L"BestSegmentTimePosition", L"PBSplitTimePosition",
    L"CompletionPercentPosition", L"ActiveCheatsPosition",
    L"ActiveCustomisationsPosition", L"GameTimePosition", L"LocalTimePosition",
    L"ButtonNumberPosition", L"KeyDisplayPosition"
}};
constexpr std::array<const wchar_t*, kHudElementCount> kHudPositionLabels{{
    L"Run timer", L"Full-run PB", L"Sum of best segments", L"Run time at last button",
    L"Player XYZ", L"Cubes collected", L"Tiles coloured", L"Frame rate (FPS)",
    L"Enabled features", L"Practice status", L"PB recording status", L"Movement speed",
    L"Other profile time", L"Fastest button time", L"PB time at button",
    L"Level completion (%)", L"Active gameplay cheats", L"Active visual settings",
    L"Game day/night clock", L"PC local clock", L"Current button number",
    L"Key presses (WASD / Space / Shift)"
}};
constexpr std::array<HudCorner, kHudElementCount> kDefaultHudCorners{{
    HudCorner::TopLeft, HudCorner::TopLeft, HudCorner::TopLeft,
    HudCorner::TopLeft, HudCorner::TopLeft, HudCorner::TopLeft,
    HudCorner::TopLeft, HudCorner::TopRight, HudCorner::TopRight,
    HudCorner::TopLeft, HudCorner::TopRight, HudCorner::TopLeft,
    HudCorner::TopLeft, HudCorner::TopLeft, HudCorner::TopLeft,
    HudCorner::TopLeft, HudCorner::TopRight, HudCorner::TopRight,
    HudCorner::TopLeft, HudCorner::TopRight, HudCorner::TopLeft,
    HudCorner::BottomRight
}};
extern std::array<HudCorner, kHudElementCount> g_hud_corners;
// Persisted appearance settings for independently drawn, persistent HUD items.
// Colour 0 = each line's original semantic colour (warning, status, etc.).
// Colours 1..9 are fixed presets; 10 = user-defined RRGGBB.
constexpr int kHudColourModeCount = 11;
constexpr int kHudMinSizePercent = 50;
constexpr int kHudMaxSizePercent = 200;
constexpr int kHudSizeStepPercent = 10;
extern std::array<uint8_t, kHudElementCount> g_hud_colour_modes;
extern std::array<uint32_t, kHudElementCount> g_hud_custom_colours;
extern std::array<int, kHudElementCount> g_hud_size_percent;
// Headers default to the legacy display style. Timer/XYZ/record status never
// had a separate heading, so those default to value-only.
constexpr std::array<bool, kHudElementCount> kDefaultHudShowHeaders{{
    false, true, true, true, false, true, true,
    true, true, true, false, true, true, true,
    true, true, true, true, true, true, true, true
}};
extern std::array<bool, kHudElementCount> g_hud_show_headers;
// Keep existing persisted corner values (1..4) stable; a previous Centre (0)
// setting is migrated to TopRight when loading settings.
enum class MenuPosition : uint8_t {
    TopRight = 1, TopLeft = 2, BottomRight = 3, BottomLeft = 4
};
extern MenuPosition g_menu_position;
// HUD progression denominator: whole map or just clusters available so far.
// Preserve the legacy Off/On settings through storage.cpp migration.
enum class ProgressDisplayMode : uint8_t { Off = 0, Total = 1, Loaded = 2 };
extern ProgressDisplayMode g_cube_display_mode;
extern ProgressDisplayMode g_tile_display_mode;
extern bool g_show_splits;
// Independent, persistent top-left HUD records (unrelated to record saving).
extern bool g_show_pb_run_hud;
extern bool g_show_sum_of_best_hud;
extern bool g_show_last_button_split_hud;
extern bool g_show_compare_hud;       // Persistent last-split comparison.
extern RecordTimeView g_compare_hud_view; // Cumulative or individual split delta.
extern bool g_show_best_segment_time_hud; // Active profile: best individual segment.
extern bool g_show_pb_split_time_hud;     // Active profile: PB time readout.
extern RecordTimeView g_best_segment_time_view; // Segment (default) or cumulative best.
extern RecordTimeView g_pb_split_time_view;     // Cumulative (default) or PB segment.
// Section-only elapsed-time deltas. Both default to Off and never affect recording.
// Difference in PB lead between the previous and current *button* split.
// Independent of best segment records and section-practice comparisons.
extern SectionCompareDisplayMode g_pb_gain_loss_display;
extern SectionCompareDisplayMode g_section_vs_pb_display;
extern SectionCompareDisplayMode g_section_vs_file_display;
extern bool g_show_compare_popup;     // Centered signed split-popup row.
extern bool g_compare_popup_show_time; // False: sign only (+/-), not elapsed time.
extern bool g_compare_popup_show_header; // False: omit the profile name/header.
extern ProfileCompareMode g_compare_mode;
extern std::wstring g_compare_profile; // Independent from the recording profile.
// These control only split popup presentation, never record eligibility.
extern SplitRowMode g_segment_row_mode;
extern SplitRowMode g_pb_row_mode;
extern bool g_show_split_button;
extern bool g_show_split_total_time;
extern bool g_show_split_fastest_delta;
// Include the duration in the fastest-segment comparison; OFF displays only +/-.
extern bool g_show_fastest_delta_time;
// Display "Fastest segment" before the fastest-segment +/- comparison.
extern bool g_show_fastest_delta_label;
extern bool g_flying;
extern bool g_noclip;
// Independent, persistent policy toggles. Both OFF preserves v9.27 defaults.
extern bool g_keep_movement_on_game_launch;
extern bool g_keep_movement_on_map_restart;
// World gameplay tuning and object overrides are independent of movement and
// visual customisation. Both OFF by default, matching vanilla reset behaviour.
extern bool g_keep_world_on_game_launch;
extern bool g_keep_world_on_map_restart;
// When B loads the last real button, optionally resume the captured 3D velocity.
// Independent from the global jump momentum tuning. OFF by default.
extern bool g_keep_last_button_momentum;
// Persisted record-saving preferences. Disabling these leaves saved records,
// comparisons and timing displays untouched; cheat protection stays active.
extern bool g_save_personal_bests;
extern bool g_save_fastest_segments;
extern bool g_save_pb_ghosts;
extern bool g_save_fastest_ghosts;
// Make a separate immutable-at-creation snapshot on each successful new PB.
// Existing active PB profile and its fastest-segment records are unchanged.
extern bool g_auto_create_pb_profile;
extern bool g_auto_pb_copy_best_segments;
// OFF by default. Permission to save assisted records is separate from
// the regular Save PB / Save Fastest toggles. Stored in settings.ini.
extern CheatRecordMode g_cheat_record_mode;
// Snapshot at button zero: enabling PB cheat recording mid-run cannot
// retroactively validate an earlier assisted run.
extern bool g_pb_cheats_allowed_for_run;
// PB recording must stay enabled for the entire run. Re-enabling it mid-run
// never retroactively makes a previously unrecorded run a valid PB.
extern bool g_pb_recording_enabled_for_run;
// A native restart can destroy the current character. Defer re-enabling
// flying/noclip until a valid character is ticking again; never write old pointers.
extern bool g_restore_flying_pending;
extern bool g_restore_noclip_pending;
extern bool g_saved_valid;
extern bool g_game_data_valid;
// Refunct native LevelState progression counters, never synthetic counts.
constexpr int kTotalCubes = 18;
constexpr int kTotalColourableTiles = 251;
struct ProgressCounters {
    int cubes = 0;
    int tiles = 0;
    bool valid = false;
    // Derived by summing the native level-root actor arrays for unlocked
    // clusters; never guessed from the count of pressed buttons.
    int loaded_cubes = 0;
    int loaded_tiles = 0;
    bool loaded_valid = false;
    int cached_level = -1;
    int cached_resets = -1;
    ULONGLONG next_loaded_refresh = 0;
};
extern ProgressCounters g_progress_counters;
extern bool g_restart_requested;
extern bool g_full_level_reload_requested;
// New Game Actions -> All Buttons, as implemented by refunct-tas/tool/newgame.re.
// The preloaded run is not a legal vanilla starting state; never save PB/golds.
extern bool g_preloaded_restart_requested;
// Unlike G, this restores only buttons previously pressed, using the game's
// real button-activation callbacks. It is not a standard leaderboard run.
extern bool g_progress_restart_active;
extern bool g_progress_restart_pending;
// Shared restart setting: ON retains the native rise animation for R/H.
// OFF accelerates the level-root rise speeds for R/H, with a timed restore.
// G (full-map preload) and F1/F2/B checkpoint restore are unchanged.
extern bool g_progress_rise_animation_enabled;
// ON = native animated rises and optional animated manual placement.
// OFF = near-instant native automatic rises and instant manual placement.
// OFF changes gameplay and makes the run assisted.
extern bool g_animate_section_changes;
// 100% is the original native BP_LevelRoot.Speed and manual animation duration.
extern int g_section_speed_percent;
// One World > Sections option; grounded in observed platform positions.
// The native button-state inspection report helps validate reversible activation.
extern bool g_buttons_sync_with_sections;
// A fast R restart is never eligible for official timing records. Keep this
// separate from the brief duration of the level-root speed override.
extern bool g_normal_fast_restart_assisted;
extern bool g_normal_fast_rise_active;
extern bool g_normal_fast_rise_reset_seen;
extern int g_normal_fast_rise_base_resets;
extern ULONGLONG g_normal_fast_rise_started_at;
extern ULONGLONG g_normal_fast_rise_restore_at;
// Snapshot the setting when a restore begins; toggling the menu later cannot
// change a restoration already in progress.
extern bool g_progress_rise_animation_this_load;
extern ULONGLONG g_progress_next_button_at;
constexpr ULONGLONG kProgressRiseIntervalMs = 850;
extern bool g_preloaded_hook_available;
extern bool g_preloaded_restart_armed;
extern bool g_preloaded_restart_active;
extern bool g_preloaded_new_game_seen;
extern bool g_preloaded_restart_confirmed;
extern bool g_preloaded_restart_failed;
extern bool g_preloaded_reset_seen;
extern bool g_preloaded_fast_rise_used;
extern int g_preloaded_base_resets;
extern ULONGLONG g_preloaded_started_at;
extern ULONGLONG g_preloaded_ready_at;
extern float g_fly_speed;
extern float g_original_fly_speed;
// Percent relative to the game's runtime defaults. 100% never changes physics.
extern int g_walk_speed_percent;
extern int g_swim_speed_percent;
// Multiplier of BP_MyCharacter.ClimbSpeed (resolved through UE4 reflection).
extern int g_climb_speed_percent;
extern int g_slide_speed_percent;
extern int g_slide_distance_percent;
extern int g_jump_height_percent;
extern int g_acceleration_percent;
extern int g_game_speed_percent;
// 0 = use Refunct's original camera FOV. Other values are degrees (60..150).
// Affects the player's camera (not movement); user-requested cheat tracking
// treats all non-HUD world/character adjustments as assisted.
extern int g_player_fov_degrees;
// Preserve horizontal momentum across jumps and ledges, while leaving
// ground movement, gravity and collisions under Refunct control.
extern bool g_preserve_momentum;
// Scroll-up and Hold Space both use the native Space key path. The seven
// target combinations are an assist filter, not a replacement for Refunct's
// physical Space binding. Climb requires holding Space, not repeated pulses.
// Enable switches reset on ordinary restarts unless map-restart persistence
// is enabled; target choices always persist.
extern JumpTargets g_scroll_jump_targets;
extern JumpTargets g_hold_jump_targets;
extern std::atomic_bool g_scroll_to_jump;
extern bool g_hold_space_to_jump;
// A fresh Space press (or enabled wheel jump) launches while falling.
// Unlimited presses; no automatic repeat when Space is held.
extern bool g_air_jump_enabled;
extern std::atomic_int g_wheel_jump_requests;
extern HHOOK g_mouse_wheel_hook;
extern DWORD g_mouse_wheel_thread;
extern ULONGLONG g_mouse_wheel_retry_after;
extern bool g_mouse_wheel_warning_logged;

// Post normal Space key-down/up on the game tick, never from the mouse hook.
constexpr LPARAM kSpaceKeyDownLParam = static_cast<LPARAM>(0x00390001u);
constexpr LPARAM kSpaceKeyUpLParam   = static_cast<LPARAM>(0xC0390001u);
struct JumpAssistState {
    bool grounded_on_previous_tick = false;
    // Hold+Wall needs repeated key presses, but never multiple per tick.
    // Allow a short grace period after leaving the ground before trying.
    ULONGLONG next_hold_wall_attempt_at = 0;
    bool pulse_pending_release = false;
    // A scroll-up climb request keeps synthetic Space held briefly; release
    // on timeout, landing, a mode change, loss of focus, or a game restart.
    bool synthetic_climb_held = false;
    ULONGLONG synthetic_climb_release_at = 0;
    HWND pressed_window = nullptr;
};
extern JumpAssistState g_jump_assist;

// There is no arbitrary maximum. Integer representation and finite UE4 float
// properties are the technical limits; protect the menu against overflow.
constexpr int kMovementMinimumPercent = 20;
constexpr int kGameSpeedMinimumPercent = 25;
// Slide percentages use the same overflow-safe adjustment range as other movement settings.
// The slide velocity writer separately maintains its physics safety check.
constexpr int kMaximumRepresentablePercent = (std::numeric_limits<int>::max)();

// UE reflection resolves version-specific float fields at runtime, rather
// than assuming hard-coded UE4 CharacterMovement component member offsets.
struct TunedFloatProperty {
    float* address = nullptr;
    float original = 0.0f;
    float last_written = 0.0f;
    bool modified = false;
};
extern MyCharacter* g_tuning_player;
extern MovementComponent* g_tuning_movement;
extern TunedFloatProperty g_tuning_max_walk_speed;
extern TunedFloatProperty g_tuning_max_swim_speed;
extern TunedFloatProperty g_tuning_climb_speed;
extern TunedFloatProperty g_tuning_base_speed;
extern TunedFloatProperty g_tuning_bonus_speed;
extern TunedFloatProperty g_tuning_jump_velocity;
extern TunedFloatProperty g_tuning_max_acceleration;
// Native UE4 airborne drag/braking; these are resolved with UE reflection and
// restored to their original values when the feature is disabled.
extern TunedFloatProperty g_tuning_falling_friction;
extern TunedFloatProperty g_tuning_falling_braking;
extern bool g_tuning_warning_logged;

struct AirMomentumState {
    MovementComponent* movement = nullptr;
    bool have_ground_sample = false;
    bool airborne = false;
    float ground_x = 0.0f;
    float ground_y = 0.0f;
    float minimum_air_speed = 0.0f;
    float last_air_x = 0.0f;
    float last_air_y = 0.0f;
};
extern AirMomentumState g_air_momentum;
// Cached UObject field from the current UWorld's PersistentLevel.WorldSettings.
// This mirrors refunct-tas UWorld::set_time_dilation (no FApp clock override).
extern void* g_time_dilation_world;
extern void* g_time_dilation_settings;
extern TunedFloatProperty g_tuning_time_dilation;
extern uint32_t g_time_dilation_refresh_ticks;
extern bool g_time_dilation_warning_logged;

// Never re-enable a run after changing assisted settings back to 100%.
// Only a *new* run, observed at button zero with vanilla settings, clears it.
extern bool g_recording_tainted;
extern bool g_restart_pending_clean_taint;


extern MyCharacter* g_player;
// Horizontal character movement velocity in Unreal units per second (cm/s).
// Updated on the character tick; HUD reads the cached value, never a stale actor.
extern float g_speed_units_per_second;
extern Vec3 g_position, g_saved_position;
extern Rotator g_saved_rotation;

// The engine timer is based on the run's native LevelState clock.
// QPC fallback is distinctly labelled as 'SESSION' to avoid claiming game-time precision.
extern bool g_engine_timer_valid;
extern bool g_timer_complete;
extern double g_elapsed_seconds;
extern LARGE_INTEGER g_frequency, g_session_start;

// Keyboard-only in-game menu: no mouse dependencies.
enum class Page {
    Main, RestartOptions, Movement, GameplayRecords, Practice, Hud,
    HudPositions, SplitDisplay, Settings, Hotkeys, Profiles, SaveStates,
    HudTiming, HudRecordTimes, HudGameplay,
    MovementModes, MovementPhysics, MovementJump,
    SavePositions, SavedCheckpoints, ButtonCheckpoints, TeleportCoordinates, World, WorldPlayer, WorldSections,
    WorldIndividualSections, WorldButtons, ConfirmLevelReload, WorldTiles, WorldWalls,
    WorldSectionHeights, WorldIndividualSectionHeights, WorldButtonColours,
    WorldButtonBeacons,
    // Menu-only grouping pages. Append to preserve the existing numeric values.
    WorldButtonActions, WorldTileActions, WorldTileColours,
    HudComparisons, Persistence, WorldTubes, WorldTubeColours, WorldTubeSpeed,
    WorldSectionSpeed, WorldSectionPositionEdit,
    WorldPlatforms, WorldPlatformColours, WorldPlatformSpeed,
    WorldJumpPads, WorldJumpPadColours, WorldJumpPadHeight,
    WorldWater, WorldWaterColours,
    // Append to keep all historical Page values stable for existing menu state.
    Customisation, WorldCubes, CustomisationTime, Maps,
    HudAppearance, HudAppearanceEdit, HudKeyDisplay, Search, CustomisationSky,
    CustomisationPillars, WorldObjectRemoval,
    // Sky submenus, appended to preserve all historical Page values.
    CustomisationSkyColours, CustomisationSkyVisibility,
    CustomisationFogColours, CustomisationFogVisibility,
    CustomisationShadows,
    // PB ghost controls live in their own top-level menu. Append to retain legacy IDs.
    Ghost, GhostReplay,
    // Hotkey submenu pages appended; existing page IDs stay stable.
    HotkeysGeneral, HotkeysMovement, HotkeysPractice,
    HotkeysHUD, HotkeysWorld, HotkeysGhost,
    // Dedicated split settings, appended to preserve every existing Page ID.
    Splits, SplitHud, SplitHudAppearance,
    // Independent cosmetic profile management; keep historic enum IDs stable.
    CustomisationProfiles, ConfirmDeleteCustomisationProfile,
    WaterObjectReflections, WaterBeamReflections, WaterReflections,
    HudProfiles, ConfirmDeleteHudProfile,
    // Shared profile hub; append to retain all existing page IDs.
    ProfilesHub, WorldProfiles, ConfirmDeleteWorldProfile,
    HotkeyProfiles, ConfirmDeleteHotkeyProfile, Logs, Crosshair, ConfirmClearLogs
};
enum class NameEditMode { None, Create, Rename, CustomCreate, CustomRename, HudCreate, HudRename, WorldCreate, WorldRename, HotkeyCreate, HotkeyRename };
extern NameEditMode g_name_edit_mode;
extern std::atomic_bool g_name_editing;
extern std::wstring g_name_buffer;

// Button counts are native Refunct LevelState.buttons values. We detect rising
// edges on the game thread; no arbitrary frame clock or synthetic split times.
struct ButtonSplit {
    int button_number = 0;
    double run_seconds = 0.0;
    double segment_seconds = 0.0;
    // Compare against the record before this split was saved, including
    // when the current split establishes a new fastest segment.
    double comparison_best_seconds = -1.0;
    // The actual gold record before this run's split was saved, excluding the
    // separate fallback calculated from PB cumulative button timestamps.
    double previous_recorded_gold_seconds = -1.0;
    // First legitimately saved best for this button had no earlier time to
    // compare against. Show its real duration instead of a fake +/- 0.00.
    bool first_best_without_reference = false;
    bool source_is_practice = false;
    // Snapshot the CURRENT button's historical cumulative PB at capture time.
    // The finish handler may overwrite g_pb before the popup is drawn.
    double cumulative_pb_reference_seconds = -1.0;
    // Snapshot the signed change in cumulative PB difference for this button.
    // NaN when a valid consecutive full-run split/PB reference is missing.
    double pb_gain_loss_seconds = std::numeric_limits<double>::quiet_NaN();
    // Snapshot before FinishPersonalBestIfNeeded() can replace the old PB.
    // Never substitute this run's new PB for its historical reference.
    double section_pb_reference_seconds = -1.0;
    // Historical sum-of-best durations, used ONLY as an explicitly labelled
    // fallback when the completed run PB has no section time.
    double section_best_reference_seconds = -1.0;
    double section_actual_seconds = -1.0;
    // Snapshot the comparison boundary: loading a new section later must not
    // change what a still-visible popup is comparing.
    SectionBoundary section_comparison_boundary{};
    // Intermediate buttons (e.g. 18.1) compare their own segment to the PB
    // button duration, while the final button compares the complete section.
    bool section_is_partial_button = false;
    // A completed section has its own popup lifetime. Intermediate button
    // events in the next section must not erase the previous section delta.
    ULONGLONG section_popup_expires = 0;
};

// Refunct TAS defines 37 buttons for the original map. Treat personal-best
// splits as cumulative times: never confuse individual fastest segments with
// splits belonging to a real completed run.
constexpr int kRefunctButtonCount = 37;
static_assert(kComparisonButtonCount == static_cast<size_t>(kRefunctButtonCount));
// One-based internal button ordinals are stable save/PB keys. Display names
// follow the 31 TAS level clusters: a multi-button cluster uses .1/.2/.3.
// See refunct-tas/tool/teleport.re (BUTTONS) and tool/practice.re (18.2).
static constexpr std::array<const wchar_t*, kRefunctButtonCount> kButtonNames{{
    L"1", L"2", L"3", L"4", L"5", L"6", L"7.1", L"7.2",
    L"8", L"9", L"10.1", L"10.2", L"11", L"12", L"13", L"14",
    L"15", L"16", L"17", L"18.1", L"18.2", L"19", L"20", L"21",
    L"22", L"23", L"24", L"25", L"26.1", L"26.2", L"26.3", L"27",
    L"28.1", L"28.2", L"29", L"30", L"31"
}};


enum class SegmentSource : uint8_t { Unknown, Run, Practice };

struct PersonalBest {
    double total_seconds = -1.0;
    // 0 means there is no automatically captured incomplete-run reference.
    // 1..36 means ButtonSplits/SectionSplits hold the furthest eligible run
    // until a completed PB replaces them. BestSegments remain independent.
    int incomplete_reached = 0;
    // Optional category-specific minimums. 0 disables each requirement.
    int minimum_buttons = 0;
    int minimum_cubes = 0;
    int minimum_platforms = 0;
    std::array<double, kRefunctButtonCount> button_seconds{};
    // Full-section durations from the SAME run PB (not gold segment sums).
    // Persist independently so multi-button sections do not depend on every
    // intermediate button event being present in the older PB file.
    std::array<double, kSectionEndButtons.size()> section_seconds{};
    // Independent best individual segments (duration from the previous
    // button; first segment runs from the beginning to button 1).
    // These records can be improved by legitimate runs or section practice.
    std::array<double, kRefunctButtonCount> best_segment_seconds{};
    std::array<SegmentSource, kRefunctButtonCount> best_segment_sources{};
    PersonalBest() {
        button_seconds.fill(-1.0);
        section_seconds.fill(-1.0);
        best_segment_seconds.fill(-1.0);
        best_segment_sources.fill(SegmentSource::Unknown);
    }
};
extern PersonalBest g_pb;
extern PersonalBest g_compare_pb;  // Read-only reference; never saved as active PB.
extern bool g_compare_reference_loaded;
extern bool g_run_started_at_zero;
extern bool g_run_pb_eligible;
extern bool g_run_finish_processed;
extern ULONGLONG g_pb_popup_expires;
extern std::wstring g_pb_popup_text;
extern std::vector<ButtonSplit> g_button_splits;
extern bool g_button_state_initialized;
extern int g_last_button_count;
extern double g_last_run_start;
extern double g_last_run_time;
extern ULONGLONG g_split_popup_expires;

// Section practice replays button progression using the actual game physics,
// rather than forging level counters. The coordinates are from Refunct TAS.
// This mode never writes an official personal-best record.
enum class PracticePhase {
    Idle, AwaitRestart, Warmup, MoveWaypoint, WaitForButton, PlaceTarget, Stabilize, WaitForTargetButton, Ready, Failed
};
extern PracticePhase g_practice_phase;
extern bool g_practice_active;
extern bool g_practice_restart_requested;
extern int g_practice_selected_button;
extern int g_practice_target_button;
extern int g_practice_completed_buttons;
// These BP_LevelRoot_C speed properties control the level rise animation.
// Saving and restoring the original values is essential: ordinary runs must
// retain vanilla timing after section practice finishes or is cancelled.
constexpr int kPracticeClusterCount = 31;
constexpr float kPracticeFastRiseSpeed = 999999999.0f; // From refunct-tas/tool/practice.re
constexpr int kPracticeSpawnHoldFrames = 18;
extern std::array<void*, kPracticeClusterCount> g_practice_level_actors;
extern std::array<float, kPracticeClusterCount> g_practice_original_speeds;
extern void* g_practice_level_class;
extern int32_t g_practice_speed_offset;
extern bool g_practice_speeds_overridden;
extern bool g_practice_fast_rise_attempted;
extern bool g_practice_fast_rise_succeeded;
extern Vec3 g_practice_spawn;
extern int g_practice_waypoint;
extern std::array<void*, kRefunctButtonCount> g_practice_button_actors;
extern bool g_practice_button_actors_ready;
extern int g_practice_frames_left;
extern int g_practice_retries;
extern ULONGLONG g_practice_step_started;
extern ULONGLONG g_practice_started;

// The practice loader is also the checkpoint restoration engine. A checkpoint
// represents sequentially activated buttons plus an exact player pose.
enum class PracticeOrigin { Section, SavedState, LastButton, ProgressRestart };
extern PracticeOrigin g_practice_origin;
extern bool g_practice_restore_rotation;
// For progress restart, record the *fresh* original spawn only after
// Refunct has completed the native reset. Do not guess spawn coordinates.
extern bool g_practice_capture_reset_spawn;
extern int g_practice_resets_at_request;
extern Rotator g_practice_spawn_rotation;
// Section selection and B replay the chosen button after the teleport at
// vanilla rise speed. F1 does not: it restores the exact saved progression.
extern int g_practice_animate_button;
// Practice golds start only after a checkpoint has reached Ready and its
// selected button is registered. Native setup presses and F1's partial
// checkpoint restore never contribute best-segment records.
extern int g_practice_gold_anchor_button;
extern double g_practice_gold_anchor_seconds;
// Independent HUD timing start for a loaded section. A section loader may
// automatically activate its first button; only the remaining played part
// is comparable to the matching PB button interval.
extern int g_practice_section_anchor_button;
extern double g_practice_section_anchor_seconds;

struct Checkpoint {
    int buttons = 0;
    Vec3 location{};
    Rotator rotation{};
};
extern Checkpoint g_saved_checkpoint;
extern Checkpoint g_last_button_checkpoint;
extern bool g_saved_checkpoint_valid;
extern bool g_last_button_checkpoint_valid;
extern std::wstring g_checkpoint_path;
// Independent native button observer; artificial setup presses aren't captured.
extern int g_observed_buttons;
extern int g_observed_resets;

extern Page g_page;
extern int g_selection;
extern int g_rebind_action;
extern std::wstring g_notice;
extern ULONGLONG g_notice_expires;


struct SumOfBest {
    double seconds = 0.0;
    int recorded = 0;
};
struct GoldUpdate {
    double previous_best = -1.0;
    double current_best = -1.0;
    bool new_gold = false;
};


} // namespace refunct

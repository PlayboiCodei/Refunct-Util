#include "refunct/internal_api.hpp"
#include "refunct/features/maps.hpp"


namespace refunct {
void RestorePracticeClusterSpeeds() {
    if (!g_practice_speeds_overridden) return;
    int restored = 0;
    for (int i = 0; i < kPracticeClusterCount; ++i) {
        void* actor = g_practice_level_actors[i];
        if (!actor || g_practice_speed_offset < 0 ||
            !IsReadable(actor, sizeof(UObjectHeader)) ||
            static_cast<const UObjectHeader*>(actor)->class_ptr != g_practice_level_class)
            continue;
        auto* speed = reinterpret_cast<float*>(static_cast<uint8_t*>(actor) + g_practice_speed_offset);
        if (!IsReadable(speed, sizeof(*speed))) continue;
        *speed = g_practice_original_speeds[i];
        ++restored;
    }
    g_practice_speeds_overridden = false;
    Log((L"Section practice: restored level rise speeds (" +
         std::to_wstring(restored) + L"/31)").c_str());
}

// The original TAS speeds up all 31 clusters to skip their slow rise animation.
// Never apply partially: a failed layout check must leave the map untouched.
bool AcceleratePracticeClusters() {
    if (g_practice_speeds_overridden) return true;
    if (g_practice_speed_offset < 0 || !g_practice_level_class) return false;
    std::array<float*, kPracticeClusterCount> properties{};
    for (int i = 0; i < kPracticeClusterCount; ++i) {
        void* actor = g_practice_level_actors[i];
        if (!actor || !IsReadable(actor, sizeof(UObjectHeader)) ||
            static_cast<const UObjectHeader*>(actor)->class_ptr != g_practice_level_class)
            return false;
        auto* speed = reinterpret_cast<float*>(static_cast<uint8_t*>(actor) + g_practice_speed_offset);
        if (!IsReadable(speed, sizeof(*speed)) || !std::isfinite(*speed) ||
            *speed <= 0.0f || *speed >= 10000000.0f) return false;
        properties[i] = speed;
        g_practice_original_speeds[i] = *speed;
    }
    for (float* speed : properties) *speed = kPracticeFastRiseSpeed;
    g_practice_speeds_overridden = true;
    Log(L"Section practice: accelerated all 31 level rise animations");
    return true;
}

// These native helpers are defined later, after Refunct's object reflection.
bool DiscoverPracticeButtons();
const LevelState* PracticeLevelState();

// Cancel only the transient fast-speed override; preserve the assisted-run
// flag until a genuinely vanilla restart. Never leave accelerated speeds on
// actors when entering an unrelated section load or preloaded restart.
void CancelNormalFastRise() {
    if (!g_normal_fast_rise_active) return;
    RestorePracticeClusterSpeeds();
    g_normal_fast_rise_active = false;
    g_normal_fast_rise_reset_seen = false;
    g_normal_fast_rise_base_resets = -1;
    g_normal_fast_rise_restore_at = 0;
}

// Use the same level-root Speed property and verified 31-actor scan as the
// existing TAS-derived checkpoint loader. The result is visual fast-rise;
// normal restart still uses the game's own RestartGame and buttons stay zero.
bool BeginNormalFastRise() {
    if (g_progress_rise_animation_enabled || g_normal_fast_rise_active ||
        g_practice_active || g_preloaded_restart_armed || g_preloaded_reset_seen ||
        g_preloaded_restart_active || g_progress_restart_active) return false;
    if (!DiscoverPracticeButtons() || !AcceleratePracticeClusters()) {
        Log(L"Normal restart: fast rise unavailable; original animation retained");
        return false;
    }
    g_normal_fast_rise_active = true;
    g_normal_fast_rise_reset_seen = false;
    g_normal_fast_rise_base_resets = -1;
    if (const LevelState* state = PracticeLevelState())
        g_normal_fast_rise_base_resets = state->resets;
    g_normal_fast_rise_started_at = GetTickCount64();
    g_normal_fast_rise_restore_at = 0;
    g_normal_fast_restart_assisted = true;
    g_recording_tainted = true;
    g_run_pb_eligible = false;
    Log(L"Normal restart: TAS fast-rise speeds enabled; records disabled for this run");
    return true;
}

// Give the native restart enough time to start raising its first platforms
// before restoring their original speed. A timeout restores safely even if
// Refunct does not publish a reset-counter transition.
void UpdateNormalFastRise() {
    if (!g_normal_fast_rise_active) return;
    const ULONGLONG now = GetTickCount64();
    if (!g_normal_fast_rise_reset_seen) {
        if (const LevelState* state = PracticeLevelState();
            state && g_normal_fast_rise_base_resets >= 0 &&
            state->resets != g_normal_fast_rise_base_resets) {
            g_normal_fast_rise_reset_seen = true;
            g_normal_fast_rise_restore_at = now + 600;
        }
    }
    if ((g_normal_fast_rise_reset_seen && now >= g_normal_fast_rise_restore_at) ||
        now - g_normal_fast_rise_started_at > 10000) {
        if (!g_normal_fast_rise_reset_seen)
            Log(L"WARNING: normal fast-rise reset event not confirmed; restoring actor speeds");
        CancelNormalFastRise();
    }
}

// Refunct TAS uses UMyGameInstance::RestartGame(false).
// Reimplement a narrow, defensive lookup of that UFunction using its original UE4 layout.
void* GetGameInstance() {
    if (!g_game_module) return nullptr;
    auto* world_slot = reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(g_game_module) + kGlobalWorldRva);
    if (!IsReadable(world_slot, sizeof(void*))) return nullptr;
    void* world = *world_slot;
    if (!IsReadable(world, 0xC0 + sizeof(void*))) return nullptr;
    auto* instance_slot = reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(world) + 0xC0);
    void* instance = *instance_slot;
    return IsReadable(instance, sizeof(UObjectHeader)) ? instance : nullptr;
}

bool RestartRun(MyCharacter* player) {
    if (!g_process_event) { Notify(L"Restart unavailable"); return false; }
    void* instance = GetGameInstance();
    if (!instance) { Notify(L"Restart unavailable: no GameInstance"); return false; }
    void* function = FindRestartFunction(instance);
    if (!function) {
        Notify(L"RestartGame not found - see log");
        Log(L"ERROR: reflected RestartGame function not found");
        return false;
    }
    // 'Reset=false' is the default zero-initialised argument used by refunct-tas.
    std::array<uint8_t, 64> params{};
    // RestartGame may recreate the player. Preserve requested flight modes only
    // under the map-restart policy; all other movement values live in globals.
    if (g_keep_movement_on_map_restart) {
        g_restore_flying_pending |= g_flying;
        g_restore_noclip_pending |= g_noclip;
    }
    if (player && IsReadable(player->movement, sizeof(MovementComponent))) SetFlying(player, false);
    g_saved_valid = false;
    g_button_splits.clear();
    g_button_state_initialized = false;
    g_run_started_at_zero = false;
    g_run_pb_eligible = false;
    g_run_finish_processed = false;
    g_split_popup_expires = 0;
    g_practice_gold_anchor_button = 0;
    g_practice_gold_anchor_seconds = -1.0;
    g_process_event(instance, function, params.data());
    // Don't clear taint here: the reset might fail or occur asynchronously.
    // Wait for an observed clean button-zero state.
    g_restart_pending_clean_taint = true;
    g_menu_open.store(false);
    // Replay-driven restart should keep F5 pointed at its playback controls.
    if (!IsGhostReplayRequested() && g_page != Page::GhostReplay) SetPage(Page::Main);
    // A restart may replace the character. Do not dereference player after ProcessEvent.
    return true;
}

namespace {
// This is a UE4 map travel, not a process restart. The GameInstance and ASI
// stay loaded; only the UWorld and its level actors should be recreated.
void* s_reload_original_world = nullptr;
ULONGLONG s_reload_started_ms = 0;

void* CurrentWorld() {
    if (!g_game_module) return nullptr;
    const auto address = reinterpret_cast<uintptr_t>(g_game_module) + kGlobalWorldRva;
    const auto* slot = reinterpret_cast<void* const*>(address);
    if (!IsReadable(slot, sizeof(void*))) return nullptr;
    void* world = *slot;
    if (!IsReadable(world, sizeof(UObjectHeader))) return nullptr;
    const auto* header = static_cast<const UObjectHeader*>(world);
    if (!IsReadable(header->class_ptr, sizeof(UObjectHeader))) return nullptr;
    return UnrealName(static_cast<const UObjectHeader*>(header->class_ptr)->name) == L"World"
        ? world : nullptr;
}

// Native OpenLevel is a static UGameplayStatics UFunction. A call via the
// GameplayStatics class-default object avoids new fixed executable RVAs.
void* FindGameplayStaticsDefaultObject() {
    if (!g_game_module) return nullptr;
    const auto address = reinterpret_cast<uintptr_t>(g_game_module) + kGlobalObjectArrayRva;
    const auto* array = reinterpret_cast<const GlobalObjectArray*>(address);
    if (!IsReadable(array, sizeof(*array)) || !array->objects ||
        array->num_elements <= 0 || array->num_elements > 500000 ||
        array->max_elements < array->num_elements ||
        !IsReadable(array->objects,
            sizeof(GlobalObjectItem) * static_cast<size_t>(array->num_elements))) return nullptr;
    for (int i = 0; i < array->num_elements; ++i) {
        const auto& item = array->objects[i];
        if (!item.object || (static_cast<uint32_t>(item.flags) & 0x30000000u) ||
            !IsReadable(item.object, sizeof(UObjectHeader))) continue;
        const auto* object = item.object;
        if (UnrealName(object->name) != L"Default__GameplayStatics") continue;
        if (!IsReadable(object->class_ptr, sizeof(UObjectHeader))) continue;
        if (UnrealName(static_cast<const UObjectHeader*>(object->class_ptr)->name) ==
            L"GameplayStatics") return item.object;
    }
    return nullptr;
}

// The native function's parameter struct is checked at runtime, before any
// memory is written or the world is unloaded. Offsets are the Refunct Win32
// UE4 layout: UObject* 0, FName 4, bool 12, FString 16.
bool HasOpenLevelParameter(void* function, const wchar_t* name,
                           const wchar_t* type, int32_t offset) {
    if (!IsReadable(function, sizeof(UStructHeader))) return false;
    auto* field = static_cast<const UStructHeader*>(function)->children;
    for (int i = 0; i < 128 && IsReadable(field, sizeof(UFieldHeader)); ++i) {
        const auto* item = static_cast<const UFieldHeader*>(field);
        if (UnrealName(item->object.name) == name) {
            if (!IsReadable(item->object.class_ptr, sizeof(UObjectHeader)) ||
                UnrealName(static_cast<const UObjectHeader*>(item->object.class_ptr)->name)
                    != type) return false;
            constexpr size_t kOffsetInternal = 0x3C;
            if (!IsReadable(item, kOffsetInternal + sizeof(int32_t))) return false;
            const auto actual = *reinterpret_cast<const int32_t*>(
                static_cast<const uint8_t*>(static_cast<const void*>(item)) +
                kOffsetInternal);
            return actual == offset;
        }
        field = item->next;
    }
    return false;
}

bool ValidLevelName(const std::wstring& name) {
    if (name.empty() || name.size() > 100 || name == L"None" ||
        name == L"Transient") return false;
    for (wchar_t c : name) {
        if (!((c >= L'A' && c <= L'Z') ||
              (c >= L'a' && c <= L'z') ||
              (c >= L'0' && c <= L'9') || c == L'_')) return false;
    }
    return true;
}
} // namespace

bool RequestFullLevelReload() {
    if (g_full_level_reload_requested || s_reload_original_world) {
        Notify(L"Level reload already in progress");
        return false;
    }
    if (!g_process_event || !CurrentWorld()) {
        Notify(L"Level reload unavailable: gameplay world not loaded");
        return false;
    }
    // Called from the menu during character input; defer until *after* the
    // original native character tick returns to avoid using a destroyed actor.
    g_full_level_reload_requested = true;
    Notify(L"Reloading level without closing Refunct");
    return true;
}

bool ExecuteFullLevelReload(MyCharacter* player) {
    g_full_level_reload_requested = false;
    void* world = CurrentWorld();
    void* instance = GetGameInstance();
    if (!world || !g_process_event || !instance || !FindRestartFunction(instance)) {
        Notify(L"Level reload unavailable: native run reset missing");
        OnCustomMapFullLevelReloadResult(false);
        return false;
    }
    const FNameView map_name = static_cast<const UObjectHeader*>(world)->name;
    const std::wstring readable_name = UnrealName(map_name);
    if (!ValidLevelName(readable_name)) {
        Log(L"Full level reload: current UWorld has an unsupported map name");
        Notify(L"Full level reload unavailable: cannot identify current level");
        OnCustomMapFullLevelReloadResult(false);
        return false;
    }
    void* statics = FindGameplayStaticsDefaultObject();
    void* function = statics ? FindReflectedFunction(statics, L"OpenLevel") : nullptr;
    if (!function ||
        !HasOpenLevelParameter(function, L"WorldContextObject", L"ObjectProperty", 0) ||
        !HasOpenLevelParameter(function, L"LevelName", L"NameProperty", 4) ||
        !HasOpenLevelParameter(function, L"bAbsolute", L"BoolProperty", 12) ||
        !HasOpenLevelParameter(function, L"Options", L"StrProperty", 16)) {
        Log(L"Full level reload: missing GameplayStatics::OpenLevel or incompatible reflected signature");
        Notify(L"Full level reload unsupported by this game build (see log)");
        OnCustomMapFullLevelReloadResult(false);
        return false;
    }
    // Replacing the map invalidates actors, even though the ASI stays loaded.
    // Preserve user settings, split profiles, saved PBs and the GameInstance.
    CancelNormalFastRise();
    RestorePracticeClusterSpeeds();
    ResetMovementOnNormalRestart(player);
    // LevelState lives in GameInstance, which OpenLevel does NOT recreate.
    // Reset it through Refunct's proven native path before travelling; do
    // not touch the outgoing player after RestartRun's ProcessEvent call.
    if (!RestartRun(player)) {
        Notify(L"Full level reload aborted: native run reset failed");
        OnCustomMapFullLevelReloadResult(false);
        return false;
    }
    world = CurrentWorld();
    if (!world) {
        Log(L"Full level reload: native run reset left no active UWorld");
        Notify(L"Run reset completed, but level reload could not start");
        OnCustomMapFullLevelReloadResult(false);
        return false;
    }
    ClearButtonHistoryOnRestart();
    g_practice_active = false;
    g_practice_phase = PracticePhase::Idle;
    g_practice_restart_requested = false;
    g_preloaded_restart_requested = false;
    g_preloaded_restart_armed = false;
    g_preloaded_restart_active = false;
    g_preloaded_restart_confirmed = false;
    g_preloaded_restart_failed = false;
    g_preloaded_reset_seen = false;
    g_progress_restart_active = false;
    g_progress_restart_pending = false;
    g_normal_fast_restart_assisted = false;
    g_restart_requested = false;
    g_practice_button_actors_ready = false;
    g_practice_button_actors.fill(nullptr);
    g_practice_level_actors.fill(nullptr);
    g_practice_level_class = nullptr;
    g_practice_speed_offset = -1;
    ResetSectionPlacementInternals();
    g_saved_valid = false;
    g_button_splits.clear();
    g_button_state_initialized = false;
    g_observed_buttons = -1;
    g_observed_resets = -1;
    g_run_started_at_zero = false;
    g_run_pb_eligible = false;
    g_run_finish_processed = false;
    g_normal_fast_rise_active = false;
    g_split_popup_expires = 0;
    g_pb_popup_expires = 0;
    g_pb_popup_text.clear();
    g_progress_counters = {};
    g_game_data_valid = false;
    g_engine_timer_valid = false;
    g_timer_complete = false;
    g_elapsed_seconds = 0.0;
    g_practice_gold_anchor_button = 0;
    g_practice_gold_anchor_seconds = -1.0;
    g_restart_pending_clean_taint = true;
    ResetWorldSpeedSampling();
    OnCustomMapFullLevelReloadStart();
    QueryPerformanceCounter(&g_session_start);
    g_menu_open.store(false);
    SetPage(Page::Main);

    // Calling OpenLevel schedules a UE4 map travel. This is NOT a Win32
    // CloseWindow / CreateProcess operation. A fresh UWorld appears afterwards.
    alignas(8) std::array<uint8_t, 64> params{};
    std::memcpy(params.data(), &world, sizeof(world));
    std::memcpy(params.data() + 4, &map_name, sizeof(map_name));
    params[12] = 1;  // bAbsolute; empty Options string is already zero-filled
    s_reload_original_world = world;
    s_reload_started_ms = GetTickCount64();
    Log((L"Full level reload: OpenLevel requested for " + readable_name +
         L" (process remains alive)").c_str());
    g_process_event(statics, function, params.data());
    // OpenLevel may have already destroyed player. Do not read its fields.
    g_player = nullptr;
    return true;
}

void CheckFullLevelReloadCompletion() {
    if (!s_reload_original_world) return;
    void* world = CurrentWorld();
    if (world && world != s_reload_original_world) {
        if (const LevelState* state = PracticeLevelState();
            state && state->buttons != 0) {
            Log(L"Full level reload: new world exists but Refunct progress is not zero");
            Notify(L"Level reloaded, but run progress may need resetting");
        } else {
            Log(L"Full level reload: fresh UWorld observed; game process unchanged");
        }
        s_reload_original_world = nullptr;
        s_reload_started_ms = 0;
        OnCustomMapFullLevelReloadResult(true);
        return;
    }
    const ULONGLONG now = GetTickCount64();
    if (now >= s_reload_started_ms && now - s_reload_started_ms > 15000) {
        Log(L"Full level reload: timed out waiting for a new UWorld");
        Notify(L"Full level reload not confirmed (see log)");
        OnCustomMapFullLevelReloadResult(false);
        s_reload_original_world = nullptr;
        s_reload_started_ms = 0;
    }
}

// Refunct TAS uses these physical button coordinates for teleport practice.

} // namespace refunct

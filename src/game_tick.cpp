#include "refunct/internal_api.hpp"
#include "refunct/performance.hpp"
#include "refunct/colour_work_budget.hpp"

namespace refunct {
namespace {
// Moving pipes/platforms can translate the character without updating
// CharacterMovement::Velocity. Sample world-space travel as a HUD fallback.
// This is read-only; it never modifies the player's motion or pipe actors.
struct SpeedSample {
    MyCharacter* player = nullptr;
    Vec3 previous_location{};
    bool has_previous = false;
};

SpeedSample s_speed_sample{};

float SampleWorldSpeed(MyCharacter* player, const Vec3& location, float dt) {
    const bool position_valid = std::isfinite(location.x) &&
        std::isfinite(location.y) && std::isfinite(location.z);
    if (s_speed_sample.player != player) {
        s_speed_sample = {};
        s_speed_sample.player = player;
    }

    float speed = 0.0f;
    if (position_valid && s_speed_sample.has_previous &&
        std::isfinite(dt) && dt > 0.0001f && dt <= 0.2f) {
        // Use doubles to avoid overflowing intermediate squared deltas.
        const double dx = static_cast<double>(location.x) - s_speed_sample.previous_location.x;
        const double dy = static_cast<double>(location.y) - s_speed_sample.previous_location.y;
        const double dz = static_cast<double>(location.z) - s_speed_sample.previous_location.z;
        const double displacement = std::sqrt(dx * dx + dy * dy + dz * dz);
        const double units_per_second = displacement / dt;
        // A teleport/checkpoint restore isn't pipe travel. Discard the
        // entire discontinuity rather than displaying a fake speed spike.
        constexpr double kMaximumPlausibleWorldSpeed = 30000.0;
        if (std::isfinite(units_per_second) &&
            units_per_second <= kMaximumPlausibleWorldSpeed)
            speed = static_cast<float>(units_per_second);
    }

    s_speed_sample.previous_location = location;
    s_speed_sample.has_previous = position_valid;
    return speed;
}
}  // namespace

// A teleport invalidates the previous position sample. Reset before the
// next HUD update so the jump is not mistaken for real movement speed.
void ResetWorldSpeedSampling() { s_speed_sample = {}; }

void __fastcall CharacterTickHook(MyCharacter* player, void* /*edx*/, float dt) {
    PerformanceScope perf_tick(PerfArea::Tick);
    // A native restart may replace the character before the map is ready.
    // Apply World defaults only after the new actor survives the native tick;
    // otherwise the reset could dereference the previous (destroyed) actor.
    const bool native_world_restart = g_player && g_player != player &&
        !g_practice_active;
    if (g_player != player) {
        // Refunct can recreate the character when restarting through its own
        // UI, bypassing the utility's R hotkey. Do not carry cheats into the
        // fresh character. Practice checkpoint reloads are exempt.
        if (g_player && !g_practice_active) {
            if (g_keep_movement_on_map_restart) {
                g_restore_flying_pending |= g_flying;
                g_restore_noclip_pending |= g_noclip;
                if (g_restore_noclip_pending) g_restore_flying_pending = true;
                ResetJumpAssistState();
                ResetAirMomentum();
            } else {
                const bool had_modes = g_flying || g_noclip ||
                    g_restore_flying_pending || g_restore_noclip_pending;
                g_restore_flying_pending = false;
                g_restore_noclip_pending = false;
                if (ResetMovementConfiguration() || had_modes) SaveSettings();
            }
        }
        ResetPBGhostCapture();
        ResetCharacter(player);
    }
    PollHudColourPicker(); // Apply picked HUD RGB even if the F5 menu was closed.
    const bool valid = player && IsReadable(player, sizeof(*player)) &&
        IsReadable(player->root_component, sizeof(SceneComponent)) &&
        IsReadable(player->controller, sizeof(PlayerController)) &&
        IsReadable(player->movement, sizeof(MovementComponent));

    if (valid) {
        // No key press: one atomic load, not 247 locked exchanges. Exchange the
        // dirty flag BEFORE draining, so concurrent arrivals schedule another pass.
        if (g_pending_any.load(std::memory_order_acquire) &&
            g_pending_any.exchange(false, std::memory_order_acq_rel)) {
            for (int key = 8; key < 255; ++key) {
                if (g_pending[key].load(std::memory_order_relaxed) != 0 &&
                    g_pending[key].exchange(0, std::memory_order_relaxed) != 0) ProcessKey(key, player);
            }
        }
        DrainMenuSearchKeys(player);
        // Optional Win32 mouse observer, installed on the game window's own
        // message thread rather than detouring Unreal's mouse function.
        UpdateMouseWheelHook();
        UpdateJumpAssist(player);
        { PerformanceScope perf(PerfArea::WaterSwimming);
          UpdateWaterSwimming(player); } // Prevent swimming before native physics tick.
        if (g_flying) {
            MaintainFlyingState(player);
            // F5 is an overlay; keep Space (up) / Shift (down) usable while
            // browsing menus. Don't move while capturing/editing text inputs.
            const bool move = HasFocusedGameWindow() &&
                !g_rebinding.load(std::memory_order_relaxed) &&
                !g_name_editing.load(std::memory_order_relaxed) &&
                !g_coordinate_editing.load(std::memory_order_relaxed) &&
                !(g_menu_open.load(std::memory_order_relaxed) && g_page == Page::Search) &&
                !IsGhostReplayFreeCameraActive(); // The replay spectator owns these keys.
            const float up = move && (GetAsyncKeyState(VK_SPACE) & 0x8000) ? 1.0f : 0.0f;
            const float down = move && (GetAsyncKeyState(VK_SHIFT) & 0x8000) ? 1.0f : 0.0f;
            player->movement->velocity.z = (up - down) * g_fly_speed;
        }
    }

    { PerformanceScope perf(PerfArea::NativeTick);
      if (g_original_tick) g_original_tick(player, dt); }
    // Original UE4 tick can replace/destroy actor components during a restart.
    // Recheck after it returns rather than dereferencing a stale pre-tick view.
    const bool post_tick_valid = valid &&
        IsReadable(player, sizeof(*player)) &&
        IsReadable(player->root_component, sizeof(SceneComponent)) &&
        IsReadable(player->controller, sizeof(PlayerController)) &&
        IsReadable(player->movement, sizeof(MovementComponent));
    if (post_tick_valid) {
        // Independent cosmetic queues previously stacked their 0.5-1.1 ms
        // budgets on the same tick. Share one adaptive allowance instead.
        // Only ColourWorkBudget scopes are charged; native gameplay and
        // jump-pad detection continue at their original tick frequency.
        ScopedColourFrameBudget cosmetic_frame(dt);
        ApplyWorldGameLaunchPersistence();
        if (native_world_restart) ResetWorldOnNormalRestart();
        // Refunct's own character tick may recalculate its movement values.
        // Apply after that tick, before the next physics update.
        { PerformanceScope perf(PerfArea::Movement);
        ApplyMovementTuning(player); }
        { PerformanceScope perf(PerfArea::WaterSwimming);
          UpdateWaterSwimming(player); } // Native tick may have re-entered swimming.
        ApplyPendingAirJump(player);
        ApplyGameSpeed();
        UpdateSlideTuning(player, dt);
        UpdateAirMomentum(player);
        MaintainFlyingState(player);
        const Vec3 velocity = player->movement->velocity;
        const Vec3 current_position = player->root_component->location;
        const float world_speed = SampleWorldSpeed(player, current_position, dt);
        float horizontal_speed = 0.0f;
        if (std::isfinite(velocity.x) && std::isfinite(velocity.y)) {
            horizontal_speed = std::hypot(velocity.x, velocity.y);
            if (!std::isfinite(horizontal_speed)) horizontal_speed = 0.0f;
        }
        // During pipe travel (or movement on an actor), native velocity can
        // be zero even though the player is moving through world space.
        // The fallback includes Z for vertical pipes; ordinary movement
        // retains the HUD's original horizontal-velocity measurement.
        // Don't show artificial speeds during checkpoint/section placement.
        const bool placing_checkpoint = g_practice_active &&
            g_practice_phase != PracticePhase::Ready;
        g_speed_units_per_second = horizontal_speed < 1.0f &&
            !placing_checkpoint && world_speed >= 1.0f
            ? world_speed : horizontal_speed;
        g_position = current_position;
        g_game_data_valid = std::isfinite(g_position.x) &&
                            std::isfinite(g_position.y) && std::isfinite(g_position.z);
        { PerformanceScope perf(PerfArea::Progress);
        UpdateTimer(player);
        UpdateProgressCounters(); }
        // Process native restarts *before* deciding record eligibility for
        // the first split of a new clean run.
        TrackLastButton(player);
        UpdateNormalFastRise();
        // Flag new cheat settings BEFORE recording the next split/PB.
        UpdateCheatNotifications();
        // Native replay presses must never produce PB/gold records or new
        // ghost samples, regardless of "Save with cheats" preferences.
        if (!IsGhostReplayRequested()) {
            { PerformanceScope perf(PerfArea::Records); UpdateButtonSplits(); }
            { PerformanceScope perf(PerfArea::Ghost); UpdatePBGhostRecording(player); }
        }
        { PerformanceScope perf(PerfArea::Ghost);
        UpdateGhostReplayViewer(player);
        DrawWorldPBGhost(player); } // 3D no-collision ghost; draw before scene rendering.
        // Resolve the effective FOV after replay start/stop and mode changes.
        // Keep the existing Ghost timing scope and leave saved settings alone.
        ApplyPlayerFov(player);
        if (g_practice_active) {
            g_run_pb_eligible = false;
            // Loading produces artificial button events. Hide those only;
            // once Ready, real practice splits should show comparisons.
            if (g_practice_phase != PracticePhase::Ready)
                g_split_popup_expires = 0;
        }
        if (!IsGhostReplayRequested()) { PerformanceScope perf(PerfArea::Records); FinishPersonalBestIfNeeded(); }
        { PerformanceScope perf(PerfArea::Practice);
        UpdatePractice(player);
        UpdateSectionPlacements();
        UpdateCustomMapLoad(); }
        { PerformanceScope perf(PerfArea::WorldRemoval);
        UpdateWorldObjectRemovals(player); }
        { PerformanceScope perf(PerfArea::Cubes);
        UpdateWorldCubes(player); }
        UpdateNativeSectionSpeed();
        { PerformanceScope perf(PerfArea::Tubes);
        UpdateTubeColours(player);
        UpdateTubeTravelSpeed(player); }
        { PerformanceScope perf(PerfArea::Platforms);
        UpdatePlatforms(player); }
        { PerformanceScope perf(PerfArea::Pillars);
        UpdatePillarColours(player); }
        { PerformanceScope perf(PerfArea::JumpPads);
        UpdateJumpPads(player); }
        { PerformanceScope perf(PerfArea::Water);
        UpdateWaterColours(player); }
        { PerformanceScope perf(PerfArea::TimeSkyFogShadows);
        { PerformanceScope detail(PerfArea::TimeOfDay); UpdateCustomTimeOfDay(player); }
        { PerformanceScope detail(PerfArea::Sky); UpdateSkyColours(player); }
        { PerformanceScope detail(PerfArea::Fog); UpdateFogColours(player); }
        { PerformanceScope detail(PerfArea::Shadows); UpdateShadowCustomisation(player); } }
        UpdatePreloadedRestart();
        CheckFullLevelReloadCompletion();
        // Detect changes from menu, hotkeys, and settings loaded at launch.
        UpdateCheatNotifications();
        if (g_full_level_reload_requested) {
            // Map travel can invalidate player immediately; do not execute
            // anything else in this tick after calling OpenLevel.
            ExecuteFullLevelReload(player);
            return;
        }
        if (g_restart_requested) {
            // R is a fresh run, not a replay of the previous button.
            ClearButtonHistoryOnRestart();
            g_observed_buttons = -1;
            g_observed_resets = -1;
            CancelNormalFastRise();
            RestorePracticeClusterSpeeds();
            g_practice_active = false;
            g_practice_phase = PracticePhase::Idle;
            g_practice_restart_requested = false;
            g_restart_requested = false;
            // New run replaces the prior fast-R record gate. The upcoming
            // request re-arms it only when fast rise is actually available.
            g_normal_fast_restart_assisted = false;
            // An ordinary restart exits the non-vanilla preloaded-map mode.
            g_preloaded_restart_active = false;
            g_progress_restart_active = false;
            g_progress_restart_pending = false;
            g_preloaded_restart_confirmed = false;
            g_preloaded_restart_failed = false;
            g_preloaded_restart_armed = false;
            g_preloaded_reset_seen = false;
            g_preloaded_restart_requested = false;
            // Respect the user's map-restart persistence preference.
            // Internal checkpoint reloads still preserve movement tuning.
            ResetMovementOnNormalRestart(player);
            if (!g_progress_rise_animation_enabled && !BeginNormalFastRise())
                Notify(L"Fast rise unavailable; using normal restart animation");
            if (RestartRun(player)) {
                QueryPerformanceCounter(&g_session_start);
                g_game_data_valid = false;
                g_progress_counters.loaded_valid = false;
                g_progress_counters.cached_level = -1;
                return;  // RestartGame may invalidate the player pointer.
            }
            // A failed restart must not leave platform speeds accelerated.
            CancelNormalFastRise();
        }
        if (g_preloaded_restart_requested) {
            g_preloaded_restart_requested = false;
            CancelNormalFastRise();
            g_practice_restart_requested = false;
            g_practice_active = false;
            g_progress_restart_active = false;
            g_progress_restart_pending = false;
            g_practice_phase = PracticePhase::Idle;
            ClearButtonHistoryOnRestart();
            g_observed_buttons = -1;
            g_observed_resets = -1;
            RestorePracticeClusterSpeeds();
            // Respect the map-restart preference, but preloaded-map runs stay
            // permanently ineligible for official PB/fastest-segment records.
            ResetMovementOnNormalRestart(player);
            g_recording_tainted = true;
            g_run_pb_eligible = false;
            g_practice_gold_anchor_button = 0;
            g_practice_gold_anchor_seconds = -1.0;
            g_preloaded_restart_active = true;
            g_preloaded_restart_armed = true;
            g_preloaded_new_game_seen = false;
            g_preloaded_restart_confirmed = false;
            g_preloaded_restart_failed = false;
            g_preloaded_reset_seen = false;
            g_preloaded_base_resets = -1;
            if (const auto* state = PracticeLevelState())
                g_preloaded_base_resets = state->resets;
            g_preloaded_started_at = GetTickCount64();
            g_preloaded_fast_rise_used = DiscoverPracticeButtons() && AcceleratePracticeClusters();
            if (RestartRun(player)) {
                QueryPerformanceCounter(&g_session_start);
                g_game_data_valid = false;
                g_progress_counters.loaded_valid = false;
                g_progress_counters.cached_level = -1;
                return;
            }
            g_preloaded_restart_armed = false;
            g_preloaded_restart_failed = true;
            RestorePracticeClusterSpeeds();
            Notify(L"Preloaded restart failed; records remain disabled");
        }
        if (g_practice_restart_requested) {
            g_practice_restart_requested = false;
            if (g_progress_restart_pending &&
                g_practice_origin == PracticeOrigin::ProgressRestart) {
                // Match the original H restart: respect map-restart movement
                // persistence before RestartGame replaces the player actor.
                g_progress_restart_pending = false;
                ResetMovementOnNormalRestart(player);
            }
            if (RestartRun(player)) {
                // RestartRun clears general run state, not the practice target.
                g_practice_active = true;
                g_practice_phase = PracticePhase::AwaitRestart;
                // Never reuse pre-restart button actor references: the new
                // world may have recreated the corresponding UObjects.
                g_practice_button_actors_ready = false;
                g_practice_started = GetTickCount64();
                QueryPerformanceCounter(&g_session_start);
                g_game_data_valid = false;
                g_progress_counters.loaded_valid = false;
                g_progress_counters.cached_level = -1;
                return;
            }
            PracticeFailed(L"Section practice: could not restart Refunct");
            if (g_practice_origin == PracticeOrigin::ProgressRestart)
                g_progress_restart_active = true; // Keep records blocked on failure.
        }
        RestorePendingMovementModes(player);
        // Execute last-button teleports after native movement, section practice,
        // map reapplication and all restart paths have completed this frame.
        UpdateGlobalLastButtonTeleport(player);
        // Tile contacts are native gameplay events; process at most one per
        // tick after all restart paths have had a chance to return.
        UpdateWorldTileSelection(player);
        UpdateButtonTileSelectionGuard(player);
        { PerformanceScope perf(PerfArea::Tiles);
        UpdateTileColourPresets(player); }
    } else {
        s_speed_sample = {};
        g_game_data_valid = false;
        g_speed_units_per_second = 0.0f;
        g_progress_counters.valid = false;
        // The old world's roots may have been destroyed. Force fresh native
        // actor discovery before using a loaded-only denominator again.
        g_progress_counters.loaded_valid = false;
        g_progress_counters.cached_level = -1;
    }
}


} // namespace refunct

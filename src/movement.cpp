#include "refunct/internal_api.hpp"

namespace refunct {
namespace {
// This is intentionally input-gated: native crouch sliding is part of
// Refunct's character code, not a standard UE4 movement mode. We only touch
// horizontal velocity for a short window after a running Shift press.
// The game executable is required to identify its internal slide state more
// precisely; no guessed UObject offsets or permanent friction overrides.
struct SlideTuningState {
    MyCharacter* character = nullptr;
    bool crouch_was_down = false;
    bool active = false;
    float elapsed = 0.0f;
    float previous_speed = 0.0f;
    Vec3 previous_location{};
};
SlideTuningState s_slide{};

// Only the character tick mutates this state. A request is sampled before the
// native tick and applied after it, so the game's jump code cannot erase it.
struct AirJumpInputState {
    MyCharacter* character = nullptr;
    bool initialized = false;
    bool previous_space_down = false;
    bool pending = false;
    bool warned_missing_velocity = false;
};
AirJumpInputState s_air_jump{};
void ResetAirJumpInputState() { s_air_jump = {}; }
}  // namespace

void ResetSlideTuningState() { s_slide = {}; }

void ResetJumpAssistState() {
    // Never leave a synthetic Space key held when changing settings/restarting.
    if ((g_jump_assist.pulse_pending_release || g_jump_assist.synthetic_climb_held) &&
        g_jump_assist.pressed_window)
        PostMessageW(g_jump_assist.pressed_window, WM_KEYUP, VK_SPACE, kSpaceKeyUpLParam);
    g_wheel_jump_requests.store(0, std::memory_order_relaxed);
    g_jump_assist = {};
    ResetAirJumpInputState();
}
void ResetAirMomentum() {
    g_air_momentum = {};
}

void ResetCharacter(MyCharacter* character) {
    // FOV tracking is reset by ApplyPlayerFov once the new controller is valid.
    g_player = character;
    g_flying = false;
    g_noclip = false;
    g_original_fly_speed = 600.0f;
    // Never dereference movement properties cached from a previous character.
    g_tuning_player = nullptr;
    g_tuning_movement = nullptr;
    g_tuning_max_walk_speed = {};
    g_tuning_max_swim_speed = {};
    g_tuning_climb_speed = {};
    g_tuning_base_speed = {};
    g_tuning_bonus_speed = {};
    g_tuning_jump_velocity = {};
    g_tuning_max_acceleration = {};
    g_tuning_falling_friction = {};
    g_tuning_falling_braking = {};
    ResetAirMomentum();
    ResetSlideTuningState();
    g_tuning_warning_logged = false;
    g_game_data_valid = false;
    // Saved positions intentionally remain across run restarts only if player remains the same;
    // on a new character/map they're cleared to prevent teleporting into stale geometry.
    g_saved_valid = false;
    g_button_splits.clear();
    g_button_state_initialized = false;
    g_split_popup_expires = 0;
    g_run_started_at_zero = false;
    g_run_pb_eligible = false;
    g_run_finish_processed = false;
    g_observed_buttons = -1;
    g_observed_resets = -1;
    g_practice_gold_anchor_button = 0;
    g_practice_gold_anchor_seconds = -1.0;
    if (!g_practice_active) g_last_button_checkpoint_valid = false;
}

void MaintainFlyingState(MyCharacter* player) {
    if (!g_flying || !player || !player->movement) return;
    MovementComponent* movement = player->movement;
    // Preserve the pre/post-native-tick fly state without unnecessary writes.
    if (movement->movement_mode != 5) movement->movement_mode = 5;
    if (movement->max_fly_speed != g_fly_speed)
        movement->max_fly_speed = g_fly_speed;
}

void SetFlying(MyCharacter* player, bool enabled) {
    if (!player || !player->movement || enabled == g_flying) return;
    ResetAirMomentum();
    ResetSlideTuningState();
    MovementComponent* movement = player->movement;
    if (enabled) {
        g_recording_tainted = true;
        if (!g_pb_cheats_allowed_for_run) g_run_pb_eligible = false;
        if (!CheatRecordsSegments(g_cheat_record_mode)) {
            g_practice_gold_anchor_button = 0;
            g_practice_gold_anchor_seconds = -1.0;
        }
        g_original_fly_speed = movement->max_fly_speed;
        movement->max_fly_speed = g_fly_speed;
        movement->movement_mode = 5;  // MOVE_Flying
        movement->velocity.z = 0.0f;
    } else {
        movement->movement_mode = 1;  // MOVE_Walking
        movement->velocity.z = 0.0f;
        movement->max_fly_speed = g_original_fly_speed;
        if (g_noclip) {
            g_noclip = false;
            if (g_set_collision) g_set_collision(player, 1u);
        }
    }
    g_flying = enabled;
}

void SetNoclip(MyCharacter* player, bool enabled) {
    if (!player || !g_set_collision || !player->movement) return;
    if (enabled && !g_flying) SetFlying(player, true);
    if (enabled == g_noclip) return;
    g_noclip = enabled;
    if (enabled) {
        g_recording_tainted = true;
        if (!g_pb_cheats_allowed_for_run) g_run_pb_eligible = false;
        if (!CheatRecordsSegments(g_cheat_record_mode)) {
            g_practice_gold_anchor_button = 0;
            g_practice_gold_anchor_seconds = -1.0;
        }
    }
    g_set_collision(player, enabled ? 0u : 1u);
}

void SavePosition(MyCharacter* player) {
    g_saved_position = player->root_component->location;
    g_saved_rotation = player->controller->control_rotation;
    g_saved_valid = true;
    Notify(L"Position saved");
}
// Explicit coordinate teleport. Mirrors RestorePosition's assisted-run policy
// without overwriting the player's F8/F9 saved position or camera direction.
bool TeleportToCoordinates(MyCharacter* player, const Vec3& destination,
                           bool allow_when_practice_ready) {
    if (g_practice_active &&
        !(allow_when_practice_ready && g_practice_phase == PracticePhase::Ready)) {
        Notify(L"Finish section loading before teleporting");
        return false;
    }
    constexpr float kMaxCoordinate = 100000.0f;
    if (!std::isfinite(destination.x) || !std::isfinite(destination.y) ||
        !std::isfinite(destination.z) ||
        std::abs(destination.x) > kMaxCoordinate ||
        std::abs(destination.y) > kMaxCoordinate ||
        std::abs(destination.z) > kMaxCoordinate) {
        Notify(L"Teleport coordinates must be within +/-100000");
        return false;
    }
    if (!player || !IsReadable(player, sizeof(*player)) ||
        !IsReadable(player->root_component, sizeof(SceneComponent)) ||
        !IsReadable(player->movement, sizeof(MovementComponent))) {
        Notify(L"Teleport unavailable: player is not ready");
        return false;
    }
    ResetAirMomentum();
    ResetSlideTuningState();
    ResetWorldSpeedSampling();
    g_recording_tainted = true;
    if (!g_pb_cheats_allowed_for_run) g_run_pb_eligible = false;
    g_practice_gold_anchor_button = 0;
    g_practice_gold_anchor_seconds = -1.0;
    player->movement->velocity = Vec3{};
    player->movement->acceleration = Vec3{};
    player->root_component->location = destination;
    MarkCheatAction(CheatAction::Position);
    Notify(L"Teleported to X=" + CheckpointFloat(destination.x) +
           L" Y=" + CheckpointFloat(destination.y) +
           L" Z=" + CheckpointFloat(destination.z));
    return true;
}

void RestorePosition(MyCharacter* player) {
    if (!g_saved_valid) { Notify(L"No saved position"); return; }
    ResetAirMomentum();
    ResetSlideTuningState();
    ResetWorldSpeedSampling();
    g_recording_tainted = true;
    if (!g_pb_cheats_allowed_for_run) g_run_pb_eligible = false;
    g_practice_gold_anchor_button = 0;
    g_practice_gold_anchor_seconds = -1.0;
    player->root_component->location = g_saved_position;
    player->controller->control_rotation = g_saved_rotation;
    MarkCheatAction(CheatAction::Position);
    player->movement->velocity = Vec3{};
    player->movement->acceleration = Vec3{};
    Notify(L"Position restored");
}

// Restore level animation speeds when leaving section practice.
// Actor addresses are validated against their original reflected class, so
// unloading/restarting a map never blindly writes to potentially stale memory.
void ApplyTunedFloat(TunedFloatProperty& field, float multiplier) {
    // At native defaults no read or write is needed unless restoring an override.
    if (!field.modified && std::abs(multiplier - 1.0f) < 0.0001f) return;
    if (!field.address || !IsReadable(field.address, sizeof(float))) return;
    if (std::abs(multiplier - 1.0f) < 0.0001f) {
        if (field.modified) {
            const float actual = *field.address;
            const float tolerance = std::max(0.01f, std::abs(field.last_written) * 0.001f);
            if (std::isfinite(actual) && std::abs(actual - field.last_written) <= tolerance)
                *field.address = field.original;
            field.modified = false;
        }
        return;
    }
    const float target = field.original * multiplier;
    // No arbitrary 100000-unit cap: only reject values float cannot represent.
    // Very high valid values can still destabilize Unreal physics.
    if (!std::isfinite(target) || target < 0.0f) return;
    if (*field.address != target) *field.address = target;
    field.last_written = target;
    field.modified = true;
}

// Read a UObject property referencing another UObject using native reflection.
// Any failed or implausible pointer is treated as unavailable; no blind offsets.
void* ReadReflectedObject(void* owner, const wchar_t* property) {
    if (!IsReadable(owner, sizeof(UObjectHeader))) return nullptr;
    void* klass = static_cast<const UObjectHeader*>(owner)->class_ptr;
    if (!IsReadable(klass, sizeof(UStructHeader))) return nullptr;
    const int32_t offset = FindClassPropertyOffset(klass, property);
    if (offset < 0) return nullptr;
    auto* slot = reinterpret_cast<void**>(static_cast<uint8_t*>(owner) + offset);
    if (!IsReadable(slot, sizeof(void*))) return nullptr;
    void* object = *slot;
    return IsReadable(object, sizeof(UObjectHeader)) ? object : nullptr;
}

// Refunct's controller owns the final rendered camera FOV. The old implementation
// only wrote FirstPersonCamera.FieldOfView, which the PlayerCameraManager can
// ignore/override. UE4 exposes APlayerController::FOV(float NewFOV) as a
// reflected exec function; it forwards to PlayerCameraManager::SetFOV and locks
// the final camera FOV. Passing zero releases that lock in UE4. We resolve the
// function and its parameter offset via reflection, never by guessing an RVA.
// If the function is missing on a different Refunct build, fall back to the
// actual FirstPersonCamera component and its reflected SetFieldOfView method.
namespace {
MyCharacter* s_fov_player = nullptr;
PlayerController* s_fov_controller = nullptr;
bool s_fov_controller_override = false;
int s_fov_controller_last_value = 0;
int s_fov_controller_last_attempted = 0;
ULONGLONG s_fov_controller_next_attempt = 0;
bool s_fov_controller_logged = false;
bool s_fov_controller_verified = false;
ULONGLONG s_fov_verify_after = 0;
void* s_fov_camera = nullptr;
TunedFloatProperty s_fov_field{};
ULONGLONG s_fov_retry_after = 0;
bool s_fov_warning_logged = false;

// APlayerController::FOV is an exec UFUNCTION (not a native vtable guess).
// Require the actual PlayerCameraManager to exist before dispatching; without
// it the command is a silent no-op during player/map initialization.
bool ApplyControllerFov(PlayerController* controller, float degrees) {
    if (!controller || !g_process_event ||
        !IsReadable(controller, sizeof(UObjectHeader)) ||
        !ReadReflectedObject(controller, L"PlayerCameraManager")) return false;
    void* function = FindReflectedFunction(controller, L"FOV");
    if (!function) return false;
    const int32_t offset = FindClassPropertyOffset(function, L"NewFOV");
    constexpr size_t kParamsSize = 64;
    if (offset < 0 || static_cast<size_t>(offset) > kParamsSize - sizeof(float))
        return false;
    alignas(16) std::array<uint8_t, kParamsSize> params{};
    std::memcpy(params.data() + offset, &degrees, sizeof(degrees));
    g_process_event(controller, function, params.data());
    return true;
}

// Inspect the rendered camera manager only once after a setting change. This
// gives us a diagnostic when UE accepts the command but renders another FOV.
bool ReadRenderedFov(PlayerController* controller, float& out_fov) {
    if (!controller || !g_process_event) return false;
    void* manager = ReadReflectedObject(controller, L"PlayerCameraManager");
    if (!manager) return false;
    void* function = FindReflectedFunction(manager, L"GetFOVAngle");
    if (!function) return false;
    const int32_t offset = FindClassPropertyOffset(function, L"ReturnValue");
    constexpr size_t kParamsSize = 64;
    if (offset < 0 || static_cast<size_t>(offset) > kParamsSize - sizeof(float))
        return false;
    alignas(16) std::array<uint8_t, kParamsSize> params{};
    g_process_event(manager, function, params.data());
    std::memcpy(&out_fov, params.data() + offset, sizeof(out_fov));
    // Diagnostics must also accept extreme/negative values. Reject only NaN
    // or infinity, rather than treating intentionally distorted FOV as invalid.
    return std::isfinite(out_fov);
}

bool IsActualPlayerCamera(void* candidate, MyCharacter* player) {
    if (!IsReadable(candidate, sizeof(UObjectHeader))) return false;
    const auto* object = static_cast<const UObjectHeader*>(candidate);
    if (!IsReadable(object->class_ptr, sizeof(UStructHeader))) return false;
    // Blueprint camera subclasses still inherit from CameraComponent.
    void* klass = object->class_ptr;
    bool is_camera = false;
    for (int depth = 0; depth < 16 && IsReadable(klass, sizeof(UStructHeader)); ++depth) {
        if (UnrealName(static_cast<const UObjectHeader*>(klass)->name) == L"CameraComponent") {
            is_camera = true;
            break;
        }
        klass = static_cast<const UStructHeader*>(klass)->super_struct;
    }
    if (!is_camera) return false;
    if (object->outer == player) return true;
    // Some versions nest a camera component under an owned subobject. Verify
    // its GetOwner() rather than trusting the object name alone.
    if (!g_process_event) return false;
    void* get_owner = FindReflectedFunction(candidate, L"GetOwner");
    if (!get_owner) return false;
    const int32_t offset = FindClassPropertyOffset(get_owner, L"ReturnValue");
    constexpr size_t kParamsSize = 64;
    if (offset < 0 || static_cast<size_t>(offset) > kParamsSize - sizeof(void*))
        return false;
    alignas(16) std::array<uint8_t, kParamsSize> params{};
    g_process_event(candidate, get_owner, params.data());
    void* owner = nullptr;
    std::memcpy(&owner, params.data() + offset, sizeof(owner));
    return owner == player;
}

void* FindFirstPersonCamera(MyCharacter* player) {
    if (!g_game_module || !IsReadable(player, sizeof(*player))) return nullptr;
    for (const wchar_t* name : {L"FirstPersonCamera", L"FirstPersonCameraComponent"}) {
        void* object = ReadReflectedObject(player, name);
        if (IsActualPlayerCamera(object, player)) return object;
    }
    const auto* objects = reinterpret_cast<const GlobalObjectArray*>(
        reinterpret_cast<uintptr_t>(g_game_module) + kGlobalObjectArrayRva);
    if (!IsReadable(objects, sizeof(*objects)) || !objects->objects ||
        objects->num_elements < 1 || objects->num_elements > 500000 ||
        objects->max_elements < objects->num_elements ||
        !IsReadable(objects->objects, sizeof(GlobalObjectItem) *
                      static_cast<size_t>(objects->num_elements))) return nullptr;
    for (int32_t i = 0; i < objects->num_elements; ++i) {
        const auto& item = objects->objects[i];
        if (!item.object || (static_cast<uint32_t>(item.flags) & 0x30000000u) ||
            !IsReadable(item.object, sizeof(UObjectHeader))) continue;
        // Filter by the native UObject outer chain *before* converting FNames.
        // Resolving a name for every global object would freeze older PCs.
        void* outer = item.object->outer;
        bool belongs_to_player = false;
        for (int depth = 0; depth < 4 && IsReadable(outer, sizeof(UObjectHeader)); ++depth) {
            if (outer == player) { belongs_to_player = true; break; }
            outer = static_cast<const UObjectHeader*>(outer)->outer;
        }
        if (!belongs_to_player) continue;
        if (UnrealName(item.object->name) == L"FirstPersonCamera" &&
            IsActualPlayerCamera(item.object, player)) return item.object;
    }
    return nullptr;
}

bool SetCameraComponentFov(void* camera, float degrees) {
    if (!camera || !g_process_event) return false;
    void* function = FindReflectedFunction(camera, L"SetFieldOfView");
    if (!function) return false;
    const int32_t offset = FindClassPropertyOffset(function, L"InFieldOfView");
    constexpr size_t kParamsSize = 64;
    if (offset < 0 || static_cast<size_t>(offset) > kParamsSize - sizeof(float))
        return false;
    alignas(16) std::array<uint8_t, kParamsSize> params{};
    std::memcpy(params.data() + offset, &degrees, sizeof(degrees));
    g_process_event(camera, function, params.data());
    return true;
}
}  // namespace

void ResetPlayerFovTracking() {
    // Never retain pointers to destroyed characters/controllers after a reset.
    s_fov_player = nullptr;
    s_fov_controller = nullptr;
    s_fov_controller_override = false;
    s_fov_controller_last_value = 0;
    s_fov_controller_last_attempted = 0;
    s_fov_controller_next_attempt = 0;
    s_fov_controller_logged = false;
    s_fov_controller_verified = false;
    s_fov_verify_after = 0;
    s_fov_camera = nullptr;
    s_fov_field = {};
    s_fov_retry_after = 0;
    s_fov_warning_logged = false;
}

void ApplyPlayerFov(MyCharacter* player) {
    if (!player || !IsReadable(player, sizeof(*player)) ||
        !IsReadable(player->controller, sizeof(PlayerController))) return;

    PlayerController* controller = player->controller;
    if (player != s_fov_player) {
        // A respawn can keep the same PlayerController and its FOV lock.
        // Discard old character/component pointers, but retain ownership of
        // that controller's lock so a replay FOV can still be released.
        const bool keep_lock = controller == s_fov_controller && s_fov_controller_override;
        const int last_value = s_fov_controller_last_value;
        ResetPlayerFovTracking();
        s_fov_player = player;
        if (keep_lock) {
            s_fov_controller = controller;
            s_fov_controller_override = true;
            s_fov_controller_last_value = last_value;
        }
    }
    if (controller != s_fov_controller) {
        s_fov_controller = controller;
        s_fov_controller_override = false;
        s_fov_controller_last_value = 0;
        s_fov_controller_last_attempted = 0;
        s_fov_controller_next_attempt = 0;
        s_fov_controller_logged = false;
        s_fov_controller_verified = false;
        s_fov_verify_after = 0;
    }
    // Replay cameras use the final camera-manager lock, not the gameplay
    // camera component. The user's saved FOV is never changed.
    const bool replay_first_person = IsGhostReplayFirstPersonActive(player);
    const int requested = replay_first_person ? 130 : g_player_fov_degrees;
    const ULONGLONG now = GetTickCount64();

    if (requested == 0) {
        // The UE4 camera manager treats LockedFOV <= 0 as unlocked. Crucially,
        // we must unlock instead of replacing DefaultFOV with a guessed angle.
        if (s_fov_controller_override &&
            (s_fov_controller_next_attempt <= now || s_fov_controller_last_attempted != 0)) {
            s_fov_controller_last_attempted = 0;
            if (ApplyControllerFov(controller, 0.0f)) {
                s_fov_controller_override = false;
                s_fov_controller_last_value = 0;
                Log(L"FOV: camera-manager override released");
            }
            s_fov_controller_next_attempt = now + 1000;
        }
        if (!s_fov_controller_override) s_fov_controller_last_attempted = 0;
        if (s_fov_field.modified && IsReadable(s_fov_camera, sizeof(UObjectHeader)) &&
            IsReadable(s_fov_field.address, sizeof(float))) {
            const float actual = *s_fov_field.address;
            const float tolerance = std::max(0.01f, std::abs(s_fov_field.last_written) * 0.001f);
            if (std::isfinite(actual) && std::abs(actual - s_fov_field.last_written) <= tolerance) {
                if (!SetCameraComponentFov(s_fov_camera, s_fov_field.original))
                    *s_fov_field.address = s_fov_field.original;
            }
            s_fov_field.modified = false;
        }
        return;
    }

    // UE4's PlayerCameraManager treats a non-positive LockedFOV as unlocked.
    // For negative FOV, release any prior positive lock and try the actual
    // FirstPersonCamera component below. The engine may still reject or
    // override negative angles; do not pretend the lock supports them.
    if (requested < 0 && s_fov_controller_override &&
        ApplyControllerFov(controller, 0.0f)) {
        s_fov_controller_override = false;
        s_fov_controller_last_value = 0;
    }

    if (requested < 0) s_fov_controller_last_attempted = requested;

    // SetFOV is a *persistent lock*. Refresh every second so a newly created
    // camera manager or a game-side overwrite is also handled, but never scan
    // the global object list or resolve component fields every frame.
    if (requested > 0 &&
        (requested != s_fov_controller_last_attempted || now >= s_fov_controller_next_attempt)) {
        s_fov_controller_last_attempted = requested;
        const bool changed = requested != s_fov_controller_last_value;
        const bool applied = ApplyControllerFov(controller, static_cast<float>(requested));
        s_fov_controller_next_attempt = now + (applied ? 1000 : 2500);
        if (applied) {
            s_fov_controller_override = true;
            s_fov_controller_last_value = requested;
            if (changed) {
                s_fov_controller_verified = false;
                s_fov_verify_after = now + 800;
            }
            if (!s_fov_controller_logged) {
                Log(L"FOV: using PlayerController.FOV / PlayerCameraManager lock");
                s_fov_controller_logged = true;
            }
        }
    }
    if (requested > 0 && s_fov_controller_override) {
        if (!s_fov_controller_verified && now >= s_fov_verify_after) {
            float actual = 0.0f;
            if (ReadRenderedFov(controller, actual)) {
                if (std::abs(actual - static_cast<float>(requested)) > 1.0f) {
                    Log((L"FOV WARNING: requested " + std::to_wstring(requested) +
                         L", rendered camera reports " + std::to_wstring(actual)).c_str());
                    Notify(L"FOV not applied: see RefunctUtility.log");
                } else {
                    Log((L"FOV: rendered camera verified at " + std::to_wstring(actual)).c_str());
                }
            } else {
                Log(L"FOV: camera manager GetFOVAngle unavailable; visual check required");
            }
            s_fov_controller_verified = true;
        }
        return;
    }

    // A player-component fallback cannot change the separate replay camera.
    // Retry the controller at its normal deadline instead of touching gameplay.
    if (replay_first_person) return;

    // Fallback for builds without the controller's FOV exec function.
    if (s_fov_field.address &&
        (!IsReadable(s_fov_camera, sizeof(UObjectHeader)) ||
         !IsReadable(s_fov_field.address, sizeof(float)) ||
         !std::isfinite(*s_fov_field.address))) {
        s_fov_camera = nullptr;
        s_fov_field = {};
    }
    if (!s_fov_field.address) {
        if (now < s_fov_retry_after) return;
        s_fov_retry_after = now + 5000;
        s_fov_camera = FindFirstPersonCamera(player);
        if (s_fov_camera)
            s_fov_field = ResolveTunedFloat(s_fov_camera, L"FieldOfView", 30.0f, 170.0f);
        if (!s_fov_field.address) {
            if (!s_fov_warning_logged) {
                Log(L"FOV: controller command and FirstPersonCamera.FieldOfView unavailable");
                Notify(L"FOV unavailable: see RefunctUtility.log");
                s_fov_warning_logged = true;
            }
            return;
        }
        Log(L"FOV: using FirstPersonCamera.SetFieldOfView fallback");
        s_fov_warning_logged = false;
    }
    const float target = static_cast<float>(requested);
    if (std::abs(*s_fov_field.address - target) > 0.01f) {
        if (!SetCameraComponentFov(s_fov_camera, target))
            *s_fov_field.address = target;
        s_fov_field.last_written = target;
        s_fov_field.modified = true;
    }
}

// TAS: UWorld -> PersistentLevel -> WorldSettings -> TimeDilation.
// Global time dilation affects simulation, movement and world animations;
// it is NOT the same as an FPS limit or the character's walk speed.
void ApplyGameSpeed() {
    if (g_game_speed_percent == 100 && !g_tuning_time_dilation.modified) return;
    if (!g_game_module) return;
    auto* world_slot = reinterpret_cast<void**>(
        reinterpret_cast<uintptr_t>(g_game_module) + kGlobalWorldRva);
    if (!IsReadable(world_slot, sizeof(void*))) return;
    void* world = *world_slot;
    if (!IsReadable(world, sizeof(UObjectHeader))) return;

    // Refresh periodically because a level restart can replace WorldSettings
    // without changing the UWorld address. Reuse the original baseline if
    // the same WorldSettings object is still active (no repeated multiplier).
    const bool refresh = world != g_time_dilation_world ||
                         !g_tuning_time_dilation.address ||
                         !IsReadable(g_tuning_time_dilation.address, sizeof(float)) ||
                         ++g_time_dilation_refresh_ticks >= 120;
    if (refresh) {
        g_time_dilation_refresh_ticks = 0;
        void* level = ReadReflectedObject(world, L"PersistentLevel");
        void* settings = ReadReflectedObject(level, L"WorldSettings");
        if (settings != g_time_dilation_settings || world != g_time_dilation_world) {
            g_tuning_time_dilation = {};
            g_time_dilation_settings = settings;
            g_time_dilation_world = world;
        }
        if (settings && !g_tuning_time_dilation.address) {
            g_tuning_time_dilation = ResolveTunedFloat(settings, L"TimeDilation", 0.01f, 20.0f);
        }
    }
    if (!g_tuning_time_dilation.address) {
        if (g_game_speed_percent != 100 && !g_time_dilation_warning_logged) {
            Log(L"WARNING: WorldSettings.TimeDilation is unavailable in this Refunct build");
            Notify(L"Game speed unavailable: see RefunctUtility.log");
            g_time_dilation_warning_logged = true;
        }
        return;
    }
    // Apply after character tick so any game-side value resets do not undo it.
    ApplyTunedFloat(g_tuning_time_dilation, static_cast<float>(g_game_speed_percent) / 100.0f);
}

void ApplyMovementTuning(MyCharacter* player) {
    if (!player || !IsReadable(player, sizeof(MyCharacter)) ||
        !IsReadable(player->movement, sizeof(MovementComponent))) return;

    MovementComponent* movement = player->movement;
    if (g_tuning_player != player || g_tuning_movement != movement) {
        g_tuning_player = player;
        g_tuning_movement = movement;
        g_tuning_max_walk_speed = ResolveTunedFloat(movement, L"MaxWalkSpeed", 1.0f, 20000.0f);
        // Resolve swimming independently of walking; no fixed UObject offset.
        g_tuning_max_swim_speed = ResolveTunedFloat(movement, L"MaxSwimSpeed", 1.0f, 20000.0f);
        // Refunct-specific BP_MyCharacter float, verified in the shipped game asset.
        // Do not adjust MaxWalkSpeed/JumpZVelocity to simulate climbing.
        g_tuning_climb_speed = ResolveTunedFloat(player, L"ClimbSpeed", 0.01f, 20000.0f);
        g_tuning_base_speed = ResolveTunedFloat(player, L"BaseSpeed", 0.01f, 20000.0f);
        g_tuning_bonus_speed = ResolveTunedFloat(player, L"MaxBonusSpeed", 0.0f, 20000.0f);
        g_tuning_jump_velocity = ResolveTunedFloat(movement, L"JumpZVelocity", 1.0f, 20000.0f);
        // MaxAcceleration is the actual UE CharacterMovement property, not
        // the transient Acceleration velocity vector updated every frame.
        g_tuning_max_acceleration = ResolveTunedFloat(movement, L"MaxAcceleration", 1.0f, 100000.0f);
        g_tuning_falling_friction = ResolveTunedFloat(movement, L"FallingLateralFriction", 0.0f, 100000.0f);
        g_tuning_falling_braking = ResolveTunedFloat(movement, L"BrakingDecelerationFalling", 0.0f, 100000.0f);
        ResetAirMomentum();
    }
    if (GameplayTuningActive() && !g_tuning_warning_logged &&
        ((g_walk_speed_percent != 100 &&
          !g_tuning_max_walk_speed.address && !g_tuning_base_speed.address) ||
         (g_swim_speed_percent != 100 && !g_tuning_max_swim_speed.address) ||
         (g_climb_speed_percent != 100 && !g_tuning_climb_speed.address) ||
         (g_jump_height_percent != 100 && !g_tuning_jump_velocity.address) ||
         (g_acceleration_percent != 100 && !g_tuning_max_acceleration.address))) {
        Log(L"WARNING: requested movement property unavailable; verify Refunct executable version");
        Notify(L"Movement setting unavailable: see RefunctUtility log");
        g_tuning_warning_logged = true;
    }

    if (GameplayTuningActive()) {
        // Latch for the whole run, even if the user later returns to 100%.
        g_recording_tainted = true;
        if (!g_pb_cheats_allowed_for_run) g_run_pb_eligible = false;
        if (!CheatRecordsSegments(g_cheat_record_mode)) {
            g_practice_gold_anchor_button = 0;
            g_practice_gold_anchor_seconds = -1.0;
        }
    }
    const float walk_multiplier = static_cast<float>(g_walk_speed_percent) / 100.0f;
    // With unchanged gravity, jump height is proportional to velocity squared.
    // sqrt(2) velocity therefore produces approximately double jump height.
    const float jump_velocity_multiplier = std::sqrt(
        static_cast<float>(g_jump_height_percent) / 100.0f);
    ApplyTunedFloat(g_tuning_base_speed, walk_multiplier);
    ApplyTunedFloat(g_tuning_bonus_speed, walk_multiplier);
    ApplyTunedFloat(g_tuning_max_walk_speed, walk_multiplier);
    ApplyTunedFloat(g_tuning_max_swim_speed,
                    static_cast<float>(g_swim_speed_percent) / 100.0f);
    ApplyTunedFloat(g_tuning_climb_speed,
                    static_cast<float>(g_climb_speed_percent) / 100.0f);
    ApplyTunedFloat(g_tuning_jump_velocity, jump_velocity_multiplier);
    ApplyTunedFloat(g_tuning_max_acceleration,
                    static_cast<float>(g_acceleration_percent) / 100.0f);
    // With the toggle enabled, prevent UE4 from braking or applying lateral
    // friction in falling mode. Turning it off restores the cached originals.
    ApplyTunedFloat(g_tuning_falling_friction, g_preserve_momentum ? 0.0f : 1.0f);
    ApplyTunedFloat(g_tuning_falling_braking, g_preserve_momentum ? 0.0f : 1.0f);
}

// Slide speed and distance are intentionally independent of walk/swim speed.
// Refunct has no standard UE4 MOVE_Sliding mode. The fallback below detects
// a running crouch (default Shift) and leaves all unrelated modes untouched.
// This is deliberately conservative until the game's native slide state can
// be verified against its executable.
void UpdateSlideTuning(MyCharacter* player, float dt) {
    if (!player || !IsReadable(player->root_component, sizeof(SceneComponent)) ||
        !IsReadable(player->movement, sizeof(MovementComponent))) {
        ResetSlideTuningState();
        return;
    }
    if (s_slide.character != player) {
        ResetSlideTuningState();
        s_slide.character = player;
    }
    const bool has_focus = HasFocusedGameWindow() &&
                           !g_rebinding.load(std::memory_order_relaxed) &&
                           !g_name_editing.load(std::memory_order_relaxed) &&
                           !g_coordinate_editing.load(std::memory_order_relaxed) &&
                           !IsMenuSearchTypingKey(VK_SHIFT);
    const bool crouch_down = has_focus && ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0);
    const bool new_crouch_press = crouch_down && !s_slide.crouch_was_down;
    s_slide.crouch_was_down = crouch_down;

    MovementComponent* movement = player->movement;
    const bool grounded = movement->movement_mode == 1 || movement->movement_mode == 2;
    const bool tuned = g_slide_speed_percent != 100 || g_slide_distance_percent != 100;
    if (!tuned || !has_focus || !crouch_down || !grounded || g_flying || g_noclip ||
        g_restore_flying_pending || g_restore_noclip_pending ||
        !std::isfinite(dt) || dt < 0.0f || dt > 0.25f) {
        s_slide.active = false;
        return;
    }

    Vec3& velocity = movement->velocity;
    const float speed = std::hypot(velocity.x, velocity.y);
    const Vec3 location = player->root_component->location;
    if (!std::isfinite(speed) || !std::isfinite(location.x) || !std::isfinite(location.y)) {
        s_slide.active = false;
        return;
    }

    if (new_crouch_press) {
        // Ignore ordinary crouching while stationary or walking slowly.
        // Use the actual, reflected run speed when available.
        float threshold = 160.0f;
        if (g_tuning_base_speed.address) {
            const float base = g_tuning_base_speed.original *
                               static_cast<float>(g_walk_speed_percent) / 100.0f;
            if (std::isfinite(base)) threshold = std::clamp(base * 0.55f, 120.0f, 700.0f);
        }
        s_slide.active = speed >= threshold;
        s_slide.elapsed = 0.0f;
        if (s_slide.active) {
            const float factor = static_cast<float>(g_slide_speed_percent) / 100.0f;
            // Avoid huge velocities that destabilize physics in old UE4 builds.
            const float target = std::min(speed * factor, 16000.0f);
            if (target > 0.0f && speed > 0.01f && std::isfinite(target)) {
                velocity.x *= target / speed;
                velocity.y *= target / speed;
            }
            s_slide.previous_speed = std::hypot(velocity.x, velocity.y);
            s_slide.previous_location = location;
        }
        return;
    }
    if (!s_slide.active) return;

    s_slide.elapsed += dt;
    if (s_slide.elapsed > 2.5f || speed < 10.0f) {
        s_slide.active = false;
        return;
    }

    // Scale only the *loss* of speed since the preceding slide frame. This
    // approximates longer/shorter slide travel without modifying global
    // GroundFriction or braking (which would affect ordinary walking).
    // When blocked against a wall, do not reintroduce lost velocity.
    const float moved = std::hypot(location.x - s_slide.previous_location.x,
                                   location.y - s_slide.previous_location.y);
    if (g_slide_distance_percent != 100 && s_slide.previous_speed > speed &&
        moved >= 0.5f && speed > 0.01f) {
        const float drag_scale = 100.0f / static_cast<float>(g_slide_distance_percent);
        const float loss = s_slide.previous_speed - speed;
        const float target = std::clamp(s_slide.previous_speed - loss * drag_scale,
                                        0.0f, 16000.0f);
        if (std::isfinite(target)) {
            const float factor = target / speed;
            velocity.x *= factor;
            velocity.y *= factor;
        }
    }
    s_slide.previous_speed = std::hypot(velocity.x, velocity.y);
    s_slide.previous_location = location;
}

// Refunct's CharacterTick may still clamp horizontal speed when walking turns
// into falling (jump or ledge). Preserve the grounded velocity at that
// transition. Keep at least that horizontal *speed* in the air, but use the
// engine's current velocity direction so air steering continues to work.
// Gravity / Z velocity, collisions and ground friction are untouched.
void UpdateAirMomentum(MyCharacter* player) {
    if (!g_preserve_momentum || g_flying || g_noclip ||
        (g_practice_active && g_practice_phase != PracticePhase::Ready) ||
        !player || !IsReadable(player->movement, sizeof(MovementComponent))) {
        ResetAirMomentum();
        return;
    }
    MovementComponent* movement = player->movement;
    AirMomentumState& state = g_air_momentum;
    if (state.movement != movement) {
        ResetAirMomentum();
        state.movement = movement;
    }

    const Vec3 velocity = movement->velocity;
    if (!std::isfinite(velocity.x) || !std::isfinite(velocity.y) ||
        !std::isfinite(velocity.z)) {
        ResetAirMomentum();
        return;
    }

    // UE4 EMovementMode: MOVE_Walking=1, MOVE_NavWalking=2,
    // MOVE_Falling=3. Swimming/flying/none end momentum preservation.
    if (movement->movement_mode == 1 || movement->movement_mode == 2) {
        state.have_ground_sample = true;
        state.ground_x = velocity.x;
        state.ground_y = velocity.y;
        state.airborne = false;
        state.minimum_air_speed = 0.0f;
        state.last_air_x = 0.0f;
        state.last_air_y = 0.0f;
        return;
    }
    if (movement->movement_mode != 3) {
        ResetAirMomentum();
        return;
    }

    const float actual_speed = std::hypot(velocity.x, velocity.y);
    if (!std::isfinite(actual_speed)) return;
    if (!state.airborne) {
        state.airborne = true;
        if (state.have_ground_sample) {
            const float ground_speed = std::hypot(state.ground_x, state.ground_y);
            if (std::isfinite(ground_speed) && ground_speed > 0.1f) {
                state.minimum_air_speed = ground_speed;
                state.last_air_x = state.ground_x;
                state.last_air_y = state.ground_y;
            }
        } else {
            // If the mod is enabled while already airborne, use the current
            // speed as baseline; don't invent a grounded velocity.
            state.minimum_air_speed = actual_speed;
            state.last_air_x = velocity.x;
            state.last_air_y = velocity.y;
        }
    }

    if (actual_speed > state.minimum_air_speed) {
        // Keep extra speed gained in the air as the new floor.
        state.minimum_air_speed = actual_speed;
    }
    if (actual_speed > 0.1f) {
        state.last_air_x = velocity.x;
        state.last_air_y = velocity.y;
    }

    const float floor = state.minimum_air_speed;
    if (!(floor > 0.1f) || !std::isfinite(floor)) return;
    const float tolerance = std::max(0.1f, floor * 0.001f);
    if (actual_speed >= floor - tolerance) return;

    const float direction_x = actual_speed > 0.1f ? velocity.x : state.last_air_x;
    const float direction_y = actual_speed > 0.1f ? velocity.y : state.last_air_y;
    const float direction_length = std::hypot(direction_x, direction_y);
    if (!(direction_length > 0.1f) || !std::isfinite(direction_length)) return;

    const double scale = static_cast<double>(floor) / direction_length;
    const double vx = direction_x * scale;
    const double vy = direction_y * scale;
    if (std::isfinite(vx) && std::isfinite(vy) &&
        std::abs(vx) <= (std::numeric_limits<float>::max)() &&
        std::abs(vy) <= (std::numeric_limits<float>::max)()) {
        movement->velocity.x = static_cast<float>(vx);
        movement->velocity.y = static_cast<float>(vy);
    }
}

// Larger percentages use progressively larger steps. Subtracting one when
// travelling left ensures returning across a threshold uses the prior step:
// 950 -> 1000 -> 1100 and 1100 -> 1000 -> 950, for example.
int PercentageAdjustmentStep(int percentage, int direction, int small_step) {
    const int index = percentage - (direction < 0 ? 1 : 0);
    if (index >= 100000000) return 10000000;
    if (index >= 10000000) return 1000000;
    if (index >= 1000000) return 100000;
    if (index >= 100000) return 10000;
    if (index >= 10000) return 1000;
    if (index >= 1000) return 100;
    if (index >= 300) return 50;
    return small_step;
}

int AdjustUncappedPercent(int percentage, int direction, int minimum, int small_step) {
    if (direction == 0) return percentage;
    const auto step = PercentageAdjustmentStep(percentage, direction, small_step);
    // 64-bit intermediate prevents overflow near INT_MAX before saturation.
    const auto next = static_cast<long long>(percentage) +
                      static_cast<long long>(direction > 0 ? step : -step);
    return static_cast<int>(std::clamp<long long>(
        next, minimum, kMaximumRepresentablePercent));
}

void InvalidateAssistedTiming() {
    g_recording_tainted = true;
    if (!g_pb_cheats_allowed_for_run) g_run_pb_eligible = false;
    g_practice_gold_anchor_button = 0;
    g_practice_gold_anchor_seconds = -1.0;
}

void AdjustMovementPercent(bool walking_speed, int direction) {
    int& percentage = walking_speed ? g_walk_speed_percent : g_jump_height_percent;
    const int updated = AdjustUncappedPercent(
        percentage, direction, kMovementMinimumPercent, 10);
    if (updated == percentage) return;
    percentage = updated;
    InvalidateAssistedTiming();
    SaveSettings();
    Notify(std::wstring(walking_speed ? L"Movement speed: " : L"Jump height: ") +
           std::to_wstring(updated) + L"%");
}

void AdjustSwimSpeedPercent(int direction) {
    const int updated = AdjustUncappedPercent(
        g_swim_speed_percent, direction, kMovementMinimumPercent, 10);
    if (updated == g_swim_speed_percent) return;
    g_swim_speed_percent = updated;
    InvalidateAssistedTiming();
    SaveSettings();
    Notify(L"Swim speed: " + std::to_wstring(updated) + L"%");
}

void AdjustClimbSpeedPercent(int direction) {
    const int updated = AdjustUncappedPercent(
        g_climb_speed_percent, direction, kMovementMinimumPercent, 10);
    if (updated == g_climb_speed_percent) return;
    g_climb_speed_percent = updated;
    InvalidateAssistedTiming();
    SaveSettings();
    Notify(L"Climb speed: " + std::to_wstring(updated) + L"%");
}

void AdjustSlidePercent(bool adjust_speed, int direction) {
    int& setting = adjust_speed ? g_slide_speed_percent : g_slide_distance_percent;
    const int updated = AdjustUncappedPercent(
        setting, direction, kMovementMinimumPercent, 10);
    if (updated == setting) return;
    setting = updated;
    // A change during a slide must not apply its remaining correction to the
    // new tuning values. The next slide starts with the new settings.
    s_slide.active = false;
    InvalidateAssistedTiming();
    SaveSettings();
    Notify(std::wstring(adjust_speed ? L"Slide speed: " : L"Slide distance: ") +
           std::to_wstring(updated) + L"%");
}

void AdjustAccelerationPercent(int direction) {
    const int updated = AdjustUncappedPercent(
        g_acceleration_percent, direction, kMovementMinimumPercent, 10);
    if (updated == g_acceleration_percent) return;
    g_acceleration_percent = updated;
    InvalidateAssistedTiming();
    SaveSettings();
    Notify(L"Acceleration: " + std::to_wstring(updated) + L"%");
}

void TogglePreserveMomentum() {
    g_preserve_momentum = !g_preserve_momentum;
    InvalidateAssistedTiming();
    ResetAirMomentum();
    SaveSettings();
    Notify(g_preserve_momentum
        ? L"Preserve momentum: ON (records disabled until clean restart)"
        : L"Preserve momentum: OFF (restart to enable records)");
}

const wchar_t* JumpTargetsLabel(JumpTargets targets) {
    switch (targets) {
        case JumpTargets::GroundOnly: return L"Ground Only";
        case JumpTargets::GroundAndWall: return L"Ground + Wall Jump";
        case JumpTargets::WallOnly: return L"Wall Jump Only";
        case JumpTargets::ClimbOnly: return L"Climb Only";
        case JumpTargets::GroundAndClimb: return L"Ground + Climb";
        case JumpTargets::WallAndClimb: return L"Wall Jump + Climb";
        case JumpTargets::GroundWallAndClimb: return L"Ground + Wall + Climb";
    }
    return L"Ground Only";
}

const wchar_t* JumpAssistModeLabel(bool scroll) {
    const bool enabled = scroll
        ? g_scroll_to_jump.load(std::memory_order_relaxed) : g_hold_space_to_jump;
    return enabled ? JumpTargetsLabel(scroll ? g_scroll_jump_targets : g_hold_jump_targets)
                   : L"Off";
}

void CycleJumpAssistMode(bool scroll, int direction) {
    const bool enabled = scroll
        ? g_scroll_to_jump.load(std::memory_order_relaxed) : g_hold_space_to_jump;
    JumpTargets& targets = scroll ? g_scroll_jump_targets : g_hold_jump_targets;
    const int current = JumpSelectorMode(enabled, targets);
    const int next = NextJumpSelectorMode(current, direction);

    // End outstanding synthetic presses before switching mode. Never leave
    // Space held after a climb mode is disabled or a menu selection changes.
    ResetJumpAssistState();
    if (next != 0) targets = static_cast<JumpTargets>(next - 1);
    if (scroll) g_scroll_to_jump.store(next != 0, std::memory_order_relaxed);
    else g_hold_space_to_jump = next != 0;

    InvalidateAssistedTiming();
    SaveSettings();
    Notify(std::wstring(scroll ? L"Scroll jump: " : L"Hold Space jump: ") +
           JumpAssistModeLabel(scroll) +
           (next == 0 ? L" (restart to re-enable records)" : L" (records disabled)"));
}

void ToggleAirJump() {
    g_air_jump_enabled = !g_air_jump_enabled;
    ResetAirJumpInputState();
    InvalidateAssistedTiming();
    SaveSettings();
    Notify(g_air_jump_enabled
        ? L"Air jump: ON (records disabled; press Space again midair)"
        : L"Air jump: OFF (restart to re-enable records)");
}

void AdjustGameSpeedPercent(int direction) {
    const int updated = AdjustUncappedPercent(
        g_game_speed_percent, direction, kGameSpeedMinimumPercent, 25);
    if (updated == g_game_speed_percent) return;
    g_game_speed_percent = updated;
    InvalidateAssistedTiming();
    SaveSettings();
    Notify(L"Game speed: " + std::to_wstring(updated) + L"%");
}

// Movement-only check. Never use CheatsCurrentlyActive() here: that now
// includes world colours, section edits, checkpoints, and historical actions.
namespace {
bool MovementSettingsModified() {
    return g_walk_speed_percent != 100 || g_swim_speed_percent != 100 ||
           g_climb_speed_percent != 100 || g_slide_speed_percent != 100 ||
           g_slide_distance_percent != 100 || g_jump_height_percent != 100 ||
           g_acceleration_percent != 100 || g_game_speed_percent != 100 ||
           g_preserve_momentum || g_scroll_to_jump || g_hold_space_to_jump ||
           g_air_jump_enabled ||
           g_flying || g_noclip || g_restore_flying_pending ||
           g_restore_noclip_pending || g_fly_speed != 1200.0f;
}
} // namespace

// Reset only the movement configuration. This is intentionally separate from
// record eligibility: a modified run stays ineligible until the game confirms
// a new run starting at zero buttons.
bool ResetMovementConfiguration() {
    const bool changed = MovementSettingsModified();
    g_fly_speed = 1200.0f;
    g_walk_speed_percent = 100;
    g_swim_speed_percent = 100;
    g_climb_speed_percent = 100;
    g_slide_speed_percent = 100;
    g_slide_distance_percent = 100;
    ResetSlideTuningState();
    g_jump_height_percent = 100;
    g_acceleration_percent = 100;
    g_game_speed_percent = 100;
    g_preserve_momentum = false;
    g_scroll_to_jump = false;
    g_hold_space_to_jump = false;
    g_air_jump_enabled = false;
    ResetJumpAssistState();
    ResetAirMomentum();
    return changed;
}

// Used for an ordinary R/native restart, and for a forced manual reset.
// Internal F1/F2/B checkpoint loads continue to preserve movement tuning.
void ResetWorldOnNormalRestart() {
    if (!g_keep_world_on_map_restart) ResetWorldGameplayForPersistence();
}

void ApplyWorldGameLaunchPersistence() {
    // Deferred until the first valid game-thread character tick. World actors,
    // material instances and reflection pointers are not available in DllMain.
    static bool applied = false;
    if (applied) return;
    applied = true;
    if (!g_keep_world_on_game_launch) ResetWorldGameplayForPersistence();
}

void ToggleWorldPersistence(bool game_launch) {
    bool& preference = game_launch ? g_keep_world_on_game_launch
                                   : g_keep_world_on_map_restart;
    preference = !preference;
    SaveSettings();
    Notify(std::wstring(game_launch ? L"Keep world after game launch: "
                                    : L"Keep world after map restart: ") +
           (preference ? L"ON" : L"OFF"));
}

void ResetMovementOnNormalRestart(MyCharacter* player, bool force_defaults) {
    // The explicit Restore Movement Defaults button must never reset World.
    if (!force_defaults) ResetWorldOnNormalRestart();
    const bool preserve = !force_defaults && g_keep_movement_on_map_restart;
    const bool had_modes = g_flying || g_noclip ||
                           g_restore_flying_pending || g_restore_noclip_pending;
    if (preserve) {
        g_restore_flying_pending |= g_flying;
        g_restore_noclip_pending |= g_noclip;
        // A saved noclip session always requires flying too.
        if (g_restore_noclip_pending) g_restore_flying_pending = true;
    } else {
        g_restore_flying_pending = false;
        g_restore_noclip_pending = false;
    }
    // Put the outgoing character in vanilla movement/collision mode before
    // the game destroys it; restore modes later on a valid new character.
    if (player && IsReadable(player->movement, sizeof(MovementComponent))) {
        if (g_flying) SetFlying(player, false);
        else if (g_noclip) SetNoclip(player, false);
    }
    if (preserve) {
        ResetJumpAssistState();
        ResetAirMomentum();
    } else {
        const bool settings_changed = ResetMovementConfiguration();
        if (settings_changed || had_modes) SaveSettings();
    }
    if (player && IsReadable(player->movement, sizeof(MovementComponent)))
        ApplyMovementTuning(player);
    ApplyGameSpeed();
}

// Called only after a valid character is ticking. Applying to a stale player
// pointer during RestartGame can crash on the next load.
void RestorePendingMovementModes(MyCharacter* player) {
    if (!player || !IsReadable(player->movement, sizeof(MovementComponent))) return;
    // Checkpoint reconstruction uses native button touches and teleports.
    // Avoid changing collision/movement mode halfway through that sequence.
    if (g_practice_active && g_practice_phase != PracticePhase::Idle &&
        g_practice_phase != PracticePhase::Ready &&
        g_practice_phase != PracticePhase::Failed) return;
    if (g_restore_flying_pending && !g_flying) SetFlying(player, true);
    if (g_restore_noclip_pending && !g_noclip) SetNoclip(player, true);
    if (g_flying) g_restore_flying_pending = false;
    if (g_noclip) g_restore_noclip_pending = false;
}

void ToggleMovementPersistence(bool game_launch) {
    bool& preference = game_launch ? g_keep_movement_on_game_launch
                                   : g_keep_movement_on_map_restart;
    preference = !preference;
    SaveSettings();
    Notify(std::wstring(game_launch ? L"Keep movement after game launch: "
                                    : L"Keep movement after map restart: ") +
           (preference ? L"ON" : L"OFF"));
}

void ToggleRecordSaving(bool personal_best) {
    bool& enabled = personal_best ? g_save_personal_bests : g_save_fastest_segments;
    enabled = !enabled;
    if (personal_best && !enabled) {
        // A completed run must not become recordable retroactively if the user
        // turns PB saving back on later. Next fresh run re-arms the gate.
        g_pb_recording_enabled_for_run = false;
    }
    SaveSettings();
    if (personal_best) {
        Notify(enabled ? L"Save PBs: ON (next new run)" : L"Save PBs: OFF");
    } else {
        Notify(enabled ? L"Save fastest segments: ON" : L"Save fastest segments: OFF");
    }
}

void RestoreMovementDefaults(MyCharacter* player) {
    const bool had_changes = MovementSettingsModified();
    ResetMovementOnNormalRestart(player, true);
    if (!had_changes) {
        Notify(L"Movement already at defaults");
        return;
    }
    InvalidateAssistedTiming();
    Notify(L"Movement defaults restored; restart for record eligibility");
}

// Locate the visual centre of the BP_Button_C actor rather than relying on
// the actor's pivot or the player's edge-of-button overlap position. The TAS
// also resolves and calls AActor::GetActorBounds through UE reflection.
// Returned coordinates are sanity-checked against the known vanilla map; if
// geometry or the game build differs, we fall back to the TAS centre table.
void* FindReflectedFunction(void* object, const wchar_t* name) {
    if (!IsReadable(object, sizeof(UObjectHeader))) return nullptr;
    void* klass = static_cast<const UObjectHeader*>(object)->class_ptr;
    return FindCachedClassField(klass, name, 2048);
}

// Scroll-up and Hold Space use Refunct's native Space input. Jump assists
// create press edges; climbing instead needs Space held across frames.
// Refunct itself decides whether an airborne Space input grabs a ledge or
// wall-jumps, so these filters do not suppress unassisted physical inputs.
void UpdateJumpAssist(MyCharacter* player) {
    const int wheel_requests = g_wheel_jump_requests.exchange(0, std::memory_order_relaxed);
    if (!player || !IsReadable(player->movement, sizeof(MovementComponent))) {
        ResetJumpAssistState();
        return;
    }
    const uint8_t movement_mode = player->movement->movement_mode;
    const bool grounded = movement_mode == 1 || movement_mode == 2;
    const bool airborne = movement_mode == 3;
    const ULONGLONG now = GetTickCount64();

    // Always release a one-frame pulse. Never leave synthetic Space stuck
    // after losing focus or changing movement / practice state.
    if (g_jump_assist.pulse_pending_release) {
        if (g_jump_assist.pressed_window)
            PostMessageW(g_jump_assist.pressed_window, WM_KEYUP, VK_SPACE, kSpaceKeyUpLParam);
        g_jump_assist.pulse_pending_release = false;
        g_jump_assist.pressed_window = nullptr;
    }

    const bool input_allowed = !g_flying && !g_noclip &&
        !g_restore_flying_pending && !g_restore_noclip_pending && HasFocusedGameWindow() &&
        !g_rebinding.load(std::memory_order_relaxed) &&
        !g_name_editing.load(std::memory_order_relaxed) &&
        !g_coordinate_editing.load(std::memory_order_relaxed) &&
        !IsMenuSearchTypingKey(VK_SPACE) &&
        (!g_practice_active || g_practice_phase == PracticePhase::Ready);
    const bool active = (g_scroll_to_jump || g_hold_space_to_jump) && input_allowed;

    const bool space_held = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
    // Detect physical key edges independently of synthetic remapped presses.
    // If a player is rebound while already holding Space, do not fake a jump.
    if (s_air_jump.character != player) {
        ResetAirJumpInputState();
        s_air_jump.character = player;
    }
    const bool fresh_space_press = s_air_jump.initialized &&
        space_held && !s_air_jump.previous_space_down;
    s_air_jump.initialized = true;
    s_air_jump.previous_space_down = space_held;
    const bool air_jump_from_wheel =
        g_scroll_to_jump.load(std::memory_order_relaxed) && wheel_requests > 0;
    s_air_jump.pending = g_air_jump_enabled && input_allowed && airborne &&
        (fresh_space_press || air_jump_from_wheel);
    if (g_jump_assist.synthetic_climb_held &&
        (!active || !airborne || space_held || now >= g_jump_assist.synthetic_climb_release_at)) {
        // When the real Space key is held, it owns the held state and will
        // receive its normal physical key-up; avoid injecting a competing one.
        if (!space_held && g_jump_assist.pressed_window)
            PostMessageW(g_jump_assist.pressed_window, WM_KEYUP, VK_SPACE, kSpaceKeyUpLParam);
        g_jump_assist.synthetic_climb_held = false;
        g_jump_assist.synthetic_climb_release_at = 0;
        g_jump_assist.pressed_window = nullptr;
    }

    if (!active) {
        g_jump_assist.grounded_on_previous_tick = grounded;
        g_jump_assist.next_hold_wall_attempt_at = 0;
        return;
    }

    const bool landed = grounded && !g_jump_assist.grounded_on_previous_tick;
    const bool scroll_requested = g_scroll_to_jump && wheel_requests > 0;
    const bool scroll_ground_requested = scroll_requested && grounded &&
        JumpAllowsGround(g_scroll_jump_targets);
    const bool scroll_climb_requested = scroll_requested && airborne &&
        !s_air_jump.pending && JumpAllowsClimb(g_scroll_jump_targets);
    const bool scroll_wall_requested = scroll_requested && airborne &&
        !s_air_jump.pending &&
        JumpAllowsWall(g_scroll_jump_targets) && !JumpAllowsClimb(g_scroll_jump_targets);

    // When Hold Space includes Climb, do not synthesize repeated wall-jump
    // press/release edges: those would break Refunct's native hold-to-climb.
    // The initial physical Space press can still initiate a native wall-jump.
    bool hold_wall_requested = false;
    if (!airborne || !space_held || !g_hold_space_to_jump ||
        !JumpAllowsWall(g_hold_jump_targets) || JumpAllowsClimb(g_hold_jump_targets)) {
        g_jump_assist.next_hold_wall_attempt_at = 0;
    } else {
        constexpr ULONGLONG kFirstAirborneAttemptMs = 125;
        constexpr ULONGLONG kWallRepeatIntervalMs = 115;
        if (!g_jump_assist.next_hold_wall_attempt_at) {
            g_jump_assist.next_hold_wall_attempt_at = now + kFirstAirborneAttemptMs;
        } else if (now >= g_jump_assist.next_hold_wall_attempt_at) {
            hold_wall_requested = true;
            g_jump_assist.next_hold_wall_attempt_at = now + kWallRepeatIntervalMs;
        }
    }
    const bool hold_ground_requested = g_hold_space_to_jump && space_held && landed &&
        JumpAllowsGround(g_hold_jump_targets);
    const bool pulse_requested = scroll_ground_requested || scroll_wall_requested ||
        hold_ground_requested || hold_wall_requested;

    if (pulse_requested) {
        // A press edge takes priority over an outstanding synthetic climb
        // hold. This can happen when both assists are enabled together.
        if (g_jump_assist.synthetic_climb_held) {
            if (g_jump_assist.pressed_window)
                PostMessageW(g_jump_assist.pressed_window, WM_KEYUP, VK_SPACE, kSpaceKeyUpLParam);
            g_jump_assist.synthetic_climb_held = false;
            g_jump_assist.synthetic_climb_release_at = 0;
            g_jump_assist.pressed_window = nullptr;
        }
        HWND window = GetForegroundWindow(); // Checked by HasFocusedGameWindow.
        if (window) {
            if (space_held)
                PostMessageW(window, WM_KEYUP, VK_SPACE, kSpaceKeyUpLParam);
            if (PostMessageW(window, WM_KEYDOWN, VK_SPACE, kSpaceKeyDownLParam)) {
                g_jump_assist.pressed_window = window;
                g_jump_assist.pulse_pending_release = true;
            }
        }
    } else if (scroll_climb_requested && !space_held) {
        // A mouse-wheel step is momentary; hold the remapped Space long enough
        // to request Refunct's native ledge grab. Further steps extend it.
        constexpr ULONGLONG kScrollClimbHoldMs = 450;
        HWND window = GetForegroundWindow();
        if (window) {
            // If this mode also includes wall jumps, a second wheel step
            // creates another press edge. One isolated step stays held for
            // ledge climbing, while repeated steps can wall-jump normally.
            if (g_jump_assist.synthetic_climb_held &&
                JumpAllowsWall(g_scroll_jump_targets)) {
                if (g_jump_assist.pressed_window)
                    PostMessageW(g_jump_assist.pressed_window, WM_KEYUP,
                                 VK_SPACE, kSpaceKeyUpLParam);
                g_jump_assist.synthetic_climb_held = false;
                g_jump_assist.pressed_window = nullptr;
            }
            if (!g_jump_assist.synthetic_climb_held) {
                if (PostMessageW(window, WM_KEYDOWN, VK_SPACE, kSpaceKeyDownLParam)) {
                    g_jump_assist.synthetic_climb_held = true;
                    g_jump_assist.pressed_window = window;
                }
            }
            if (g_jump_assist.synthetic_climb_held)
                g_jump_assist.synthetic_climb_release_at = now + kScrollClimbHoldMs;
        }
    }

    g_jump_assist.grounded_on_previous_tick = grounded;
}

void ApplyPendingAirJump(MyCharacter* player) {
    if (!s_air_jump.pending) return;
    s_air_jump.pending = false;
    if (!g_air_jump_enabled || s_air_jump.character != player ||
        !player || !IsReadable(player->movement, sizeof(MovementComponent)) ||
        player->movement->movement_mode != 3 || g_flying || g_noclip) return;

    // JumpZVelocity was resolved by ApplyMovementTuning after the native tick.
    // Use the real, currently tuned jump strength. Never guess an ABI offset.
    float* velocity_property = g_tuning_jump_velocity.address;
    if (!velocity_property || !IsReadable(velocity_property, sizeof(float))) {
        if (!s_air_jump.warned_missing_velocity) {
            s_air_jump.warned_missing_velocity = true;
            Log(L"Air jump: JumpZVelocity unavailable in this game build");
            Notify(L"Air jump unavailable: see RefunctUtility.log");
        }
        return;
    }
    const float launch_speed = *velocity_property;
    if (!std::isfinite(launch_speed) || launch_speed <= 0.0f || launch_speed > 100000.0f)
        return;
    // Only change vertical velocity. Preserve steering, collision, and the
    // original falling movement mode; every new press can trigger another jump.
    player->movement->velocity.z = launch_speed;
}

} // namespace refunct

#include "refunct/internal_api.hpp"
#include "refunct/features/time_of_day.hpp"

namespace refunct {
namespace {
constexpr int kMinutesPerDay = 24 * 60;
constexpr int kStepMinutes = 15;
constexpr int kObjectBatch = 1536;
constexpr ULONGLONG kDiscoveryBudgetMs = 2;
constexpr ULONGLONG kApplyIntervalMs = 100;
constexpr ULONGLONG kLockedApplyIntervalMs = 16;
constexpr int kSpeedStepPercent = 25;
constexpr int kMinSpeedPercent = 25;
constexpr int kMaxSpeedPercent = 1000;
constexpr ULONGLONG kSaveDelayMs = 350;

struct ClockSettings {
    bool loaded = false;
    bool enabled = false;
    bool locked = false;
    int minute = 12 * 60;
    int speed_percent = 100;
    bool pending_save = false;
    ULONGLONG save_due = 0;
};
ClockSettings s_settings;

enum class ScalarType { None, Integer, Float };
struct ScalarField {
    int offset = -1;
    ScalarType type = ScalarType::None;
};
struct NativeClock {
    void* object = nullptr;
    int32_t index = -1;
    int32_t serial = 0;
    ScalarField minute;
    ScalarField speed;
    double original_minute = 0.0;
    double last_minute = 0.0;
    double original_speed = 0.0;
    bool original_saved = false;
    bool speed_owned = false;
    double last_written_speed = 0.0;
};
NativeClock s_clock;
int32_t s_scan_cursor = -1;
ULONGLONG s_next_scan = 0;
ULONGLONG s_next_apply = 0;
bool s_reported_failure = false;
bool s_pending_minute = true;
std::atomic_int s_visible_minute{-1};
bool s_reported_speed_unavailable = false;
// Restore exactly once after switching off a custom clock, even with HUD enabled.
bool s_restore_after_disable = false;

int NormalizeMinute(int minute) {
    const int wrapped = minute % kMinutesPerDay;
    return wrapped < 0 ? wrapped + kMinutesPerDay : wrapped;
}

ClockSettings& TimeSettings() {
    if (s_settings.loaded || g_settings_path.empty()) return s_settings;
    s_settings.enabled = GetPrivateProfileIntW(L"TimeCustomisation", L"Enabled", 0,
        g_settings_path.c_str()) != 0;
    s_settings.minute = NormalizeMinute(static_cast<int>(GetPrivateProfileIntW(
        L"TimeCustomisation", L"MinuteOfDay", 720, g_settings_path.c_str())));
    s_settings.locked = GetPrivateProfileIntW(L"TimeCustomisation", L"Locked", 0,
        g_settings_path.c_str()) != 0;
    const int stored_speed = GetPrivateProfileIntW(L"TimeCustomisation",
        L"SpeedPercent", 100, g_settings_path.c_str());
    s_settings.speed_percent = (std::clamp)(
        (stored_speed / kSpeedStepPercent) * kSpeedStepPercent,
        kMinSpeedPercent, kMaxSpeedPercent);
    s_settings.loaded = true;
    return s_settings;
}

void ScheduleSave() {
    TimeSettings().pending_save = true;
    s_settings.save_due = GetTickCount64() + kSaveDelayMs;
}

void FlushSave() {
    if (!s_settings.pending_save || g_settings_path.empty() ||
        GetTickCount64() < s_settings.save_due) return;
    const bool enabled_saved = QueueSettingsValue(L"TimeCustomisation", L"Enabled",
        s_settings.enabled ? L"1" : L"0") != 0;
    const auto minute = std::to_wstring(s_settings.minute);
    const bool minute_saved = QueueSettingsValue(L"TimeCustomisation", L"MinuteOfDay",
        minute.c_str()) != 0;
    const bool lock_saved = QueueSettingsValue(L"TimeCustomisation", L"Locked",
        s_settings.locked ? L"1" : L"0") != 0;
    const auto speed = std::to_wstring(s_settings.speed_percent);
    const bool speed_saved = QueueSettingsValue(L"TimeCustomisation", L"SpeedPercent",
        speed.c_str()) != 0;
    s_settings.pending_save = !(enabled_saved && minute_saved && lock_saved && speed_saved);
    if (s_settings.pending_save) s_settings.save_due = GetTickCount64() + 3000;
}

std::wstring TimeObjectName(const void* object) {
    return IsReadable(object, sizeof(UObjectHeader))
        ? UnrealName(static_cast<const UObjectHeader*>(object)->name) : L"";
}

// Validate both the reflected name and its native property type before any write.
// The cooked WorldBase map names CurrentMinute and TimeSpeed; their offsets are
// resolved at runtime instead of assuming an ABI layout.
ScalarField FindScalar(void* klass, const wchar_t* property) {
    for (int depth = 0; depth < 12 && IsReadable(klass, sizeof(UStructHeader)); ++depth) {
        const auto* type = static_cast<const UStructHeader*>(klass);
        void* field = type->children;
        for (int count = 0; count < 4096 && IsReadable(field, sizeof(UFieldHeader)); ++count) {
            const auto* f = static_cast<const UFieldHeader*>(field);
            if (TimeObjectName(field) != property) { field = f->next; continue; }
            const std::wstring kind = TimeObjectName(f->object.class_ptr);
            ScalarField result{};
            if (kind == L"IntProperty") result.type = ScalarType::Integer;
            else if (kind == L"FloatProperty") result.type = ScalarType::Float;
            else return {};
            result.offset = FindClassPropertyOffset(klass, property);
            if (result.offset < 0 || result.offset > 0x2000 - static_cast<int>(sizeof(float)))
                return {};
            return result;
        }
        klass = type->super_struct;
    }
    return {};
}

bool ReadScalar(void* object, const ScalarField& field, double& result) {
    if (!object || field.offset < 0 || field.type == ScalarType::None) return false;
    const auto* address = static_cast<const uint8_t*>(object) + field.offset;
    if (!IsReadable(address, sizeof(int32_t))) return false;
    if (field.type == ScalarType::Integer) {
        int32_t value = 0;
        std::memcpy(&value, address, sizeof(value));
        result = static_cast<double>(value);
    } else {
        float value = 0;
        std::memcpy(&value, address, sizeof(value));
        if (!std::isfinite(value)) return false;
        result = value;
    }
    return std::isfinite(result);
}

bool WriteScalar(void* object, const ScalarField& field, double value) {
    if (!std::isfinite(value) || !object || field.offset < 0 ||
        field.type == ScalarType::None) return false;
    auto* address = static_cast<uint8_t*>(object) + field.offset;
    if (!IsReadable(address, sizeof(int32_t))) return false;
    if (field.type == ScalarType::Integer) {
        if (value < static_cast<double>((std::numeric_limits<int32_t>::min)()) ||
            value > static_cast<double>((std::numeric_limits<int32_t>::max)())) return false;
        const int32_t number = static_cast<int32_t>(value);
        std::memcpy(address, &number, sizeof(number));
    } else {
        const float number = static_cast<float>(value);
        if (!std::isfinite(number)) return false;
        std::memcpy(address, &number, sizeof(number));
    }
    return true;
}

const GlobalObjectArray* TimeObjects() {
    if (!g_game_module) return nullptr;
    const auto* objects = reinterpret_cast<const GlobalObjectArray*>(
        reinterpret_cast<uintptr_t>(g_game_module) + kGlobalObjectArrayRva);
    if (!IsReadable(objects, sizeof(*objects)) || !objects->objects ||
        objects->num_elements <= 0 || objects->num_elements > 1000000 ||
        objects->max_elements < objects->num_elements ||
        !IsReadable(objects->objects,
            static_cast<size_t>(objects->num_elements) * sizeof(GlobalObjectItem)))
        return nullptr;
    return objects;
}

bool LiveClock(const GlobalObjectArray* objects) {
    if (!objects || !s_clock.object || s_clock.index < 0 ||
        s_clock.index >= objects->num_elements) return false;
    const auto& item = objects->objects[s_clock.index];
    return item.object == s_clock.object && item.serial_number == s_clock.serial &&
        IsReadable(item.object, sizeof(UObjectHeader)) &&
        (static_cast<uint32_t>(item.object->flags) & 0x30u) == 0;
}

bool RegisterClock(void* object, const GlobalObjectArray* objects) {
    if (!IsReadable(object, sizeof(UObjectHeader))) return false;
    const auto* header = static_cast<const UObjectHeader*>(object);
    const int32_t index = header->index;
    if (index < 0 || index >= objects->num_elements ||
        objects->objects[index].object != object ||
        (static_cast<uint32_t>(header->flags) & 0x30u) != 0) return false;
    if (!IsReadable(header->class_ptr, sizeof(UStructHeader))) return false;
    const ScalarField minute = FindScalar(header->class_ptr, L"CurrentMinute");
    if (minute.type == ScalarType::None) return false;
    double current = 0.0;
    if (!ReadScalar(object, minute, current) || current < 0.0 ||
        current > kMinutesPerDay) return false;

    NativeClock candidate{};
    candidate.object = object;
    candidate.index = index;
    candidate.serial = objects->objects[index].serial_number;
    candidate.minute = minute;
    candidate.speed = FindScalar(header->class_ptr, L"TimeSpeed");
    candidate.original_minute = current;
    candidate.last_minute = current;
    if (candidate.speed.type != ScalarType::None) {
        double speed = 0.0;
        if (ReadScalar(object, candidate.speed, speed) &&
            speed >= 0.0 && speed <= 10000.0) {
            candidate.original_speed = speed;
        } else {
            candidate.speed = {};
        }
    }
    candidate.original_saved = true;
    s_clock = candidate;
    s_next_apply = 0;
    s_pending_minute = true; // New level: apply the saved anchor exactly once.
    s_visible_minute = static_cast<int>(current) % kMinutesPerDay;
    s_reported_failure = false;
    s_reported_speed_unavailable = false;
    Log((L"Time customisation: native CurrentMinute found on " +
         TimeObjectName(object) + L" (" + TimeObjectName(header->class_ptr) +
         L"), original minute=" + std::to_wstring(static_cast<int>(current)) +
         L", TimeSpeed=" + (candidate.speed.type == ScalarType::None ?
             L"unavailable" : std::to_wstring(candidate.original_speed))).c_str());
    return true;
}

void DiscoverClock(const GlobalObjectArray* objects) {
    if (!objects || s_scan_cursor < 0) return;
    const int32_t begin = s_scan_cursor;
    const int32_t end = (std::min)(objects->num_elements, begin + kObjectBatch);
    const auto start = GetTickCount64();
    for (; s_scan_cursor < end; ++s_scan_cursor) {
        if (s_scan_cursor != begin && (s_scan_cursor & 127) == 0 &&
            GetTickCount64() - start >= kDiscoveryBudgetMs) return;
        const auto& item = objects->objects[s_scan_cursor];
        if (!IsReadable(item.object, sizeof(UObjectHeader)) ||
            (static_cast<uint32_t>(item.object->flags) & 0x30u) != 0) continue;
        const auto* header = item.object;
        if (!IsReadable(header->class_ptr, sizeof(UObjectHeader))) continue;
        const std::wstring class_name = TimeObjectName(header->class_ptr);
        if (class_name == L"BP_WorldReferences_C") {
            // This is the game's actual TimeOfDay actor reference, not an
            // arbitrary sky component or unrelated game-clock variable.
            void* referenced = ReadReflectedObject(item.object, L"TimeOfDay");
            if (referenced && RegisterClock(referenced, objects)) break;
        }
        if (class_name == L"BP_TimeOfDay_C" || class_name == L"WorldBase_C") {
            if (RegisterClock(item.object, objects)) break;
        }
    }
    if (s_clock.object) { s_scan_cursor = -1; return; }
    if (s_scan_cursor >= objects->num_elements) {
        s_scan_cursor = -1;
        s_next_scan = GetTickCount64() + 5000;
        if (!s_reported_failure) {
            Log(L"Time customisation: no validated CurrentMinute field found on "
                L"the native TimeOfDay/WorldBase actor; no memory has been written");
            // A read-only HUD should not show a popup when the clock is absent.
            if (TimeSettings().enabled)
                Notify(L"Time customisation: native clock not found (see log)");
            s_reported_failure = true;
        }
    }
}

void RestoreClockIfOwned(const GlobalObjectArray* objects) {
    if (!LiveClock(objects) || !s_clock.original_saved) {
        s_clock = {};
        s_visible_minute = -1;
        return;
    }
    double current = 0.0;
    // Unlocking and adjusting the time shouldn't rewind the running day/night
    // cycle. Only restore the starting minute if the clock was actually locked.
    if (s_settings.locked &&
        ReadScalar(s_clock.object, s_clock.minute, current) &&
        std::abs(current - s_clock.last_minute) <= 0.5)
        WriteScalar(s_clock.object, s_clock.minute, s_clock.original_minute);

    // Undo our speed override, but never overwrite a value changed by another
    // system since we last wrote it.
    if (s_clock.speed_owned && s_clock.speed.type != ScalarType::None &&
        ReadScalar(s_clock.object, s_clock.speed, current) &&
        std::abs(current - s_clock.last_written_speed) <= 0.001)
        WriteScalar(s_clock.object, s_clock.speed, s_clock.original_speed);
    s_clock = {};
    s_visible_minute = -1;
}

// The native TimeSpeed scalar can be float or int. For integer variants,
// preserve at least one unit of progression while unlocked (where the
// multiplier cannot represent finer fractional steps).
double DesiredClockSpeed() {
    if (s_settings.locked) return 0.0;
    double desired = s_clock.original_speed *
        static_cast<double>(s_settings.speed_percent) / 100.0;
    if (s_clock.speed.type == ScalarType::Integer && s_clock.original_speed > 0.0)
        desired = (std::max)(1.0, std::floor(desired + 0.5));
    return (std::clamp)(desired, 0.0, 100000.0);
}

} // namespace

bool IsCustomTimeOfDayEnabled() { return TimeSettings().enabled; }
bool IsCustomTimeLocked() { return TimeSettings().locked; }
int GetLiveGameTimeMinutes() {
    return s_visible_minute.load(std::memory_order_relaxed);
}
int GetCustomTimeMinutes() {
    const auto& settings = TimeSettings();
    const int live_minute = s_visible_minute.load(std::memory_order_relaxed);
    return settings.enabled && !settings.locked && live_minute >= 0
        ? live_minute : settings.minute;
}
int GetCustomTimeSpeedPercent() { return TimeSettings().speed_percent; }

void SetCustomTimeOfDayEnabled(bool enabled) {
    if (TimeSettings().enabled == enabled) return;
    s_settings.enabled = enabled;
    if (enabled) {
        s_pending_minute = true;
        s_scan_cursor = 0;
        s_next_scan = 0;
        s_next_apply = 0;
        s_reported_failure = false;
    } else {
        // Restore original speed on the next game tick.
        s_scan_cursor = -1;
        s_pending_minute = true;
        s_restore_after_disable = true;
    }
    ScheduleSave();
    Notify(enabled ? L"Time of day: Custom (visual only)" :
                     L"Time of day: Game default");
}

void SetCustomTimeMinutes(int minutes) {
    const int normalized = NormalizeMinute(minutes);
    const bool changed = TimeSettings().minute != normalized;
    s_settings.minute = normalized;
    // Explicit edits always re-anchor the native clock, even if the selected
    // minute equals the last saved value but the clock has since advanced.
    s_pending_minute = true;
    s_next_apply = 0;
    if (!s_settings.enabled) SetCustomTimeOfDayEnabled(true);
    if (changed) ScheduleSave();
}

void AdjustCustomTimeMinutes(int direction) {
    if (direction == 0) return;
    const int next = GetCustomTimeMinutes() +
        (direction > 0 ? kStepMinutes : -kStepMinutes);
    SetCustomTimeMinutes(next);
}

void SetCustomTimeLocked(bool locked) {
    if (TimeSettings().locked == locked && s_settings.enabled) return;
    if (locked && s_settings.enabled) {
        // Freeze what the player currently sees, not an outdated edit value.
        s_settings.minute = GetCustomTimeMinutes();
        s_pending_minute = true;
    }
    s_settings.locked = locked;
    if (!s_settings.enabled) SetCustomTimeOfDayEnabled(true);
    if (!locked) s_pending_minute = false;
    s_next_apply = 0;
    ScheduleSave();
    Notify(locked ? L"Time of day: Locked" : L"Time of day: Running");
}

void SetCustomTimeSpeedPercent(int percent) {
    const int normalized = (std::clamp)(
        (percent / kSpeedStepPercent) * kSpeedStepPercent,
        kMinSpeedPercent, kMaxSpeedPercent);
    if (TimeSettings().speed_percent != normalized) {
        s_settings.speed_percent = normalized;
        s_next_apply = 0;
        ScheduleSave();
    }
    if (!s_settings.enabled) SetCustomTimeOfDayEnabled(true);
}

void AdjustCustomTimeSpeedPercent(int direction) {
    if (direction == 0) return;
    SetCustomTimeSpeedPercent(GetCustomTimeSpeedPercent() +
        (direction > 0 ? kSpeedStepPercent : -kSpeedStepPercent));
}

void UpdateCustomTimeOfDay(MyCharacter* player) {
    FlushSave();
    if (!player || !IsReadable(player, sizeof(MyCharacter))) return;
    // The in-game HUD clock is a passive observer and works with Game Default.
    // Never scan Unreal objects when neither custom time nor its HUD is active.
    const bool custom_enabled = TimeSettings().enabled;
    if (!custom_enabled && !g_show_game_time_hud && !s_restore_after_disable) {
        s_visible_minute = -1;
        return;
    }
    const ULONGLONG now = GetTickCount64();
    static MyCharacter* last_player = nullptr;
    if (player != last_player) {
        last_player = player;
        s_next_apply = 0;
    }
    // With an already discovered clock, no native object is accessed between
    // scheduled samples. A settings edit or player change clears the delay;
    // pending restores and discovery retain their existing immediate path.
    if (s_clock.object && now < s_next_apply && !s_restore_after_disable &&
        (custom_enabled || !s_clock.speed_owned)) return;
    const GlobalObjectArray* objects = TimeObjects();
    if (!objects) { s_visible_minute = -1; return; }
    if (!custom_enabled) {
        if (s_restore_after_disable || s_clock.speed_owned) {
            if (s_clock.object) RestoreClockIfOwned(objects);
            s_restore_after_disable = false;
        }
        if (!g_show_game_time_hud) { s_visible_minute = -1; return; }
        if (s_clock.object && !LiveClock(objects)) {
            s_clock = {};
            s_visible_minute = -1;
            s_scan_cursor = 0;
            s_next_scan = 0;
        }
        if (!s_clock.object && s_scan_cursor < 0 && now >= s_next_scan)
            s_scan_cursor = 0;
        if (!s_clock.object && s_scan_cursor >= 0) DiscoverClock(objects);
        if (!LiveClock(objects)) { s_visible_minute = -1; return; }
        if (now >= s_next_apply) {
            s_next_apply = now + kApplyIntervalMs;
            double current = 0.0;
            if (ReadScalar(s_clock.object, s_clock.minute, current) &&
                current >= 0.0 && current <= kMinutesPerDay)
                s_visible_minute = NormalizeMinute(static_cast<int>(current));
            else
                s_visible_minute = -1;
        }
        return; // Do not write CurrentMinute or TimeSpeed in Game Default.
    }
    if (s_clock.object && !LiveClock(objects)) {
        s_clock = {};
        s_visible_minute = -1;
        s_scan_cursor = 0;
        s_next_scan = 0;
        s_pending_minute = true;
    }
    if (!s_clock.object && s_scan_cursor < 0 && now >= s_next_scan)
        s_scan_cursor = 0;
    if (!s_clock.object && s_scan_cursor >= 0) DiscoverClock(objects);
    if (!LiveClock(objects) || now < s_next_apply) return;
    s_next_apply = now + (s_settings.locked ? kLockedApplyIntervalMs : kApplyIntervalMs);

    double current = 0.0;
    if (!ReadScalar(s_clock.object, s_clock.minute, current) ||
        current < 0.0 || current > kMinutesPerDay) return;

    if (s_settings.locked || s_pending_minute) {
        const double desired = static_cast<double>(s_settings.minute);
        if (std::abs(current - desired) > 0.01) {
            if (!WriteScalar(s_clock.object, s_clock.minute, desired)) return;
            s_clock.last_minute = desired;
        }
        s_visible_minute = s_settings.minute;
        s_pending_minute = false;
    } else {
        // Unlike the old implementation, don't pin the minute when unlocked.
        // Read it only for live menu display and subsequent 15-minute edits.
        s_visible_minute = NormalizeMinute(static_cast<int>(current));
    }

    if (s_clock.speed.type == ScalarType::None) {
        if (!s_reported_speed_unavailable &&
            (s_settings.locked || s_settings.speed_percent != 100)) {
            Log(L"Time customisation: TimeSpeed property unavailable; "
                L"lock uses periodic CurrentMinute writes and speed cannot be changed");
            Notify(L"Time Speed unavailable on this native clock (see log)");
            s_reported_speed_unavailable = true;
        }
        return;
    }

    double actual_speed = 0.0;
    if (!ReadScalar(s_clock.object, s_clock.speed, actual_speed)) return;
    const double desired_speed = DesiredClockSpeed();
    if (std::abs(actual_speed - desired_speed) <= 0.001) return;
    const bool native_unchanged =
        std::abs(actual_speed - s_clock.original_speed) <= 0.001;
    const bool still_ours = s_clock.speed_owned &&
        std::abs(actual_speed - s_clock.last_written_speed) <= 0.001;
    if ((native_unchanged || still_ours) &&
        WriteScalar(s_clock.object, s_clock.speed, desired_speed)) {
        s_clock.speed_owned = true;
        s_clock.last_written_speed = desired_speed;
    }
}
} // namespace refunct

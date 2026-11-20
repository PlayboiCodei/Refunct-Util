#include "refunct/internal_api.hpp"
#include "refunct/colour_work_budget.hpp"
#include "refunct/discovery_watermark.hpp"
#include "refunct/features/world_object_removal.hpp"
#include <unordered_map>

namespace refunct {
namespace {
constexpr size_t kKinds = static_cast<size_t>(WorldObjectKind::Count);
constexpr size_t kMaxActors = 4096;
constexpr int32_t kScanBatch = 1024;
constexpr size_t kApplyBatch = 8;
constexpr ULONGLONG kRescanMs = 15000;
constexpr ULONGLONG kRecheckMs = 1500;
constexpr size_t kParams = 96;

constexpr std::array<const wchar_t*, kKinds> kKeys = {
    L"Tubes", L"Buttons", L"JumpPads", L"Cubes", L"Platforms", L"Tiles"
};
constexpr std::array<const wchar_t*, kKinds> kLabels = {
    L"tubes", L"buttons", L"jump pads", L"cubes", L"moving platforms", L"tiles"
};

struct Settings {
    bool loaded = false;
    std::array<bool, kKinds> removed{};
};
Settings s_settings{};

struct Actor {
    void* object = nullptr;
    void* klass = nullptr;
    int32_t index = -1;
    int32_t serial = 0;
    WorldObjectKind kind = WorldObjectKind::Count;
    bool captured = false;
    bool original_hidden = false;
    bool original_collision = false;
    ULONGLONG retry_at = 0;
    ULONGLONG next_check = 0;
};

struct HiddenField {
    int32_t byte_offset = -1;
    uint8_t mask = 0;
};
struct Api {
    HiddenField hidden_field{};
    void* is_hidden = nullptr;
    void* set_hidden = nullptr;
    void* get_collision = nullptr;
    void* set_collision = nullptr;
    int32_t is_hidden_return = -1;
    int32_t set_hidden_value = -1;
    int32_t collision_return = -1;
    int32_t set_collision_value = -1;
};

std::vector<Actor> s_actors;
std::unordered_map<int32_t, size_t> s_by_index;
std::unordered_map<void*, WorldObjectKind> s_class_kind;
std::unordered_map<void*, Api> s_api;
MyCharacter* s_player = nullptr;
void* s_world = nullptr;
int32_t s_scan_cursor = -1;
size_t s_apply_cursor = 0;
size_t s_pending = 0;
ULONGLONG s_next_scan = 0;
DiscoveryWatermark s_discovery_watermark{};
ULONGLONG s_next_work = 0;
ULONGLONG s_save_at = 0;
bool s_save_dirty = false;
bool s_reported = false;
bool s_api_warning = false;
bool s_write_warning = false;

size_t Index(WorldObjectKind kind) { return static_cast<size_t>(kind); }
bool KnownKind(WorldObjectKind kind) { return Index(kind) < kKinds; }
bool AnyRemoved() {
    for (bool setting : s_settings.removed) if (setting) return true;
    return false;
}
Settings& Config() {
    if (s_settings.loaded || g_settings_path.empty()) return s_settings;
    for (size_t i = 0; i < kKinds; ++i)
        s_settings.removed[i] = GetPrivateProfileIntW(
            L"WorldObjectRemoval", kKeys[i], 0, g_settings_path.c_str()) != 0;
    s_settings.loaded = true;
    return s_settings;
}
void Save() {
    if (g_settings_path.empty()) return;
    bool ok = true;
    for (size_t i = 0; i < kKinds; ++i)
        ok &= QueueSettingsValue(L"WorldObjectRemoval", kKeys[i],
            Config().removed[i] ? L"1" : L"0") != 0;
    if (!ok) Log(L"World removal: failed to save object toggles");
}
void InvalidateWork() {
    s_next_work = 0;
    s_next_scan = 0;
    s_scan_cursor = 0;
    s_apply_cursor = 0;
    s_pending = s_actors.size();
    for (auto& actor : s_actors) {
        actor.next_check = 0;
        actor.retry_at = 0;
    }
    s_reported = false;
    s_save_dirty = true;
    s_save_at = GetTickCount64() + 350;
}
void SetOptions(const std::array<bool, kKinds>& next) {
    if (Config().removed == next) return;
    const bool enabled_any = std::any_of(next.begin(), next.end(), [](bool b) { return b; });
    Config().removed = next;
    // Even if removed before PB finish and later restored, the run was assisted.
    if (enabled_any) {
        g_recording_tainted = true;
        if (!g_pb_cheats_allowed_for_run) g_run_pb_eligible = false;
    }
    InvalidateWork();
}

const GlobalObjectArray* Objects() {
    if (!g_game_module) return nullptr;
    const auto* objects = reinterpret_cast<const GlobalObjectArray*>(
        reinterpret_cast<uintptr_t>(g_game_module) + kGlobalObjectArrayRva);
    if (!IsReadable(objects, sizeof(*objects)) || !objects->objects ||
        objects->num_elements < 0 || objects->num_elements > 1000000 ||
        objects->max_elements < objects->num_elements ||
        !IsReadable(objects->objects,
            static_cast<size_t>(objects->num_elements) * sizeof(GlobalObjectItem)))
        return nullptr;
    return objects;
}
bool Valid(const Actor& actor, const GlobalObjectArray* objects) {
    if (!objects || !actor.object || actor.index < 0 ||
        actor.index >= objects->num_elements) return false;
    const auto& slot = objects->objects[actor.index];
    return slot.object == actor.object && slot.serial_number == actor.serial &&
        !(static_cast<uint32_t>(slot.flags) & 0x30000000u) &&
        IsReadable(actor.object, sizeof(UObjectHeader)) &&
        static_cast<const UObjectHeader*>(actor.object)->class_ptr == actor.klass;
}
WorldObjectKind ClassKind(void* klass) {
    if (!IsReadable(klass, sizeof(UObjectHeader))) return WorldObjectKind::Count;
    const auto it = s_class_kind.find(klass);
    if (it != s_class_kind.end()) return it->second;
    const auto name = UnrealName(static_cast<const UObjectHeader*>(klass)->name);
    WorldObjectKind kind = WorldObjectKind::Count;
    if (name == L"BP_TravelPipe_C") kind = WorldObjectKind::Tubes;
    else if (name == L"BP_Button_C") kind = WorldObjectKind::Buttons;
    else if (name == L"BP_Jumppad_C") kind = WorldObjectKind::JumpPads;
    else if (name == L"BP_PowerCore_C") kind = WorldObjectKind::Cubes;
    else if (name == L"BP_Lift_C" || name == L"BP_Mover_C") kind = WorldObjectKind::Platforms;
    else if (name == L"BP_IslandChunk_C") kind = WorldObjectKind::Tiles;
    if (s_class_kind.size() < 4096) s_class_kind.emplace(klass, kind);
    return kind;
}

// No speculative UE4 field offsets: require actual reflected functions and
// their Boolean parameters before issuing any native actor writes.
int32_t BoolParameter(void* function, const wchar_t* name) {
    if (!IsReadable(function, sizeof(UStructHeader))) return -1;
    const int32_t offset = FindClassPropertyOffset(function, name);
    if (offset < 0 || offset >= static_cast<int32_t>(kParams)) return -1;
    int inspected = 0;
    for (void* field = static_cast<const UStructHeader*>(function)->children;
         inspected++ < 128 && IsReadable(field, sizeof(UFieldHeader)); ) {
        const auto* property = static_cast<const UFieldHeader*>(field);
        if (UnrealName(property->object.name) == name) {
            void* type = property->object.class_ptr;
            return IsReadable(type, sizeof(UObjectHeader)) &&
                UnrealName(static_cast<const UObjectHeader*>(type)->name) == L"BoolProperty"
                ? offset : -1;
        }
        field = property->next;
    }
    return -1;
}
// Refunct's UE4 AActor::IsHidden() is a C++ method on some builds, not a
// UFUNCTION. When absent, read the native UBoolProperty metadata for bHidden.
// Never guess a hard-coded actor byte offset or write the bit directly.
HiddenField ResolveHiddenField(void* actor) {
    if (!IsReadable(actor, sizeof(UObjectHeader))) return {};
    void* cls = static_cast<const UObjectHeader*>(actor)->class_ptr;
    const int32_t offset = FindClassPropertyOffset(cls, L"bHidden");
    if (offset < 0 || offset >= 0x2000) return {};
    for (int depth = 0; depth < 16 && IsReadable(cls, sizeof(UStructHeader)); ++depth) {
        const auto* type = static_cast<const UStructHeader*>(cls);
        int count = 0;
        for (void* field = type->children;
             count++ < 2048 && IsReadable(field, sizeof(UFieldHeader)); ) {
            const auto* property = static_cast<const UFieldHeader*>(field);
            if (UnrealName(property->object.name) != L"bHidden") {
                field = property->next;
                continue;
            }
            void* field_type = property->object.class_ptr;
            if (!IsReadable(field_type, sizeof(UObjectHeader)) ||
                UnrealName(static_cast<const UObjectHeader*>(field_type)->name) != L"BoolProperty")
                return {};
            // UE4 x86 UBoolProperty descriptor layout varies across builds.
            // Accept exactly one well-formed (FieldSize,ByteOffset,ByteMask,
            // FieldMask) descriptor, never any guessed value in the actor.
            HiddenField selected{};
            int matches = 0;
            constexpr size_t candidates[] = {0x50,0x54,0x58,0x5C,0x60,0x64};
            for (size_t index : candidates) {
                if (!IsReadable(field, index + 4)) continue;
                const uint8_t* bytes = static_cast<const uint8_t*>(field) + index;
                const uint8_t size = bytes[0], displacement = bytes[1];
                const uint8_t mask = bytes[2], field_mask = bytes[3];
                if ((size != 1 && size != 2 && size != 4 && size != 8) ||
                    displacement >= size || !mask || (mask & (mask - 1)) ||
                    (field_mask != mask && field_mask != 0xFF)) continue;
                if (offset + displacement >= 0x2000) continue;
                selected = {offset + displacement, mask};
                ++matches;
            }
            return matches == 1 ? selected : HiddenField{};
        }
        cls = type->super_struct;
    }
    return {};
}
bool ReadHidden(void* actor, const Api& api, bool& value) {
    if (api.is_hidden && api.is_hidden_return >= 0) {
        alignas(16) std::array<uint8_t, kParams> args{};
        g_process_event(actor, api.is_hidden, args.data());
        const uint8_t result = args[static_cast<size_t>(api.is_hidden_return)];
        if (result > 1) return false;
        value = result != 0;
        return true;
    }
    const HiddenField field = api.hidden_field;
    if (field.byte_offset < 0 || !field.mask) return false;
    const auto* address = static_cast<const uint8_t*>(actor) + field.byte_offset;
    if (!IsReadable(address, 1)) return false;
    value = (*address & field.mask) != 0;
    return true;
}
bool WriteHidden(void* actor, const Api& api, bool value) {
    if (!api.set_hidden || api.set_hidden_value < 0) return false;
    alignas(16) std::array<uint8_t, kParams> args{};
    args[static_cast<size_t>(api.set_hidden_value)] = value ? 1 : 0;
    g_process_event(actor, api.set_hidden, args.data());
    bool observed = false;
    return ReadHidden(actor, api, observed) && observed == value;
}
Api Resolve(void* actor) {
    const auto* header = static_cast<const UObjectHeader*>(actor);
    if (!g_process_event || !IsReadable(header, sizeof(*header))) return {};
    if (const auto it = s_api.find(header->class_ptr); it != s_api.end()) return it->second;
    Api api{};
    api.is_hidden = FindReflectedFunction(actor, L"IsHidden");
    api.set_hidden = FindReflectedFunction(actor, L"SetActorHiddenInGame");
    api.get_collision = FindReflectedFunction(actor, L"GetActorEnableCollision");
    api.set_collision = FindReflectedFunction(actor, L"SetActorEnableCollision");
    api.is_hidden_return = BoolParameter(api.is_hidden, L"ReturnValue");
    if (api.is_hidden_return < 0) api.hidden_field = ResolveHiddenField(actor);
    api.set_hidden_value = BoolParameter(api.set_hidden, L"bNewHidden");
    api.collision_return = BoolParameter(api.get_collision, L"ReturnValue");
    api.set_collision_value = BoolParameter(api.set_collision, L"bNewActorEnableCollision");
    if ((api.is_hidden_return < 0 && api.hidden_field.byte_offset < 0) ||
        api.set_hidden_value < 0 || api.collision_return < 0 ||
        api.set_collision_value < 0) return {};
    if (s_api.size() < 64) s_api.emplace(header->class_ptr, api);
    return api;
}
bool ReadBool(void* actor, void* fn, int32_t offset, bool& result) {
    if (!fn || offset < 0 || offset >= static_cast<int32_t>(kParams)) return false;
    alignas(16) std::array<uint8_t, kParams> args{};
    g_process_event(actor, fn, args.data());
    if (args[static_cast<size_t>(offset)] > 1) return false;
    result = args[static_cast<size_t>(offset)] != 0;
    return true;
}
bool WriteBool(void* actor, void* fn, int32_t offset, bool value,
               void* getter, int32_t result_offset) {
    if (!fn || offset < 0 || offset >= static_cast<int32_t>(kParams)) return false;
    alignas(16) std::array<uint8_t, kParams> args{};
    args[static_cast<size_t>(offset)] = value ? 1 : 0;
    g_process_event(actor, fn, args.data());
    bool observed = false;
    return ReadBool(actor, getter, result_offset, observed) && observed == value;
}
void Apply(Actor& actor, const GlobalObjectArray* objects, ULONGLONG now) {
    if (!Valid(actor, objects) || now < actor.retry_at || now < actor.next_check) return;
    const bool remove = Config().removed[Index(actor.kind)];
    if (!remove && !actor.captured) { actor.next_check = now + kRecheckMs; return; }
    const Api api = Resolve(actor.object);
    if (!api.set_hidden || (!api.is_hidden && api.hidden_field.byte_offset < 0) ||
        !api.get_collision || !api.set_collision) {
        actor.retry_at = now + 5000;
        if (!s_api_warning) {
            s_api_warning = true;
            Log(L"World removal: bHidden reflection or actor visibility/collision API unavailable");
        }
        return;
    }
    bool hidden = false, collision = false;
    if (!ReadHidden(actor.object, api, hidden) ||
        !ReadBool(actor.object, api.get_collision, api.collision_return, collision)) {
        actor.retry_at = now + 2000;
        return;
    }
    if (remove && !actor.captured) {
        actor.original_hidden = hidden;
        actor.original_collision = collision;
        actor.captured = true;
    }
    const bool wanted_hidden = remove ? true : actor.original_hidden;
    const bool wanted_collision = remove ? false : actor.original_collision;
    bool ok = true;
    if (hidden != wanted_hidden)
        ok = WriteHidden(actor.object, api, wanted_hidden);
    if (collision != wanted_collision)
        ok = WriteBool(actor.object, api.set_collision, api.set_collision_value,
                       wanted_collision, api.get_collision, api.collision_return) && ok;
    if (!ok) {
        actor.retry_at = now + 2000;
        if (!s_write_warning) {
            s_write_warning = true;
            Log(L"World removal: actor visibility/collision write failed read-back verification");
        }
        return;
    }
    if (!remove) actor.captured = false;
    actor.next_check = now + kRecheckMs;
}
void Discover(const GlobalObjectArray* objects) {
    ColourWorkBudget budget(500);
    const int32_t end = (std::min)(objects->num_elements, s_scan_cursor + kScanBatch);
    int32_t processed = 0;
    for (; s_scan_cursor < end; ++s_scan_cursor) {
        if ((processed & 31) == 0 && budget.expired()) break;
        ++processed;
        const auto& slot = objects->objects[s_scan_cursor];
        if (!slot.object || static_cast<uint32_t>(slot.flags) & 0x30000000u ||
            !IsReadable(slot.object, sizeof(UObjectHeader))) continue;
        const auto* header = static_cast<const UObjectHeader*>(slot.object);
        if (header->index != s_scan_cursor || static_cast<uint32_t>(header->flags) & 0x30u)
            continue;
        const WorldObjectKind kind = ClassKind(header->class_ptr);
        if (!KnownKind(kind) || !Config().removed[Index(kind)]) continue;
        if (UnrealName(header->name).rfind(L"Default__", 0) == 0) continue;
        if (const auto it = s_by_index.find(s_scan_cursor); it != s_by_index.end()) {
            Actor& existing = s_actors[it->second];
            if (existing.object == slot.object && existing.serial == slot.serial_number) continue;
            // A recycled UObject index can no longer be restored safely.
            existing = {};
            s_by_index.erase(it);
        }
        size_t target_slot = s_actors.size();
        if (target_slot >= kMaxActors) {
            const auto hole = std::find_if(s_actors.begin(), s_actors.end(),
                [](const Actor& candidate) { return !candidate.object; });
            if (hole == s_actors.end()) break;
            target_slot = static_cast<size_t>(hole - s_actors.begin());
        }
        Actor actor{};
        actor.object = slot.object;
        actor.klass = header->class_ptr;
        actor.index = s_scan_cursor;
        actor.serial = slot.serial_number;
        actor.kind = kind;
        if (!s_pending) s_apply_cursor = target_slot;
        ++s_pending;
        s_by_index.emplace(s_scan_cursor, target_slot);
        if (target_slot == s_actors.size()) s_actors.push_back(actor);
        else s_actors[target_slot] = actor;
    }
    if (s_scan_cursor >= objects->num_elements || s_actors.size() >= kMaxActors) {
        s_scan_cursor = -1;
        s_discovery_watermark.Complete(objects->objects, objects->num_elements, GetTickCount64(),
            s_actors.empty() ? 5000 : kRescanMs, s_next_scan);
        if (!s_reported) {
            s_reported = true;
            Log((L"World removal: discovered " + std::to_wstring(s_actors.size()) +
                 L" eligible native actors").c_str());
        }
    }
}
} // namespace

bool IsWorldObjectRemoved(WorldObjectKind kind) {
    return KnownKind(kind) && Config().removed[Index(kind)];
}
bool AreAllWorldObjectsRemoved() {
    const auto& flags = Config().removed;
    return std::all_of(flags.begin(), flags.end(), [](bool flag) { return flag; });
}
void SetWorldObjectRemoved(WorldObjectKind kind, bool removed) {
    if (!KnownKind(kind) || IsWorldObjectRemoved(kind) == removed) return;
    auto values = Config().removed;
    values[Index(kind)] = removed;
    SetOptions(values);
    Notify(std::wstring(L"Remove ") + kLabels[Index(kind)] +
           (removed ? L": ON (render + collision)" : L": OFF (restoring)"));
}
void SetWorldObjectRemovalProfile(const std::array<bool, kKinds>& removed) { SetOptions(removed); }
void SetAllWorldObjectsRemoved(bool removed) {
    std::array<bool, kKinds> values{};
    values.fill(removed);
    SetOptions(values);
    Notify(removed ? L"Remove world objects: all ON" : L"Remove world objects: all OFF (restoring)");
}
void ResetWorldObjectRemovals() { SetAllWorldObjectsRemoved(false); }

void UpdateWorldObjectRemovals(MyCharacter* player) {
    Config();
    const ULONGLONG now = GetTickCount64();
    if (s_save_dirty && now >= s_save_at) {
        s_save_dirty = false;
        Save();
    }
    if (!player || !g_process_event || g_restart_requested ||
        g_full_level_reload_requested || CustomMapApplying()) return;
    // World replacement invalidates all native actor handles, but replacing
    // only the player does not: the original level actors may still exist.
    void* world = nullptr;
    if (g_game_module) {
        const auto* address = reinterpret_cast<void* const*>(
            reinterpret_cast<uintptr_t>(g_game_module) + kGlobalWorldRva);
        if (IsReadable(address, sizeof(world))) std::memcpy(&world, address, sizeof(world));
    }
    if (world != s_world) {
        s_world = world;
        s_actors.clear();
        s_by_index.clear();
        s_class_kind.clear();
        s_api.clear();
        s_apply_cursor = 0;
        s_pending = 0;
        s_scan_cursor = AnyRemoved() ? 0 : -1;
        s_next_scan = 0;
        s_api_warning = s_write_warning = false;
    }
    if (player != s_player) {
        s_player = player;
        s_next_scan = 0;
        if (AnyRemoved()) s_scan_cursor = 0;
    }
    const bool active = AnyRemoved();
    if (!active && s_actors.empty()) return;
    const auto* objects = Objects();
    if (!objects) return;
    if (active && s_scan_cursor < 0) {
        s_scan_cursor = s_discovery_watermark.Resume(objects->objects, objects->num_elements, now, s_next_scan);
        if (s_scan_cursor == 0) s_reported = false;
    }
    if (active && s_scan_cursor >= 0) Discover(objects);
    if (!active) s_scan_cursor = -1;
    if (s_actors.empty() || (now < s_next_work && !s_pending)) return;
    s_next_work = now + (s_pending ? 0 : 100);
    ColourWorkBudget budget(850);
    size_t handled = 0;
    while (handled < kApplyBatch && !s_actors.empty()) {
        if (handled && budget.expired()) break;
        ++handled;
        if (s_apply_cursor >= s_actors.size()) s_apply_cursor = 0;
        Actor& actor = s_actors[s_apply_cursor++];
        if (s_pending) --s_pending;
        if (!actor.object) continue;
        if (!Valid(actor, objects)) {
            s_next_scan = 0;
            s_by_index.erase(actor.index);
            actor = {};
            continue;
        }
        Apply(actor, objects, now);
    }
    // Avoid carrying stale handles indefinitely if a level is restarted.
    if (!active && !s_pending) {
        bool awaiting_restore = false;
        for (const auto& actor : s_actors) if (actor.captured) awaiting_restore = true;
        if (!awaiting_restore) {
            s_actors.clear();
            s_by_index.clear();
            s_apply_cursor = 0;
        }
    }
}
} // namespace refunct

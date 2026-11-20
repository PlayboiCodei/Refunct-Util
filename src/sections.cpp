#include "refunct/internal_api.hpp"
#include "refunct/features/tiles.hpp"
#include "refunct/section_heights.hpp"
#include "refunct/button_sync_policy.hpp"
#include "refunct/features/button_colours.hpp"
#include "refunct/colour_work_budget.hpp"
#include <commdlg.h>
#pragma comment(lib, "Comdlg32.lib")

namespace refunct {
// ---------------------------------------------------------------------------
// World > Sections: independently position the 31 BP_LevelRoot_C actors.
// Uses the same native level roots as the existing section-practice loader.
// No LevelState modification or synthetic button presses. A normal game
// restart discards section overrides. All platform heights are preset.
namespace {
struct SectionPlacementContext {
    struct Transition {
        bool active = false;
        float initial_z = 0.0f;
        float target_z = 0.0f;
        ULONGLONG started_ms = 0;
    };
    std::array<SectionPlacement, kPracticeClusterCount> modes{};
    // World-Z adjustments relative to the native Auto/Raised/Lowered targets.
    std::array<float, kPracticeClusterCount> height_offsets{};
    std::array<float, kPracticeClusterCount> x_offsets{};
    std::array<float, kPracticeClusterCount> y_offsets{};
    // Capture a section's native XY position before the first horizontal edit.
    // It remains fixed until the map is recreated, so edits never accumulate.
    std::array<Vec3, kPracticeClusterCount> xy_anchors{};
    std::array<bool, kPracticeClusterCount> xy_anchor_valid{};
    std::array<Transition, kPracticeClusterCount> transitions{};
    void* get_location_function = nullptr;
    void* set_location_function = nullptr;
    int32_t get_result_offset = -1;
    int32_t set_location_offset = -1;
    int32_t source_offset = -1;
    int last_resets = -1;
    // All 31 live SourcePosition.Z values must match the embedded presets.
    bool ready = false;
    int mismatched_preset = -1;

    // Contact/collision tracking is not the same as a native button's
    // *pressed* state. Instrument actual contacts before changing progress.
    // Cache it only for this instance of the map; discard on game restart.
    struct ButtonCollision {
        void* actor = nullptr;
        void* actor_class = nullptr;
        void* get_fn = nullptr;
        void* set_fn = nullptr;
        int32_t get_result_offset = -1;
        int32_t set_value_offset = -1;
        int section = -1;
        bool original_enabled = false;
        // BP_Button_C::IsPressed is the native lock/pressed gate, not a
        // collision flag. Its verified x86 BoolProperty was observed at
        // offset 0x2F4 on Refunct v9.30.
        int32_t pressed_offset = -1;
        uint8_t pressed_mask = 0;
        bool native_byte_bool = false;
        bool original_pressed = false;
        // Native BP_Button_C::SetActive(bool Value) controls button visuals.
        // This is deliberately separate from the working press/collision gate.
        void* visual_set_fn = nullptr;
        int32_t visual_value_offset = -1;
        bool visual_applied = false;
        bool visual_active = false;
        // Incremented after each native SetActive call performed by this mod.
        // A separate button action must invalidate previous repair attempts.
        uint32_t visual_write_serial = 0;
    };
    std::array<ButtonCollision, kRefunctButtonCount> buttons{};
    bool buttons_initialized = false;
    // Explicit World > Button Controls changes remain active until the user
    // restores default button states or the world is recreated.
    bool button_controls_modified = false;
    bool buttons_sync_applied = false;
    bool visual_sync_faulted = false;
    ULONGLONG next_collision_check = 0;
    int button_progress_when_enabled = -1;
    ULONGLONG next_button_discovery = 0;
    // Slow verification of persistent manual overrides. Native animations
    // still update every tick, independently of this timer.
    ULONGLONG next_override_check_ms = 0;
    std::wstring button_init_issue;
    ULONGLONG last_button_init_error_at = 0;

    // One-shot native button presses are processed one at a time. This is
    // intentionally separate from the persistent section-sync toggle.
    struct PressBatch {
        bool active = false;
        bool awaiting_progress = false;
        std::array<int, kRefunctButtonCount> indices{};
        int count = 0;
        int next = 0;
        int pressed = 0;
        int skipped = 0;
        int progress_before_press = -1;
        int resets_at_start = -1;
        MyCharacter* player = nullptr;  // The batch belongs to one live character.
        ULONGLONG sent_at = 0;
        ULONGLONG next_allowed_at = 0;
    } press_batch;
};
SectionPlacementContext s_sections;

// K2_GetActorLocation/K2_SetActorLocation are the Blueprint-facing native UE4
// functions. Query reflected parameter offsets; never guess an actor layout.
bool GetSectionRootLocation(void* actor, Vec3& out) {
    if (!actor || !s_sections.get_location_function ||
        s_sections.get_result_offset < 0 ||
        !IsReadable(actor, sizeof(UObjectHeader))) return false;
    alignas(16) std::array<uint8_t, 128> args{};
    g_process_event(actor, s_sections.get_location_function, args.data());
    std::memcpy(&out, args.data() + s_sections.get_result_offset, sizeof(out));
    return std::isfinite(out.x) && std::isfinite(out.y) &&
           std::isfinite(out.z) && std::abs(out.z) < 1000000.0f;
}

bool SetSectionRootLocation(void* actor, const Vec3& target) {
    if (!actor || !s_sections.set_location_function ||
        s_sections.set_location_offset < 0 ||
        !IsReadable(actor, sizeof(UObjectHeader))) return false;
    alignas(16) std::array<uint8_t, 256> args{};
    std::memcpy(args.data() + s_sections.set_location_offset, &target, sizeof(target));
    // bSweep defaults false; bTeleport defaults false. Moving the entire root
    // actor carries its attached pieces to the requested section position.
    g_process_event(actor, s_sections.set_location_function, args.data());
    Vec3 actual{};
    // ProcessEvent can succeed while K2_SetActorLocation is refused. Verify
    // the actor's final position instead of trusting a void dispatch.
    return GetSectionRootLocation(actor, actual) &&
           std::abs(actual.x - target.x) <= 3.f &&
           std::abs(actual.y - target.y) <= 3.f &&
           std::abs(actual.z - target.z) <= 3.f;
}

bool ReadSectionSource(int index, Vec3& out) {
    if (index < 0 || index >= kPracticeClusterCount ||
        !g_practice_level_class) return false;
    void* actor = g_practice_level_actors[index];
    if (!IsReadable(actor, sizeof(UObjectHeader)) ||
        static_cast<UObjectHeader*>(actor)->class_ptr != g_practice_level_class) return false;
    const int32_t offset = s_sections.source_offset;
    if (offset < 0) return false;
    const auto* addr = static_cast<const uint8_t*>(actor) + offset;
    if (!IsReadable(addr, sizeof(out))) return false;
    std::memcpy(&out, addr, sizeof(out));
    return std::isfinite(out.x) && std::isfinite(out.y) &&
           std::isfinite(out.z) && std::abs(out.z) < 100000.0f;
}

// Prepare native actor reflection and validate the 31 preset geometries.
bool PrepareSectionPlacements() {
    if (s_sections.ready) return true;
    if (!g_process_event || !g_practice_level_class ||
        !IsReadable(g_practice_level_actors[0], sizeof(UObjectHeader))) return false;
    void* first = g_practice_level_actors[0];
    void* get_fn = FindReflectedFunction(first, L"K2_GetActorLocation");
    if (!get_fn) get_fn = FindReflectedFunction(first, L"GetActorLocation");
    void* set_fn = FindReflectedFunction(first, L"K2_SetActorLocation");
    if (!set_fn) set_fn = FindReflectedFunction(first, L"SetActorLocation");
    if (!get_fn || !set_fn) {
        Log(L"World sections: reflected actor-location functions unavailable");
        return false;
    }
    const int get_offset = FindClassPropertyOffset(get_fn, L"ReturnValue");
    const int set_offset = FindClassPropertyOffset(set_fn, L"NewLocation");
    const int source_offset = FindClassPropertyOffset(g_practice_level_class, L"SourcePosition");
    if (get_offset < 0 || get_offset > 128 - static_cast<int>(sizeof(Vec3)) ||
        set_offset < 0 || set_offset > 256 - static_cast<int>(sizeof(Vec3)) ||
        source_offset < 0) {
        Log(L"World sections: required location/source reflection missing");
        return false;
    }
    s_sections.get_location_function = get_fn;
    s_sections.set_location_function = set_fn;
    s_sections.get_result_offset = get_offset;
    s_sections.set_location_offset = set_offset;
    s_sections.source_offset = source_offset;
    const LevelState* state = PracticeLevelState();
    if (!state || state->level < 0 || state->level >= kPracticeClusterCount) return false;
    for (int i = 0; i < kPracticeClusterCount; ++i) {
        Vec3 native_source{}, actual{};
        if (!ReadSectionSource(i, native_source) ||
            !GetSectionRootLocation(g_practice_level_actors[i], actual)) return false;
        float raised_z = 0.0f, lowered_z = 0.0f;
        if (!section_heights::Target(static_cast<std::size_t>(i), true,
                                     native_source.z, raised_z) ||
            !section_heights::Target(static_cast<std::size_t>(i), false,
                                     native_source.z, lowered_z)) {
            if (s_sections.mismatched_preset != i) {
                s_sections.mismatched_preset = i;
                Log((L"World sections: preset source-height mismatch at section " +
                     std::to_wstring(i + 1) + L"; movement disabled").c_str());
            }
            return false;
        }
    }
    s_sections.mismatched_preset = -1;
    s_sections.last_resets = state->resets;
    s_sections.ready = true;
    Log(L"World sections: verified all 31 embedded native section heights");
    return true;
}

bool EnsureSectionPlacementsReady() {
    // A game restart invalidates the prior world actor pointers. Detect it
    // before processing menu input (which precedes UpdateSectionPlacements).
    const LevelState* state = PracticeLevelState();
    if (s_sections.last_resets != -1 &&
        (!state || state->resets != s_sections.last_resets)) {
        s_sections = {};
        g_practice_level_actors.fill(nullptr);
        g_practice_level_class = nullptr;
        g_practice_speed_offset = -1;
    }
    if (g_practice_active && g_practice_phase != PracticePhase::Ready &&
        g_practice_phase != PracticePhase::Idle) return false;
    if (!g_practice_level_class || !g_practice_level_actors[0]) {
        if (!DiscoverPracticeButtons()) return false;
    }
    return PrepareSectionPlacements();
}

// BP_Button_C exposes three consecutive, distinct Boolean properties:
// IsPressed (0x2F4), IsFinal (0x2F5), IsOutro (0x2F6).
// The runtime diagnostics confirmed those reflected offsets and observed
// IsPressed changing as a single byte (0 <-> 1) during native contacts.
//
// Do NOT interpret bytes at an assumed offset inside UBoolProperty itself:
// its *descriptor* layout is UE4-build dependent. The previous +0x58
// descriptor assumption rejected this verified game build, leaving sync on
// WAITING. Instead, validate the reflected types, the distinct byte offsets,
// and each actor's actual 0/1 value before writing anything.
//
// Fail closed if the game updates or begins packing these Boolean fields.
bool ResolveIsPressedProperty(void* klass, int32_t& offset, uint8_t& mask,
                              bool& native_byte) {
    offset = -1;
    mask = 0;
    native_byte = false;
    if (!IsReadable(klass, sizeof(UStructHeader))) return false;
    const int32_t pressed_offset = FindClassPropertyOffset(klass, L"IsPressed");
    const int32_t final_offset   = FindClassPropertyOffset(klass, L"IsFinal");
    const int32_t outro_offset   = FindClassPropertyOffset(klass, L"IsOutro");
    // These offsets came from a real v9.30 BP_Button_C reflection dump.
    if (pressed_offset != 0x2F4 || final_offset != 0x2F5 ||
        outro_offset != 0x2F6) return false;

    bool pressed_is_bool = false;
    bool final_is_bool = false;
    bool outro_is_bool = false;
    for (int depth = 0; depth < 12 && IsReadable(klass, sizeof(UStructHeader)); ++depth) {
        const auto* cls = static_cast<const UStructHeader*>(klass);
        void* field = cls->children;
        for (int n = 0; n < 512 && IsReadable(field, sizeof(UFieldHeader)); ++n) {
            const auto* entry = static_cast<const UFieldHeader*>(field);
            const std::wstring name = UnrealName(entry->object.name);
            if (name == L"IsPressed" || name == L"IsFinal" || name == L"IsOutro") {
                if (!IsReadable(entry->object.class_ptr, sizeof(UObjectHeader)) ||
                    UnrealName(static_cast<const UObjectHeader*>(entry->object.class_ptr)->name) !=
                        L"BoolProperty") return false;
                if (name == L"IsPressed") pressed_is_bool = true;
                else if (name == L"IsFinal") final_is_bool = true;
                else outro_is_bool = true;
                if (pressed_is_bool && final_is_bool && outro_is_bool) {
                    offset = pressed_offset;
                    mask = 1;
                    native_byte = true;  // Validated against each actor below.
                    return true;
                }
            }
            field = entry->next;
        }
        klass = cls->super_struct;
    }
    if (!pressed_is_bool || !final_is_bool || !outro_is_bool) return false;
    offset = pressed_offset;
    mask = 1;
    native_byte = true;  // Validated against each actor below.
    return true;
}

bool ReadButtonPressed(const SectionPlacementContext::ButtonCollision& button,
                       bool& pressed) {
    if (!IsReadable(button.actor, sizeof(UObjectHeader)) ||
        static_cast<const UObjectHeader*>(button.actor)->class_ptr != button.actor_class ||
        button.pressed_offset != 0x2F4 || button.pressed_mask == 0) return false;
    const auto* address = static_cast<const uint8_t*>(button.actor) + button.pressed_offset;
    if (!IsReadable(address, 1)) return false;
    if (button.native_byte_bool && *address > 1) return false;
    pressed = (*address & button.pressed_mask) != 0;
    return true;
}

bool WritableButtonByte(const void* address) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(address, &mbi, sizeof(mbi)) != sizeof(mbi) ||
        mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const DWORD protection = mbi.Protect & 0xFF;
    return protection == PAGE_READWRITE || protection == PAGE_WRITECOPY ||
           protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
}

bool SetButtonPressed(const SectionPlacementContext::ButtonCollision& button,
                      bool pressed) {
    bool before = false;
    if (!ReadButtonPressed(button, before)) return false;
    if (before == pressed) return true;
    auto* address = static_cast<uint8_t*>(button.actor) + button.pressed_offset;
    if (!WritableButtonByte(address)) return false;
    const uint8_t current = *address;
    // Native C++ bools use 0/1; packed Blueprint bools use the reflected bit.
    const uint8_t next = button.native_byte_bool ? (pressed ? 1 : 0) :
        static_cast<uint8_t>((current & ~button.pressed_mask) |
                             (pressed ? button.pressed_mask : 0));
    *address = next;
    bool actual = false;
    return ReadButtonPressed(button, actual) && actual == pressed;
}

// Refunct v9.30 exposes BP_Button_C::SetActive(bool Value). Diagnostics
// confirmed Value at offset 0 and the Blueprint's scratch-frame fields at
// offsets 4, 16, 144. The function also calls K2_SetRelativeLocation, so it
// is a better visual-state entry point than manually changing mesh material.
// Validate the exact observed signature before calling this native function.
bool ResolveButtonVisualSetter(void* actor, void*& out_fn, int32_t& value_offset) {
    out_fn = nullptr;
    value_offset = -1;
    void* fn = FindReflectedFunction(actor, L"SetActive");
    if (!IsReadable(fn, sizeof(UStructHeader))) return false;
    if (FindClassPropertyOffset(fn, L"Value") != 0 ||
        FindClassPropertyOffset(fn, L"CallFunc_MakeVector_ReturnValue") != 4 ||
        FindClassPropertyOffset(fn, L"CallFunc_K2_SetRelativeLocation_SweepHitResult") != 16 ||
        FindClassPropertyOffset(fn, L"CallFunc_K2_SetRelativeLocation_SweepHitResult2") != 144)
        return false;
    // Verify Value is genuinely a reflected BoolProperty, not just a field
    // whose name happens to match. The other offsets are signature guards.
    void* field = static_cast<const UStructHeader*>(fn)->children;
    bool value_is_bool = false;
    for (int i = 0; i < 64 && IsReadable(field, sizeof(UFieldHeader)); ++i) {
        const auto* item = static_cast<const UFieldHeader*>(field);
        if (UnrealName(item->object.name) == L"Value") {
            const void* kind = item->object.class_ptr;
            value_is_bool = IsReadable(kind, sizeof(UObjectHeader)) &&
                UnrealName(static_cast<const UObjectHeader*>(kind)->name) == L"BoolProperty";
            break;
        }
        field = item->next;
    }
    if (!value_is_bool) return false;
    out_fn = fn;
    value_offset = 0;
    return true;
}

// Runs only on a visual state transition, not every 250ms sync pass. Never
// directly presses a button or edits the LevelState. A guard catches any
// unexpected progression side effect so the test can stop safely.
bool SetButtonVisualActive(SectionPlacementContext::ButtonCollision& button,
                           bool active, int one_based_button) {
    if (!button.visual_set_fn || button.visual_value_offset != 0 ||
        !g_process_event || !IsReadable(button.actor, sizeof(UObjectHeader)) ||
        static_cast<const UObjectHeader*>(button.actor)->class_ptr != button.actor_class)
        return false;
    const LevelState* state = PracticeLevelState();
    if (!state) return false;
    const LevelState before = *state;
    // The reflected event has additional Blueprint local/scratch structs.
    // Supply more than the observed highest offset instead of a bool-sized
    // buffer, avoiding the earlier ProcessEvent argument-size hazard.
    alignas(16) std::array<uint8_t, 512> args{};
    args[button.visual_value_offset] = active ? 1u : 0u;
    g_process_event(button.actor, button.visual_set_fn, args.data());
    const LevelState* after = PracticeLevelState();
    if (!after || after->buttons != before.buttons || after->level != before.level ||
        after->platforms != before.platforms || after->cubes != before.cubes ||
        after->resets != before.resets ||
        after->end_seconds != before.end_seconds ||
        after->end_partial_seconds != before.end_partial_seconds) {
        Log(L"Button sync visual: SetActive changed progress unexpectedly; "
            L"visual updates stopped, restart the map");
        return false;
    }
    button.visual_applied = true;
    button.visual_active = active;
    ++button.visual_write_serial;
    Log((L"Button sync visual: SetActive button " +
         std::to_wstring(one_based_button) +
         (active ? L" Value=ON" : L" Value=OFF")).c_str());
    return true;
}

bool SetButtonCollision(const SectionPlacementContext::ButtonCollision& button, bool enable);

void SyncButtonVisuals(const std::array<bool, kPracticeClusterCount>& raised,
                       int completed_buttons) {
    if (s_sections.visual_sync_faulted) return;
    int updated = 0;
    for (int i = completed_buttons; i < kRefunctButtonCount; ++i) {
        auto& button = s_sections.buttons[i];
        // Never visually reactivate an already-pressed button. Its normal
        // animation and material stay under the game's control.
        const bool wanted = raised[button.section];
        if (!button.visual_set_fn) continue;
        if (!wanted && !button.visual_applied) continue;
        if (button.visual_applied && button.visual_active == wanted) continue;
        if (!SetButtonVisualActive(button, wanted, i + 1)) {
            s_sections.visual_sync_faulted = true;
            Notify(L"Button visuals failed; restart map (see RefunctUtility.log)");
            return;
        }
        // If SetActive also updates the native IsPressed gate or collision,
        // preserve the independently verified button-sync contact policy.
        bool actual_pressed = false;
        const bool desired_pressed = button_sync_policy::DesiredIsPressed(
            i, completed_buttons, wanted);
        if (!ReadButtonPressed(button, actual_pressed) ||
            (actual_pressed != desired_pressed &&
             !SetButtonPressed(button, desired_pressed))) {
            s_sections.visual_sync_faulted = true;
            Log(L"Button sync visual: unable to preserve IsPressed gate after SetActive");
            Notify(L"Button visual state conflict; restart map");
            return;
        }
        alignas(16) std::array<uint8_t, 128> collision_args{};
        if (!button.get_fn || button.get_result_offset < 0 ||
            button.get_result_offset >= 128 || !g_process_event) {
            s_sections.visual_sync_faulted = true;
            return;
        }
        g_process_event(button.actor, button.get_fn, collision_args.data());
        if ((collision_args[button.get_result_offset] != 0) != wanted &&
            !SetButtonCollision(button, wanted)) {
            s_sections.visual_sync_faulted = true;
            Log(L"Button sync visual: unable to preserve native collision after SetActive");
            Notify(L"Button visual collision conflict; restart map");
            return;
        }
        ++updated;
    }
    if (updated != 0)
        Log((L"Button sync visual: applied native SetActive to " +
             std::to_wstring(updated) + L" button(s)").c_str());
}

// The menu and game tick can attempt discovery at different points in a
// level's lifetime. Keep the precise reason, but throttle recurring errors.
bool ButtonInitFailure(const wchar_t* reason) {
    const ULONGLONG now = GetTickCount64();
    if (s_sections.button_init_issue != reason ||
        now - s_sections.last_button_init_error_at >= 10000) {
        s_sections.button_init_issue = reason;
        s_sections.last_button_init_error_at = now;
        Log((std::wstring(L"Button sync: waiting: ") + reason).c_str());
    }
    return false;
}

// UE4 reflected collision functions target only the 37 native button actors.
// Preflight the full actor->section mapping before modifying anything.
// Refuse to write unverified offsets or scan GUObjectArray each frame.
bool InitializeSectionButtons() {
    if (s_sections.buttons_initialized) {
        bool same_actors = g_practice_button_actors_ready;
        for (int i = 0; same_actors && i < kRefunctButtonCount; ++i) {
            const auto& cached = s_sections.buttons[i];
            same_actors = cached.actor == g_practice_button_actors[i] &&
                          IsReadable(cached.actor, sizeof(UObjectHeader)) &&
                          static_cast<const UObjectHeader*>(cached.actor)->class_ptr == cached.actor_class;
        }
        if (same_actors) return true;
        s_sections.buttons_initialized = false;
        s_sections.buttons_sync_applied = false;
    }
    if (!g_process_event) return ButtonInitFailure(L"ProcessEvent unavailable");
    // The platform preset may already be READY even though the separate
    // button-actor cache was invalidated during a restart. Recover it here.
    if (!g_practice_button_actors_ready) {
        if (!DiscoverPracticeButtons())
            return ButtonInitFailure(L"37 button actors not discovered yet");
        // Discovery replaces actor pointers. Revalidate the platform actors
        // and geometry before applying a single button-state write.
        s_sections.ready = false;
        if (!PrepareSectionPlacements())
            return ButtonInitFailure(L"platform actors changed during discovery");
    }
    if (!g_practice_level_class)
        return ButtonInitFailure(L"BP_LevelRoot_C class unavailable");

    std::array<SectionPlacementContext::ButtonCollision, kRefunctButtonCount> candidate{};
    int32_t buttons_offset = FindClassPropertyOffset(g_practice_level_class, L"Buttons");
    if (buttons_offset < 0 || buttons_offset >= 0x10000)
        return ButtonInitFailure(L"BP_LevelRoot_C.Buttons reflection unavailable");
    int count = 0;
    for (int section = 0; section < kPracticeClusterCount; ++section) {
        void* root = g_practice_level_actors[section];
        if (!IsReadable(root, sizeof(UObjectHeader)) ||
            static_cast<const UObjectHeader*>(root)->class_ptr != g_practice_level_class)
            return ButtonInitFailure(L"section root pointer unavailable");
        const auto* address = static_cast<const uint8_t*>(root) + buttons_offset;
        if (!IsReadable(address, sizeof(UObjectPointerArray)))
            return ButtonInitFailure(L"section button-array pointer unreadable");
        UObjectPointerArray array{};
        std::memcpy(&array, address, sizeof(array));
        if (array.count < 0 || array.count > 16 || array.capacity < array.count ||
            (array.count != 0 && (!array.data || !IsReadable(array.data, sizeof(void*) * array.count))))
            return ButtonInitFailure(L"invalid section button-array bounds");
        for (int j = 0; j < array.count; ++j) {
            if (count >= kRefunctButtonCount ||
                array.data[j] != g_practice_button_actors[count])
                return ButtonInitFailure(L"button order differs from discovery cache");
            void* actor = array.data[j];
            if (!IsReadable(actor, sizeof(UObjectHeader)))
                return ButtonInitFailure(L"button actor pointer unavailable");
            const auto* klass = static_cast<const UObjectHeader*>(actor)->class_ptr;
            if (!IsReadable(klass, sizeof(UStructHeader)))
                return ButtonInitFailure(L"button actor class unavailable");
            auto& entry = candidate[count];
            entry.actor = actor;
            entry.actor_class = const_cast<void*>(static_cast<const void*>(klass));
            entry.section = section;
            entry.get_fn = FindReflectedFunction(actor, L"GetActorEnableCollision");
            entry.set_fn = FindReflectedFunction(actor, L"SetActorEnableCollision");
            if (!entry.get_fn || !entry.set_fn)
                return ButtonInitFailure(L"Get/SetActorEnableCollision function missing");
            entry.get_result_offset = FindClassPropertyOffset(entry.get_fn, L"ReturnValue");
            entry.set_value_offset = FindClassPropertyOffset(entry.set_fn, L"bNewActorEnableCollision");
            if (entry.get_result_offset < 0 || entry.get_result_offset >= 128 ||
                entry.set_value_offset < 0 || entry.set_value_offset >= 128)
                return ButtonInitFailure(L"actor collision parameter reflection missing");
            alignas(16) std::array<uint8_t, 128> get_args{};
            g_process_event(actor, entry.get_fn, get_args.data());
            entry.original_enabled = get_args[entry.get_result_offset] != 0;
            if (!ResolveIsPressedProperty(entry.actor_class, entry.pressed_offset,
                                          entry.pressed_mask, entry.native_byte_bool))
                return ButtonInitFailure(L"IsPressed/IsFinal/IsOutro reflected Boolean layout mismatch");
            if (!ReadButtonPressed(entry, entry.original_pressed))
                return ButtonInitFailure(L"IsPressed native value is not a readable 0/1 byte");
            // A failed visual-signature check must never disable the already
            // working collision/IsPressed sync. The visual path is optional.
            ResolveButtonVisualSetter(actor, entry.visual_set_fn,
                                      entry.visual_value_offset);
            ++count;
        }
    }
    if (count != kRefunctButtonCount)
        return ButtonInitFailure(L"expected 37 buttons; got different actor count");
    s_sections.button_init_issue.clear();
    s_sections.buttons = candidate;
    s_sections.buttons_initialized = true;
    s_sections.next_collision_check = 0;
    s_sections.visual_sync_faulted = false;
    const int visual_functions = static_cast<int>(std::count_if(
        s_sections.buttons.begin(), s_sections.buttons.end(),
        [](const auto& b) { return b.visual_set_fn != nullptr; }));
    Log((L"Button sync visual: verified SetActive signature on " +
         std::to_wstring(visual_functions) + L"/37 button actors").c_str());
    Log(L"World sections: verified IsPressed and collision for 37 native button actors");
    return true;
}

bool SetButtonCollision(const SectionPlacementContext::ButtonCollision& button, bool enable) {
    if (!button.actor || !button.set_fn || !button.get_fn || !g_process_event ||
        button.set_value_offset < 0 || button.set_value_offset >= 128 ||
        button.get_result_offset < 0 || button.get_result_offset >= 128 ||
        !IsReadable(button.actor, sizeof(UObjectHeader)) ||
        static_cast<const UObjectHeader*>(button.actor)->class_ptr != button.actor_class)
        return false;
    alignas(16) std::array<uint8_t, 128> args{};
    args[button.set_value_offset] = enable ? 1 : 0;
    g_process_event(button.actor, button.set_fn, args.data());
    // Reflected setters can fail without a return value. Verify the result.
    alignas(16) std::array<uint8_t, 128> verify{};
    g_process_event(button.actor, button.get_fn, verify.data());
    return (verify[button.get_result_offset] != 0) == enable;
}

// Physical-height mode only accepts sections at their verified raised Z.
// A platform still rising is considered DOWN until it reaches the target.
bool IsSectionPhysicallyRaised(int index, bool& up) {
    up = false;
    if (!s_sections.ready || index < 0 || index >= kPracticeClusterCount) return false;
    Vec3 source{}, actual{};
    return ReadSectionSource(index, source) &&
           GetSectionRootLocation(g_practice_level_actors[index], actual) &&
           section_heights::IsPhysicallyRaised(static_cast<std::size_t>(index),
                                               source.z,
                                               actual.z - s_sections.height_offsets[index], up);
}

// A native contact is allowed only when its platform is fully raised.
// Collision alone is insufficient: Refunct starts *future* buttons with
// BP_Button_C::IsPressed=true, then unlocks the next button by setting it
// false. Preserve legitimately completed buttons while synchronising all
// 37 remaining buttons to their parent platform's physical height.
bool SyncSectionButtons(bool force) {
    if (!g_buttons_sync_with_sections || !s_sections.ready) return false;
    const ULONGLONG now = GetTickCount64();
    if (!s_sections.buttons_initialized) {
        s_sections.buttons_sync_applied = false;
        if (!force && now < s_sections.next_button_discovery) return false;
        s_sections.next_button_discovery = now + 2500;
        if (!InitializeSectionButtons()) return false;
    }
    if (!InitializeSectionButtons()) return false;
    if (!force && now < s_sections.next_collision_check)
        return s_sections.buttons_sync_applied;
    s_sections.buttons_sync_applied = false;
    const LevelState* state = PracticeLevelState();
    if (!state || state->buttons < 0 || state->buttons > kRefunctButtonCount) return false;

    std::array<bool, kPracticeClusterCount> raised{};
    for (int section = 0; section < kPracticeClusterCount; ++section)
        if (!IsSectionPhysicallyRaised(section, raised[section])) return false;

    std::array<bool, kRefunctButtonCount> old_collisions{};
    std::array<bool, kRefunctButtonCount> old_pressed{};
    std::array<bool, kRefunctButtonCount> desired_collision{};
    std::array<bool, kRefunctButtonCount> desired_pressed{};
    // All 37 targets must be valid *before* changing any actor.
    for (int i = 0; i < kRefunctButtonCount; ++i) {
        const auto& button = s_sections.buttons[i];
        if (!button.actor || button.section < 0 ||
            button.section >= kPracticeClusterCount || !button.get_fn ||
            button.get_result_offset < 0 || button.get_result_offset >= 128 ||
            !ReadButtonPressed(button, old_pressed[i])) return false;
        alignas(16) std::array<uint8_t, 128> args{};
        g_process_event(button.actor, button.get_fn, args.data());
        old_collisions[i] = args[button.get_result_offset] != 0;
        desired_collision[i] = raised[button.section];
        // Already-completed buttons stay pressed/consumed, even if up.
        desired_pressed[i] = button_sync_policy::DesiredIsPressed(
            i, state->buttons, raised[button.section]);
    }
    for (int i = 0; i < kRefunctButtonCount; ++i) {
        const auto& button = s_sections.buttons[i];
        // Lock first when lowering. Unlock last when raising, after collision.
        bool ok = true;
        if (desired_pressed[i]) {
            if (old_pressed[i] != desired_pressed[i])
                ok = SetButtonPressed(button, true);
            if (ok && old_collisions[i] != desired_collision[i])
                ok = SetButtonCollision(button, desired_collision[i]);
        } else {
            if (old_collisions[i] != desired_collision[i])
                ok = SetButtonCollision(button, desired_collision[i]);
            if (ok && old_pressed[i] != desired_pressed[i])
                ok = SetButtonPressed(button, false);
        }
        if (ok) continue;
        // Undo every prior write (and the possibly partial failing write).
        bool rollback_ok = true;
        for (int j = i; j >= 0; --j) {
            rollback_ok = SetButtonPressed(s_sections.buttons[j], old_pressed[j]) && rollback_ok;
            rollback_ok = SetButtonCollision(s_sections.buttons[j], old_collisions[j]) && rollback_ok;
        }
        Log(rollback_ok ? L"Button sync: transaction failed; restored native states" :
                          L"Button sync: transaction failed; rollback incomplete (restart map)");
        s_sections.next_collision_check = now + 2500;
        return false;
    }
    s_sections.next_collision_check = now + 250;
    s_sections.buttons_sync_applied = true;
    // Native visuals are a separate, best-effort path. If SetActive fails,
    // keep the proven button press/contact behaviour available.
    SyncButtonVisuals(raised, state->buttons);
    return true;
}

// The public reset is defined outside the anonymous namespace so it can be
// called by the full-level-reload feature in restart.cpp.

// Calculate the target against the actual game state, without changing
// LevelState or pressing any button.
bool SectionTargetZ(int index, SectionPlacement mode, float& out) {
    if (!s_sections.ready || index < 0 ||
        index >= kPracticeClusterCount) return false;
    const LevelState* state = PracticeLevelState();
    if (!state) return false;
    const bool raised_by_game = index <= state->level;
    const bool raised = mode == SectionPlacement::Raised ||
        (mode == SectionPlacement::Auto && raised_by_game);
    Vec3 source{};
    if (!ReadSectionSource(index, source) ||
        !section_heights::Target(static_cast<std::size_t>(index), raised,
                                 source.z, out)) return false;
    out += s_sections.height_offsets[index];
    // Guard against overflow/invalid native positions.
    return std::isfinite(out) && std::abs(out) <= 100000.0f;
}

// Start at the real height (even when interrupting an unfinished animation).
// Animation only changes the actor's vertical position, not its game progress.
bool MoveSectionToMode(int index, SectionPlacement mode, bool animate,
                       bool preserve_auto_z = false) {
    if (index < 0 || index >= kPracticeClusterCount) return false;
    float target_z = 0.0f;
    Vec3 current{};
    if (!SectionTargetZ(index, mode, target_z) ||
        !GetSectionRootLocation(g_practice_level_actors[index], current)) return false;
    // X/Y-only edits must not skip a native automatic rise. Preserve the
    // current Z when the section is still controlled by Refunct's Auto mode.
    if (preserve_auto_z && mode == SectionPlacement::Auto)
        target_z = current.z;
    auto& transition = s_sections.transitions[index];
    Vec3 target = current;
    if (s_sections.xy_anchor_valid[index]) {
        const Vec3& anchor = s_sections.xy_anchors[index];
        target.x = anchor.x + s_sections.x_offsets[index];
        target.y = anchor.y + s_sections.y_offsets[index];
        if (!std::isfinite(target.x) || !std::isfinite(target.y) ||
            std::abs(target.x) > 10000000.0f ||
            std::abs(target.y) > 10000000.0f) return false;
    }
    const bool move_xy = std::abs(target.x - current.x) > 1.0f ||
                         std::abs(target.y - current.y) > 1.0f;
    const bool move_z = std::abs(current.z - target_z) > 1.0f;
    if (!move_xy && !move_z) {
        transition.active = false;
        return true;
    }
    if (animate && move_z) {
        // Horizontal edits are immediate. Only the vertical rise animates.
        if (move_xy && !SetSectionRootLocation(g_practice_level_actors[index], target))
            return false;
        transition = {true, current.z, target_z, GetTickCount64()};
        return true;
    }
    transition.active = false;
    target.z = target_z;
    return SetSectionRootLocation(g_practice_level_actors[index], target);
}

// Smoothstep, 1.1 seconds, based on wall clock so Game Speed does not
// unexpectedly stall the UI animation. All bulk changes start together.
constexpr ULONGLONG kSectionTransitionMs = 1100;
void AdvanceSectionTransition(int index, ULONGLONG now) {
    auto& motion = s_sections.transitions[index];
    if (!motion.active) return;
    Vec3 current{};
    if (!GetSectionRootLocation(g_practice_level_actors[index], current)) {
        motion.active = false;
        Log(L"World sections: animation cancelled (section actor unavailable)");
        return;
    }
    const auto elapsed = now >= motion.started_ms ? now - motion.started_ms : 0;
    const double duration_ms = std::max(1.0,
        static_cast<double>(kSectionTransitionMs) * 100.0 /
        static_cast<double>(std::max(g_section_speed_percent, 25)));
    const float t = static_cast<float>(
        std::min(1.0, static_cast<double>(elapsed) / duration_ms));
    const float eased = t * t * (3.0f - 2.0f * t);
    current.z = motion.initial_z + (motion.target_z - motion.initial_z) * eased;
    if (!SetSectionRootLocation(g_practice_level_actors[index], current)) {
        motion.active = false;
        Log(L"World sections: animation cancelled (unable to move section)");
        return;
    }
    if (t >= 1.0f) motion.active = false;
}

} // namespace

// Called on full map reload and on native run resets. Clears all cached
// section and button pointers before the old UWorld is discarded.
void ResetSectionPlacementInternals() {
    s_sections = {};
}

// The tile module uses the same verified raised-height criteria as button sync.
// Query all 31 on the game thread, before initiating a batch of tile contacts.
bool SnapshotRaisedSections(std::array<bool, kPracticeClusterCount>& raised) {
    if (!EnsureSectionPlacementsReady()) return false;
    for (int i = 0; i < kPracticeClusterCount; ++i) {
        if (!IsSectionPhysicallyRaised(i, raised[i])) return false;
    }
    return true;
}

// Intercept the real native contact. Do not synthesize presses here: native
// press events also change progression. Block contacts on lowered platforms
// and collect before/after evidence of native progress changes.
void __fastcall SectionButtonContactHook(void* actor, void* /*edx*/, MyCharacter* player) {
    if (!g_original_add_based_character) return;
    // Ignore only physical contacts with native BP_IslandChunk_C tile actors
    // when the tile-selection toggle is enabled. Existing button/section
    // gates still run normally for every other ALiftBase actor.
    if (player && ShouldBlockPhysicalTileContact(actor, player)) return;
    const bool guard_button_tiles = BeginButtonTileSelectionGuard(actor, player);
    // This callback can register several consecutive buttons before the
    // next CharacterTick. Save each genuine native progress edge, including
    // when section-button synchronisation is switched off.
    const auto invoke_native = [&]() {
        int before = -1;
        // A custom map can trigger more than one button between character
        // ticks. Always sample the native counter at the contact itself,
        // independently of PB timer setup and section-sync preferences.
        if (player && !IsSyntheticPracticeButtonContact() &&
            (!g_practice_active || g_practice_phase == PracticePhase::Ready)) {
            if (const LevelState* progress = PracticeLevelState())
                before = progress->buttons;
        }
        g_original_add_based_character(actor, player);
        NotifyNativeTileContactCompleted(actor, player);
        if (before >= 0) {
            if (const LevelState* progress = PracticeLevelState()) {
                const int after = progress->buttons;
                CaptureGlobalLastButtonContact(player, before, after);
                CaptureNativeButtonSplit(player, before, after, actor);
            }
        }
    };
    if (!g_section_button_contact_hook_available ||
        !g_buttons_sync_with_sections || IsSyntheticPracticeButtonContact() ||
        !s_sections.buttons_initialized || !actor || !player) {
        invoke_native();
        if (guard_button_tiles) EndButtonTileSelectionGuard();
        return;
    }
    const SectionPlacementContext::ButtonCollision* matched = nullptr;
    for (int i = 0; i < kRefunctButtonCount; ++i) {
        if (s_sections.buttons[i].actor == actor) {
            matched = &s_sections.buttons[i];
            break;
        }
    }
    if (!matched) {
        invoke_native();
        if (guard_button_tiles) EndButtonTileSelectionGuard();
        return;
    }
    bool allowed = true;
    // Unknown geometry must fail open; never break vanilla interaction based
    // on stale actor pointers or a missing reflected function.
    if (!IsSectionPhysicallyRaised(matched->section, allowed)) allowed = true;
    if (allowed) invoke_native();
    if (guard_button_tiles) EndButtonTileSelectionGuard();
}

bool SetButtonsSyncWithSections(bool enabled) {
    if (enabled == g_buttons_sync_with_sections) return true;
    if (enabled) {
        if (!g_section_button_contact_hook_available) {
            Notify(L"Native button hook unavailable; see RefunctUtility.log");
            return false;
        }
        if (const LevelState* state = PracticeLevelState())
            s_sections.button_progress_when_enabled = state->buttons;
        // Enabling the preference must not depend on button actors having
        // finished spawning. The game tick already retries discovery safely.
        g_buttons_sync_with_sections = true;
        g_recording_tainted = true;
        if (EnsureSectionPlacementsReady() && SyncSectionButtons(true)) {
            Notify(L"Button sync ON: native button gates ready");
        } else {
            Log(L"Button sync: enabled pending native button initialization");
            Notify(L"Button sync ON (WAITING); see RefunctUtility.log");
        }
        return true;
    } else {
        if (s_sections.buttons_initialized) {
            const LevelState* state = PracticeLevelState();
            if (!state || state->buttons < 0 ||
                state->buttons > kRefunctButtonCount) return false;
            // Revert the model appearance before restoring the native gate:
            // SetActive may itself change IsPressed or collision.
            if (!s_sections.visual_sync_faulted) {
                for (int i = state->buttons; i < kRefunctButtonCount; ++i) {
                    auto& button = s_sections.buttons[i];
                    if (button.visual_applied &&
                        !SetButtonVisualActive(button, i == state->buttons, i + 1)) {
                        Log(L"Button sync visual: unable to restore vanilla appearance");
                        Notify(L"Button visuals: restart map to restore appearance");
                        break;
                    }
                }
            }
            // The vanilla game keeps only the next sequential button
            // unpressed/unlocked. Reconstruct this after optional out-of-order
            // interactions; the original per-button snapshots can be stale.
            for (int i = 0; i < kRefunctButtonCount; ++i) {
                const auto& button = s_sections.buttons[i];
                const bool vanilla_pressed = button_sync_policy::VanillaIsPressed(
                    i, state->buttons);
                if (!SetButtonPressed(button, vanilla_pressed)) {
                    Notify(L"Button sync: native state restore failed; restart map");
                    return false;
                }
                // If no progress changed, the cached collision states are
                // still the genuine native baseline. Otherwise restart is
                // recommended because native collision changes are event-based.
                if (s_sections.button_progress_when_enabled == state->buttons &&
                    !SetButtonCollision(button, button.original_enabled)) {
                    Notify(L"Button sync: collision restore failed; restart map");
                    return false;
                }
            }
            if (s_sections.button_progress_when_enabled != state->buttons)
                Notify(L"Button sync OFF; restart map to restore native collision");
            else Notify(L"Button sync OFF");
        }
        g_buttons_sync_with_sections = false;
        s_sections.buttons_initialized = false;
        s_sections.buttons_sync_applied = false;
    }
    g_recording_tainted = true;
    MarkCheatAction(CheatAction::Buttons);
    return true;
}


// One-shot native button restoration. The raised-only variant locks underwater
// buttons. The all-buttons variant restores them as well, without moving any
// sections. The DEFAULT variant instead reconstructs Refunct's sequential
// button gate: only the next unpressed button is enabled when its section is
// physically above water. Completed and future buttons remain locked.
// No synthetic contacts or direct LevelState writes are made here.
namespace {
bool RestoreButtonsImpl(bool include_underwater, bool restore_default) {
    if (s_sections.press_batch.active) {
        Notify(L"Restore buttons: finish Press All before restoring");
        return false;
    }
    if (!EnsureSectionPlacementsReady() || !InitializeSectionButtons()) {
        Notify(L"Restore buttons: native button actors are unavailable");
        return false;
    }
    const LevelState* state = PracticeLevelState();
    if (!state || state->buttons < 0 || state->buttons > kRefunctButtonCount) {
        Notify(L"Restore buttons: invalid progress state");
        return false;
    }
    const int completed = state->buttons;

    // Decide from the physical native root height, never from the override
    // mode or sequential progress. Treat sections still rising as underwater.
    std::array<bool, kPracticeClusterCount> raised{};
    for (int section = 0; section < kPracticeClusterCount; ++section) {
        if (!IsSectionPhysicallyRaised(section, raised[section])) {
            Log((L"Restore buttons: failed to read section " +
                 std::to_wstring(section + 1) + L" height").c_str());
            Notify(L"Restore buttons: platform heights are unavailable");
            return false;
        }
    }

    struct SavedButtonState {
        bool pressed = false;
        bool collision = false;
        bool visual_applied = false;
        bool visual_active = false;
    };
    std::array<SavedButtonState, kRefunctButtonCount> previous{};
    std::array<bool, kRefunctButtonCount> wanted_active{};
    int restored_buttons = 0;
    int previously_pressed_restored = 0;

    // Validate *all 37* actors before changing any one of them. In particular,
    // do not skip buttons with index < LevelState.buttons: those are precisely
    // the previously pressed buttons that Restore Buttons must reset.
    for (int i = 0; i < kRefunctButtonCount; ++i) {
        const auto& button = s_sections.buttons[i];
        auto& saved = previous[i];
        if (!button.actor || button.section < 0 ||
            button.section >= kPracticeClusterCount ||
            !button.visual_set_fn || button.visual_value_offset != 0 ||
            !button.get_fn || !button.set_fn ||
            button.get_result_offset < 0 || button.get_result_offset >= 128 ||
            button.set_value_offset < 0 || button.set_value_offset >= 128 ||
            button.pressed_offset != 0x2F4 ||
            !ReadButtonPressed(button, saved.pressed) ||
            !WritableButtonByte(static_cast<const uint8_t*>(button.actor) +
                                button.pressed_offset)) {
            Log((L"Restore buttons: native state preflight failed for button " +
                 std::to_wstring(i + 1)).c_str());
            Notify(L"Restore buttons: a native button property is unavailable");
            return false;
        }
        alignas(16) std::array<uint8_t, 128> args{};
        g_process_event(button.actor, button.get_fn, args.data());
        saved.collision = args[button.get_result_offset] != 0;
        saved.visual_applied = button.visual_applied;
        saved.visual_active = button.visual_active;
        // DEFAULT means vanilla sequential progress, not "enable every
        // visible button". The current next button is usable only once its
        // own section has fully risen. The other restore modes are unchanged.
        wanted_active[i] = restore_default
            ? (i == completed && raised[button.section])
            : (include_underwater || raised[button.section]);
        if (wanted_active[i]) {
            ++restored_buttons;
            if (i < completed) ++previously_pressed_restored;
        }
    }

    // A native visual call may have side effects, so invalidate PB eligibility
    // before the first attempted mutation, including failure/rollback cases.
    g_recording_tainted = true;
    MarkCheatAction(CheatAction::Buttons);
    if (restore_default) g_run_pb_eligible = false;
    int touched = 0;
    bool success = true;
    for (int i = 0; i < kRefunctButtonCount; ++i) {
        auto& button = s_sections.buttons[i];
        const bool active = wanted_active[i];
        touched = i + 1;

        // Always call SetActive for buttons being restored. The cached visual flag is
        // not reliable after the player actually presses a button: native
        // animations may sink the mesh without updating our cached flag.
        if (!SetButtonVisualActive(button, active, i + 1)) {
            success = false;
            break;
        }
        // SetActive may change IsPressed or collision; set and verify the
        // intended native gate *after* applying the visual state.
        if (active) {
            if (!SetButtonCollision(button, true) ||
                !SetButtonPressed(button, false)) {
                success = false;
                break;
            }
        } else {
            if (!SetButtonPressed(button, true) ||
                !SetButtonCollision(button, false)) {
                success = false;
                break;
            }
        }
        bool actual_pressed = false;
        alignas(16) std::array<uint8_t, 128> args{};
        g_process_event(button.actor, button.get_fn, args.data());
        if (!ReadButtonPressed(button, actual_pressed) ||
            actual_pressed == active ||
            (args[button.get_result_offset] != 0) != active) {
            success = false;
            break;
        }
    }

    if (!success) {
        // Best-effort rollback includes the actor whose setter failed; a
        // native Blueprint call may partially change its visual state.
        bool rollback_ok = true;
        for (int i = touched - 1; i >= 0; --i) {
            auto& button = s_sections.buttons[i];
            const auto& saved = previous[i];
            const bool prior_visual = saved.visual_applied
                ? saved.visual_active : (i == completed);
            rollback_ok = SetButtonVisualActive(button, prior_visual, i + 1) && rollback_ok;
            if (!saved.visual_applied) {
                button.visual_applied = false;
                button.visual_active = false;
            }
            rollback_ok = SetButtonPressed(button, saved.pressed) && rollback_ok;
            rollback_ok = SetButtonCollision(button, saved.collision) && rollback_ok;
        }
        Log((L"Restore buttons: failed at button " + std::to_wstring(touched) +
             (rollback_ok ? L"; native gates rolled back" :
                            L"; rollback incomplete (restart map)")).c_str());
        Notify(L"Restore buttons failed; restart map if appearance is incorrect");
        return false;
    }

    // Track explicit World button overrides without counting a completed
    // Restore Default operation as an active World setting.
    s_sections.button_controls_modified = !restore_default;
    // A one-shot restoration must not be undone by periodic section sync,
    // whose policy intentionally keeps already completed buttons consumed.
    const bool sync_was_on = g_buttons_sync_with_sections;
    g_buttons_sync_with_sections = false;
    s_sections.buttons_sync_applied = false;
    s_sections.button_progress_when_enabled = -1;
    SaveSettings();
    // A vanilla restore must not reactivate completed buttons or unlock
    // future buttons. Keep the existing bulk-restore behaviour intact.
    const int disabled_buttons = kRefunctButtonCount - restored_buttons;
    if (restore_default) {
        Log((L"Restore DEFAULT buttons: progression " + std::to_wstring(completed) +
             L"/37; active " + std::to_wstring(restored_buttons) +
             L"; locked " + std::to_wstring(disabled_buttons) +
             L"; continuous sync " +
             (sync_was_on ? L"disabled" : L"already off")).c_str());
        Notify(restored_buttons != 0
            ? L"Default buttons restored: next button active; others locked"
            : L"Default buttons restored: no available button at current height");
    } else {
        Log((L"Restore buttons: activated " + std::to_wstring(restored_buttons) +
             (include_underwater ? L" buttons including underwater (" : L" raised buttons (") +
             std::to_wstring(previously_pressed_restored) +
             L" previously pressed); disabled " +
             std::to_wstring(disabled_buttons) +
             L" on submerged sections; continuous sync " +
             (sync_was_on ? L"disabled" : L"already off")).c_str());
        Notify(L"Restore buttons: " + std::to_wstring(restored_buttons) +
               (include_underwater ? L" total ON, " : L" above water ON, ") +
               std::to_wstring(disabled_buttons) +
               L" underwater OFF");
    }
    return true;
}
} // namespace

bool RestoreButtons(bool include_underwater) {
    return RestoreButtonsImpl(include_underwater, false);
}

bool RestoreDefaultButtons() {
    return RestoreButtonsImpl(false, true);
}

// Automatic ghost-button recovery observes all 37 buttons while a World
// gameplay option is active. The explicit manual repair pass always works.
// Healthy/future/completed buttons are never automatically modified.
namespace {
// Set only while a broken native button is temporarily backing off.
ULONGLONG s_button_fix_retry_after_ms = 0;
bool s_button_fix_faulted = false;
ULONGLONG s_button_fix_next_check = 0;
int s_button_fix_last_resets = -1;
std::array<void*, kRefunctButtonCount> s_button_fix_actor_cache{};
std::array<bool, kRefunctButtonCount> s_button_fix_repaired{};
std::array<bool, kRefunctButtonCount> s_button_fix_state_known{};
std::array<bool, kRefunctButtonCount> s_button_fix_prior_pressed{};
std::array<bool, kRefunctButtonCount> s_button_fix_prior_collision{};
// A suspected fault is trustworthy only if the CURRENT button was previously
// observed pressable during this run. Never guess from an initially locked gate.
std::array<bool, kRefunctButtonCount> s_button_fix_seen_healthy{};
std::array<uint32_t, kRefunctButtonCount> s_button_fix_visual_serial{};
// Only a confirmed native-gate mismatch on the current expected button can
// trigger automatic recovery. Proximity to a healthy button is NOT a fault.
// The contact-only ghost case needs manual intervention until a dependable
// signal exists that contact actually failed.
std::array<ULONGLONG, kRefunctButtonCount> s_button_fix_near_since{};
std::array<ULONGLONG, kRefunctButtonCount> s_button_fix_last_repair_ms{};
bool s_button_fix_manual_pending = false;
bool s_button_fix_auto_scan_active = false;
unsigned s_button_fix_manual_repairs = 0;
ULONGLONG s_button_fix_next_status_log = 0;
unsigned s_button_fix_attempts = 0;

void ResetButtonFixCache() {
    s_button_fix_actor_cache.fill(nullptr);
    s_button_fix_repaired.fill(false);
    s_button_fix_state_known.fill(false);
    s_button_fix_seen_healthy.fill(false);
    s_button_fix_visual_serial.fill(0);
    s_button_fix_near_since.fill(0);
    s_button_fix_last_repair_ms.fill(0);
    s_button_fix_manual_pending = false;
    s_button_fix_auto_scan_active = false;
    s_button_fix_manual_repairs = 0;
    s_button_fix_last_resets = -1;
    s_button_fix_next_check = 0;
    s_button_fix_next_status_log = 0;
    s_button_fix_attempts = 0;
    s_button_fix_faulted = false;
    s_button_fix_retry_after_ms = 0;
}

// BP_Button_C owns two possible physical contact meshes. Setting only actor
// collision to ON does not refresh their physics bodies after a visual reset.
// Refresh the *existing* query-enabled collision state without inventing a
// new profile. NoCollision components intentionally remain NoCollision.
// Reflection and post-write verification protect unknown game versions.
bool RefreshButtonContactComponent(
    const SectionPlacementContext::ButtonCollision& button,
    const wchar_t* property, int one_based_index) {
    if (!button.actor_class || !g_process_event) return false;
    const int member = FindClassPropertyOffset(button.actor_class, property);
    if (member < 0 || member > 0x4000 ||
        !IsReadable(static_cast<const uint8_t*>(button.actor) + member, sizeof(void*)))
        return true;  // This optional mesh does not exist in this Blueprint.
    void* component = nullptr;
    std::memcpy(&component, static_cast<const uint8_t*>(button.actor) + member,
                sizeof(component));
    if (!IsReadable(component, sizeof(UObjectHeader))) return false;

    void* get_collision = FindReflectedFunction(component, L"GetCollisionEnabled");
    void* set_collision = FindReflectedFunction(component, L"SetCollisionEnabled");
    if (!IsReadable(get_collision, sizeof(UStructHeader)) ||
        !IsReadable(set_collision, sizeof(UStructHeader))) return false;
    const int collision_result = FindClassPropertyOffset(get_collision, L"ReturnValue");
    const int collision_arg = FindClassPropertyOffset(set_collision, L"NewType");
    if (collision_result < 0 || collision_result >= 128 ||
        collision_arg < 0 || collision_arg >= 128) return false;
    const auto read_mode = [&]() -> int {
        alignas(16) std::array<uint8_t, 128> params{};
        g_process_event(component, get_collision, params.data());
        return params[collision_result] <= 3 ? params[collision_result] : -1;
    };
    const auto write_mode = [&](uint8_t mode) -> bool {
        alignas(16) std::array<uint8_t, 128> params{};
        params[collision_arg] = mode;
        g_process_event(component, set_collision, params.data());
        return read_mode() == mode;
    };
    const int mode = read_mode();
    if (mode < 0) return false;
    if (mode == 0 || mode == 2) {
        // NoCollision and PhysicsOnly are not query contact components. The
        // game's intended collision profile is unknown; never guess one.
        return true;
    }

    // Preserve the exact original mode; toggling it forces physics state and
    // based-character contact registration to refresh. A failure restores it.
    if (!write_mode(0)) {
        write_mode(static_cast<uint8_t>(mode));
        return false;
    }
    if (!write_mode(static_cast<uint8_t>(mode))) {
        Log((L"Button fix: button " + std::to_wstring(one_based_index) +
             L" component collision restore failed; restart map").c_str());
        return false;
    }
    Log((L"Button fix: refreshed " + std::wstring(property) + L" collision on button " +
         std::to_wstring(one_based_index) + L" (mode=" +
         std::to_wstring(mode) + L")").c_str());
    return true;
}

// Do not call these setters on locked or visually disabled buttons.
bool RefreshButtonContactComponents(
    const SectionPlacementContext::ButtonCollision& button, int one_based_index) {
    // The Base mesh is where the native BasedCharacter callback is expected.
    // Core and Base may use different profiles, so preserve each independently.
    return RefreshButtonContactComponent(button, L"Button base", one_based_index) &&
           RefreshButtonContactComponent(button, L"Button", one_based_index);
}

// A visual SetActive/actor collision mismatch can leave a button visible but
// impossible to touch. Native Reenable() is the only validated Blueprint event
// available for rebuilding its internal interaction. Never call it on a button
// that is currently locked or deliberately disabled by the user's menu.
bool RepairUsableButton(int index) {
    if (index < 0 || index >= kRefunctButtonCount || !g_process_event) return false;
    auto& button = s_sections.buttons[index];
    if (!button.actor || button.actor != g_practice_button_actors[index] ||
        !IsReadable(button.actor, sizeof(UObjectHeader)) ||
        static_cast<const UObjectHeader*>(button.actor)->class_ptr != button.actor_class ||
        !button.visual_set_fn || button.visual_value_offset != 0 ||
        !button.get_fn || !button.set_fn ||
        button.get_result_offset < 0 || button.get_result_offset >= 128 ||
        button.set_value_offset < 0 || button.set_value_offset >= 128 ||
        button.pressed_offset != 0x2F4 ||
        !WritableButtonByte(static_cast<const uint8_t*>(button.actor) +
                            button.pressed_offset)) return false;

    // Never guess the Blueprint ProcessEvent ABI after a game update.
    void* reenable = FindReflectedFunction(button.actor, L"Reenable");
    if (!IsReadable(reenable, sizeof(UStructHeader))) {
        Log(L"Button fix: reflected Reenable() unavailable");
        return false;
    }
    void* field = static_cast<const UStructHeader*>(reenable)->children;
    int traversed = 0;
    while (field && traversed < 64) {
        if (!IsReadable(field, sizeof(UFieldHeader))) return false;
        const auto* item = static_cast<const UFieldHeader*>(field);
        if (!IsReadable(item->object.class_ptr, sizeof(UObjectHeader))) return false;
        const std::wstring kind = UnrealName(
            static_cast<const UObjectHeader*>(item->object.class_ptr)->name);
        if (kind.find(L"Property") != std::wstring::npos) {
            Log(L"Button fix: Reenable() parameter signature differs; not called");
            return false;
        }
        field = item->next;
        ++traversed;
    }
    if (field) return false;

    bool prior_pressed = true;
    if (!ReadButtonPressed(button, prior_pressed)) return false;
    alignas(16) std::array<uint8_t, 128> collision_args{};
    g_process_event(button.actor, button.get_fn, collision_args.data());
    const bool prior_collision = collision_args[button.get_result_offset] != 0;

    const bool visual_applied = button.visual_applied;
    const bool visual_active = button.visual_active;
    const LevelState* state = PracticeLevelState();
    if (!state) return false;
    const LevelState before = *state;
    g_recording_tainted = true;
    g_run_pb_eligible = false;

    // Native SetActive can reposition the mesh and reset its internal gates.
    // Apply it BEFORE Reenable(), so the native reenable event is the last
    // Blueprint interaction-reset operation rather than being overwritten.
    const bool visual_restored = SetButtonVisualActive(button, true, index + 1);
    if (!visual_restored) {
        Log((L"Button fix: button " + std::to_wstring(index + 1) +
             L" SetActive failed before Reenable").c_str());
        return false;
    }
    alignas(16) std::array<uint8_t, 512> args{};
    g_process_event(button.actor, reenable, args.data());
    const LevelState* after = PracticeLevelState();
    if (!after || after->buttons != before.buttons ||
        after->level != before.level || after->platforms != before.platforms ||
        after->cubes != before.cubes || after->resets != before.resets ||
        after->end_seconds != before.end_seconds ||
        after->end_partial_seconds != before.end_partial_seconds) {
        Log(L"Button fix: Reenable() changed native progression; repairs halted");
        return false;
    }

    // Reenable can update IsPressed/collision. Reapply only the native gates,
    // never SetActive again (which can undo the interaction reset).
    bool ok = RefreshButtonContactComponents(button, index + 1) &&
              SetButtonCollision(button, true) &&
              SetButtonPressed(button, false);
    // Re-registering an existing collision body can generate a native contact.
    // Never claim a repair succeeded if that advanced the level unexpectedly.
    const LevelState* post_refresh = PracticeLevelState();
    if (!post_refresh || post_refresh->buttons != before.buttons ||
        post_refresh->resets != before.resets ||
        post_refresh->level != before.level) {
        Log(L"Button fix: contact refresh changed native progression; stopping repairs");
        return false;
    }
    bool verified_pressed = true;
    alignas(16) std::array<uint8_t, 128> verify{};
    if (ok) {
        g_process_event(button.actor, button.get_fn, verify.data());
        ok = ReadButtonPressed(button, verified_pressed) && !verified_pressed &&
             verify[button.get_result_offset] != 0;
    }
    if (!ok) {
        // Best-effort rollback: never edit LevelState or synthesize a press.
        if (visual_applied) SetButtonVisualActive(button, visual_active, index + 1);
        else {
            button.visual_applied = false;
            button.visual_active = false;
        }
        const bool restored_pressed = SetButtonPressed(button, prior_pressed);
        const bool restored_collision = SetButtonCollision(button, prior_collision);
        Log((L"Button fix: button " + std::to_wstring(index + 1) +
             (restored_pressed && restored_collision
                 ? L" verification failed; gates rolled back"
                 : L" rollback incomplete; restart map")).c_str());
        return false;
    }
    // Report the verified repair without reading unrelated component state.
    if (s_button_fix_manual_pending)
        MarkCheatAction(CheatAction::Buttons);
    else
        MarkAutomaticButtonRepair(index + 1);
    Log((L"Button fix: button " + std::to_wstring(index + 1) +
         L" native gates restored").c_str());
    return true;
}

// A position check is used only AFTER the native pressed/collision gates are
// observed broken. Merely standing near an enabled button is normal gameplay.
bool ButtonFixPlayerIsNear(const SectionPlacementContext::ButtonCollision& button) {
    if (!g_player || !IsReadable(g_player, sizeof(MyCharacter)) ||
        !IsReadable(g_player->root_component, sizeof(SceneComponent))) return false;
    Vec3 target{};
    if (!GetSectionRootLocation(button.actor, target)) return false;
    const Vec3 player = g_player->root_component->location;
    return std::isfinite(player.x) && std::isfinite(player.y) &&
           std::isfinite(player.z) &&
           std::abs(player.x - target.x) <= 115.0f &&
           std::abs(player.y - target.y) <= 115.0f &&
           std::abs(player.z - target.z) <= 185.0f;
}

// Read only already-resolved option state. This does not inspect UE4 actors,
// and runs only on the existing 400 ms button-monitor cadence. Cosmetic-only
// changes (sky/fog/material colours) are intentionally not World gameplay
// options. A manual ghost-button repair bypasses this gate.
bool AnyWorldGameplayOptionActive() {
    if (s_sections.button_controls_modified || g_buttons_sync_with_sections ||
        !g_animate_section_changes || IsSectionSpeedModified() ||
        g_air_jump_enabled || IsWorldCubeOverrideActive() ||
        IsButtonTileSelectionDisabled() || IsTileSelectionOnContactDisabled() ||
        IsTubeTravelSpeedModified() || IsPlatformSpeedModified() ||
        IsJumpPadHeightModified() || IsAllWaterRemoved() || IsWaterRemoved() ||
        IsWaterSwimmingDisabled() || GetWaterHeightOffset() != 0 ||
        CustomMapActive() || g_keep_last_button_momentum ||
        !g_progress_rise_animation_enabled || g_preloaded_restart_active ||
        g_progress_restart_active) return true;
    for (unsigned i = 0; i < static_cast<unsigned>(WorldObjectKind::Count); ++i)
        if (IsWorldObjectRemoved(static_cast<WorldObjectKind>(i))) return true;
    for (int i = 0; i < kPracticeClusterCount; ++i) {
        if (s_sections.modes[i] != SectionPlacement::Auto ||
            s_sections.x_offsets[i] != 0.0f ||
            s_sections.y_offsets[i] != 0.0f ||
            s_sections.height_offsets[i] != 0.0f) return true;
    }
    return false;
}

// Runs on the game thread, throttled. Full 37-button read-only observation is
// restored when any World gameplay option is active. Automatic *writes* still
// require evidence that the current expected button regressed from healthy,
// is physically raised, and the player remained nearby. Manual repair remains
// available with all World options at their defaults.
void UpdateButtonFix() {
    // Deliberately removed buttons have collision disabled on purpose, not
    // because of a ghost-button regression. Do not auto-repair them.
    if (IsWorldObjectRemoved(WorldObjectKind::Buttons)) return;
    if (s_sections.press_batch.active || !g_process_event ||
        g_full_level_reload_requested || g_restart_requested ||
        g_preloaded_restart_requested || g_practice_restart_requested ||
        (g_practice_active && g_practice_phase != PracticePhase::Ready &&
         g_practice_phase != PracticePhase::Idle)) return;
    const ULONGLONG now = GetTickCount64();
    // A failed reflection/contact attempt must not permanently disable the
    // automatic feature now that there is no ON/OFF toggle. Retry at most
    // once per ten seconds, and reset immediately on a fresh map or manual pass.
    if (s_button_fix_faulted) {
        if (now < s_button_fix_retry_after_ms) return;
        s_button_fix_faulted = false;
    }
    if (!s_sections.ready) {
        if (now >= s_button_fix_next_status_log) {
            s_button_fix_next_status_log = now + 10000;
            Log(L"Button fix: ON, waiting for section placement discovery");
        }
        return;
    }
    if (now < s_button_fix_next_check) return;
    s_button_fix_next_check = now + 400;
    if (!s_button_fix_manual_pending && !AnyWorldGameplayOptionActive()) {
        // A later ON transition must re-observe a healthy native gate before
        // calling a regression a ghost. Inactive periods cannot supply proof.
        if (s_button_fix_auto_scan_active) {
            s_button_fix_repaired.fill(false);
            s_button_fix_state_known.fill(false);
            s_button_fix_seen_healthy.fill(false);
            s_button_fix_near_since.fill(0);
        }
        s_button_fix_auto_scan_active = false;
        return;
    }
    if (!s_button_fix_manual_pending) s_button_fix_auto_scan_active = true;
    if (!InitializeSectionButtons()) {
        if (now >= s_button_fix_next_status_log) {
            s_button_fix_next_status_log = now + 10000;
            Log((L"Button fix: ON, waiting for button discovery: " +
                 s_sections.button_init_issue).c_str());
        }
        return;
    }
    const LevelState* state = PracticeLevelState();
    if (!state || state->buttons < 0 || state->buttons > kRefunctButtonCount)
        return;
    if (state->resets != s_button_fix_last_resets) {
        s_button_fix_actor_cache.fill(nullptr);
        s_button_fix_repaired.fill(false);
        s_button_fix_state_known.fill(false);
        s_button_fix_seen_healthy.fill(false);
        s_button_fix_near_since.fill(0);
        s_button_fix_last_repair_ms.fill(0);
        s_button_fix_last_resets = state->resets;
    }
    int attempted = 0;
    int eligible_count = 0;
    int remaining = 0;
    for (int i = 0; i < kRefunctButtonCount; ++i) {
        // Observe all buttons when a World option is active (or on manual
        // request). The stricter current-button checks below still prevent
        // automatic writes to future or completed native buttons.
        auto& button = s_sections.buttons[i];
        if (s_button_fix_actor_cache[i] != button.actor) {
            s_button_fix_actor_cache[i] = button.actor;
            s_button_fix_repaired[i] = false;
            s_button_fix_state_known[i] = false;
            s_button_fix_seen_healthy[i] = false;
            s_button_fix_near_since[i] = 0;
            s_button_fix_last_repair_ms[i] = 0;
        }
        bool pressed = true;
        if (!ReadButtonPressed(button, pressed) || !button.get_fn ||
            button.get_result_offset < 0 || button.get_result_offset >= 128) {
            s_button_fix_repaired[i] = false;
            continue;
        }
        alignas(16) std::array<uint8_t, 128> args{};
        g_process_event(button.actor, button.get_fn, args.data());
        const bool collision = args[button.get_result_offset] != 0;
        // The old toggle would repair once, then permanently skip this actor.
        // Invalidate after any later native gate transition or SetActive call,
        // e.g. Restore All -> Disable All -> Restore Default. Do not repeat
        // Reenable each frame while the observed state remains unchanged.
        const bool changed = !s_button_fix_state_known[i] ||
            s_button_fix_prior_pressed[i] != pressed ||
            s_button_fix_prior_collision[i] != collision ||
            s_button_fix_visual_serial[i] != button.visual_write_serial;
        if (changed) {
            s_button_fix_repaired[i] = false;
            s_button_fix_near_since[i] = 0;  // A new fault needs a full dwell.
        }
        s_button_fix_state_known[i] = true;
        s_button_fix_prior_pressed[i] = pressed;
        s_button_fix_prior_collision[i] = collision;
        s_button_fix_visual_serial[i] = button.visual_write_serial;
        const bool explicitly_disabled = button.visual_applied && !button.visual_active;
        const bool native_enabled = !pressed && collision;
        const bool visually_enabled = button.visual_applied && button.visual_active;
        // A vanilla next button is expected to become pressable once its
        // section is raised. Future buttons and already-consumed buttons are
        // never automatically repaired, even if a visual appears active.
        const bool current_button = i == state->buttons;
        bool physically_raised = false;
        const bool raised = current_button &&
            IsSectionPhysicallyRaised(button.section, physically_raised) && physically_raised;
        const bool vanilla_next = raised && !button.visual_applied;
        const bool restored_completed = i < state->buttons &&
            visually_enabled && native_enabled;
        const bool eligible = !explicitly_disabled &&
            (i >= state->buttons
                ? (native_enabled || visually_enabled || vanilla_next)
                : restored_completed);
        if (!eligible) {
            s_button_fix_repaired[i] = false;
            s_button_fix_near_since[i] = 0;
            continue;
        }
        ++eligible_count;
        if (!s_button_fix_manual_pending) {
            // Crucial: a healthy next button (including button 20 / 18.1)
            // MUST NOT be reset simply because the player is near it.
            if (native_enabled) {
                if (current_button) s_button_fix_seen_healthy[i] = true;
                s_button_fix_repaired[i] = true;
                s_button_fix_near_since[i] = 0;
                continue;
            }
            // Only a gate which WAS observed healthy for the current button
            // can be called a regression. An initially locked button may be
            // intentional (e.g. multi-button sections); fail safe.
            if (!s_button_fix_seen_healthy[i]) {
                s_button_fix_near_since[i] = 0;
                continue;
            }
            // Only the genuinely broken gate of the current expected button
            // is a candidate. Never alter an intentionally disabled button,
            // an unfinished section, or a future/completed button.
            if (!current_button || !raised || s_button_fix_repaired[i]) {
                s_button_fix_near_since[i] = 0;
                continue;
            }
            if (!ButtonFixPlayerIsNear(button)) {
                s_button_fix_near_since[i] = 0;
                continue;
            }
            if (!s_button_fix_near_since[i]) {
                s_button_fix_near_since[i] = now;
                continue;
            }
            if (now - s_button_fix_near_since[i] < 2000 ||
                now - s_button_fix_last_repair_ms[i] < 12000) continue;
            // Log the native evidence BEFORE any mutation, distinguishing a
            // real state correction from ordinary contact with the platform.
            Log((L"Button fix: AUTO native-gate fault on button " +
                 std::to_wstring(i + 1) + L"; pressed=" +
                 (pressed ? L"1" : L"0") + L" collision=" +
                 (collision ? L"1" : L"0") + L" currentProgress=" +
                 std::to_wstring(state->buttons) + L"; repair requested").c_str());
        }
        if (s_button_fix_repaired[i] && !s_button_fix_manual_pending) continue;
        ++remaining;
        if (attempted >= 4) continue; // Keep the full scan, cap mutations.
        if (!RepairUsableButton(i)) {
            s_button_fix_faulted = true;
            s_button_fix_retry_after_ms = now + 10000;
            Log((L"Button fix: failed on button " + std::to_wstring(i + 1) +
                 L"; retrying after 10-second safety cooldown").c_str());
            return;
        }
        s_button_fix_repaired[i] = true;
        s_button_fix_last_repair_ms[i] = now;
        s_button_fix_near_since[i] = 0;
        ++s_button_fix_attempts;
        if (s_button_fix_manual_pending) ++s_button_fix_manual_repairs;
        // The repair itself changes the observable state and the visual write
        // serial; store its resulting values to avoid re-arming next tick.
        s_button_fix_prior_pressed[i] = false;
        s_button_fix_prior_collision[i] = true;
        s_button_fix_visual_serial[i] = button.visual_write_serial;
        ++attempted;
    }
    if (s_button_fix_manual_pending && remaining == attempted) {
        s_button_fix_manual_pending = false;
        Notify(L"Ghost button repair complete: " +
               std::to_wstring(s_button_fix_manual_repairs) + L" checked/repaired");
        Log((L"Button fix: manual pass completed; repaired=" +
             std::to_wstring(s_button_fix_manual_repairs) +
             L", eligible=" + std::to_wstring(eligible_count)).c_str());
    }
    if (now >= s_button_fix_next_status_log) {
        s_button_fix_next_status_log = now + 10000;
        Log((L"Button fix: ON; eligible=" + std::to_wstring(eligible_count) +
             L"; total repairs=" + std::to_wstring(s_button_fix_attempts) +
             L"; native progress=" + std::to_wstring(state->buttons)).c_str());
    }
}
} // namespace

bool RepairGhostButtonsNow() {
    if (s_sections.press_batch.active || g_full_level_reload_requested ||
        g_restart_requested || g_preloaded_restart_requested ||
        g_practice_restart_requested) {
        Notify(L"Ghost button fix: finish the current action first");
        return false;
    }
    if (!EnsureSectionPlacementsReady() || !InitializeSectionButtons()) {
        Notify(L"Ghost button fix: buttons not ready; retry after level loads");
        return false;
    }
    // Request an immediate one-shot pass in addition to automatic monitoring.
    ResetButtonFixCache();
    s_button_fix_manual_pending = true;
    s_button_fix_next_check = 0;
    g_recording_tainted = true;
    g_run_pb_eligible = false;
    Notify(L"Ghost button fix: refreshing all currently usable buttons");
    Log(L"Button fix: manual all-usable repair requested");
    return true;
}

// One-shot action: lock buttons based on their physical section position.
// The native IsPressed gate, collision state, and visual state are updated
// together, without synthesizing button contacts or changing LevelState.
// Includes already-completed buttons and never changes untargeted buttons.
namespace {
enum class ButtonDisableScope { Underwater, AboveWater, All };

bool DisableButtonsByScope(ButtonDisableScope scope) {
    const wchar_t* action = scope == ButtonDisableScope::Underwater
        ? L"Disable underwater" : scope == ButtonDisableScope::AboveWater
        ? L"Disable above water" : L"Disable ALL";
    if (s_sections.press_batch.active) {
        Notify(std::wstring(action) + L": finish Press All first");
        return false;
    }
    if (!EnsureSectionPlacementsReady() || !InitializeSectionButtons()) {
        Notify(std::wstring(action) + L": native buttons not available");
        return false;
    }
    const LevelState* state = PracticeLevelState();
    if (!state || state->buttons < 0 || state->buttons > kRefunctButtonCount) {
        Notify(std::wstring(action) + L": invalid progress state");
        return false;
    }

    std::array<bool, kPracticeClusterCount> raised{};
    if (scope != ButtonDisableScope::All) {
        for (int section = 0; section < kPracticeClusterCount; ++section) {
            if (!IsSectionPhysicallyRaised(section, raised[section])) {
                Notify(std::wstring(action) + L": unable to read platform heights");
                return false;
            }
        }
    }

    struct PreviousState {
        bool pressed = false;
        bool collision = false;
        bool visual_applied = false;
        bool visual_active = false;
    };
    std::array<PreviousState, kRefunctButtonCount> previous{};
    std::array<int, kRefunctButtonCount> target_indices{};
    int count = 0;

    // Validate every targeted actor before writing ANY native button state.
    // Untargeted actors are not read or modified.
    for (int i = 0; i < kRefunctButtonCount; ++i) {
        const auto& button = s_sections.buttons[i];
        if (button.section < 0 || button.section >= kPracticeClusterCount) {
            Notify(std::wstring(action) + L": invalid section mapping");
            return false;
        }
        const bool target = scope == ButtonDisableScope::All ||
            (scope == ButtonDisableScope::AboveWater ? raised[button.section]
                                                     : !raised[button.section]);
        if (!target) continue;
        auto& saved = previous[i];
        if (!button.actor || button.actor != g_practice_button_actors[i] ||
            !button.visual_set_fn || button.visual_value_offset != 0 ||
            !button.get_fn || !button.set_fn ||
            button.get_result_offset < 0 || button.get_result_offset >= 128 ||
            button.set_value_offset < 0 || button.set_value_offset >= 128 ||
            button.pressed_offset != 0x2F4 ||
            !ReadButtonPressed(button, saved.pressed) ||
            !WritableButtonByte(static_cast<const uint8_t*>(button.actor) +
                                button.pressed_offset)) {
            Log((std::wstring(action) + L": preflight failed at button " +
                 std::to_wstring(i + 1)).c_str());
            Notify(std::wstring(action) + L": native button property unavailable");
            return false;
        }
        alignas(16) std::array<uint8_t, 128> args{};
        g_process_event(button.actor, button.get_fn, args.data());
        saved.collision = args[button.get_result_offset] != 0;
        saved.visual_applied = button.visual_applied;
        saved.visual_active = button.visual_active;
        target_indices[count++] = i;
    }
    if (count == 0) {
        Notify(std::wstring(action) + L": no matching buttons found");
        return true;
    }

    g_recording_tainted = true;
    g_run_pb_eligible = false;
    MarkCheatAction(CheatAction::Buttons);
    int touched = 0;
    bool success = true;
    for (int n = 0; n < count; ++n) {
        const int index = target_indices[n];
        auto& button = s_sections.buttons[index];
        touched = n + 1;
        // SetActive controls the visible model. Reapply the lock and collision
        // afterwards because the Blueprint setter may modify the native gates.
        if (!SetButtonVisualActive(button, false, index + 1) ||
            !SetButtonPressed(button, true) ||
            !SetButtonCollision(button, false)) {
            success = false;
            break;
        }
        bool actual_pressed = false;
        alignas(16) std::array<uint8_t, 128> args{};
        g_process_event(button.actor, button.get_fn, args.data());
        if (!ReadButtonPressed(button, actual_pressed) || !actual_pressed ||
            args[button.get_result_offset] != 0) {
            success = false;
            break;
        }
    }
    if (!success) {
        bool rollback_ok = true;
        for (int n = touched - 1; n >= 0; --n) {
            const int index = target_indices[n];
            auto& button = s_sections.buttons[index];
            const auto& saved = previous[index];
            // Native active visuals cannot always be reconstructed perfectly
            // after a game press. Restore the best known visual and gate states.
            const bool prior_visual = saved.visual_applied
                ? saved.visual_active : (index == state->buttons);
            rollback_ok = SetButtonVisualActive(button, prior_visual, index + 1) && rollback_ok;
            if (!saved.visual_applied) {
                button.visual_applied = false;
                button.visual_active = false;
            }
            rollback_ok = SetButtonPressed(button, saved.pressed) && rollback_ok;
            rollback_ok = SetButtonCollision(button, saved.collision) && rollback_ok;
        }
        Log((std::wstring(action) + L": native operation failed; " +
             std::wstring(rollback_ok ? L"rolled back" : L"rollback incomplete; restart map")).c_str());
        Notify(std::wstring(action) + L" failed; restart map if state looks wrong");
        return false;
    }

    // The explicit World button change stays active until Restore Default.
    s_sections.button_controls_modified = true;
    // Keep this a one-shot action. Otherwise periodic synchronization could
    // change the raised buttons after we deliberately left them untouched.
    const bool sync_was_on = g_buttons_sync_with_sections;
    if (sync_was_on) {
        g_buttons_sync_with_sections = false;
        s_sections.buttons_sync_applied = false;
        s_sections.button_progress_when_enabled = -1;
        SaveSettings();
    }
    const wchar_t* details = scope == ButtonDisableScope::Underwater
        ? L" underwater buttons; above-water unchanged"
        : scope == ButtonDisableScope::AboveWater
        ? L" above-water buttons; underwater unchanged"
        : L" buttons (all sections)";
    Log((std::wstring(action) + L": disabled " + std::to_wstring(count) +
         details).c_str());
    Notify(L"Disabled " + std::to_wstring(count) + details);
    return true;
}

} // namespace

bool DisableUnderwaterButtons() {
    return DisableButtonsByScope(ButtonDisableScope::Underwater);
}
bool DisableAboveWaterButtons() {
    return DisableButtonsByScope(ButtonDisableScope::AboveWater);
}
bool DisableAllButtons() {
    return DisableButtonsByScope(ButtonDisableScope::All);
}

// Queue a snapshot of currently unlocked, collision-enabled buttons. Native
// progress is monotonic; actors already counted in LevelState.buttons are not
// replayed even if Restore Buttons made them physically usable again.
bool PressAllAvailableButtons() {
    if (s_sections.press_batch.active) {
        Notify(L"Press All: already running");
        return false;
    }
    if (g_practice_active && g_practice_phase != PracticePhase::Ready &&
        g_practice_phase != PracticePhase::Idle) {
        Notify(L"Press All: finish section loading first");
        return false;
    }
    if (!g_player || !IsReadable(g_player, sizeof(MyCharacter)) ||
        !g_process_event || !g_add_based_character || !g_remove_based_character ||
        !EnsureSectionPlacementsReady() || !InitializeSectionButtons()) {
        Notify(L"Press All: native buttons/player not available");
        return false;
    }
    const LevelState* state = PracticeLevelState();
    if (!state || state->buttons < 0 || state->buttons > kRefunctButtonCount) {
        Notify(L"Press All: invalid native progress");
        return false;
    }
    SectionPlacementContext::PressBatch batch{};
    batch.resets_at_start = state->resets;
    batch.player = g_player;
    for (int i = state->buttons; i < kRefunctButtonCount; ++i) {
        const auto& button = s_sections.buttons[i];
        bool pressed = true;
        if (!button.actor || button.actor != g_practice_button_actors[i] ||
            !ReadButtonPressed(button, pressed) || !button.get_fn ||
            button.get_result_offset < 0 || button.get_result_offset >= 128) {
            Notify(L"Press All: button state unavailable");
            return false;
        }
        alignas(16) std::array<uint8_t, 128> args{};
        g_process_event(button.actor, button.get_fn, args.data());
        const bool collision = args[button.get_result_offset] != 0;
        if (!pressed && collision) batch.indices[batch.count++] = i;
    }
    if (batch.count == 0) {
        Notify(L"Press All: no unpressed, enabled buttons available");
        return false;
    }

    // Do not let the periodic height sync re-lock the snapshot mid-batch.
    // Preserve native gates exactly as they are right now.
    if (g_buttons_sync_with_sections) {
        g_buttons_sync_with_sections = false;
        s_sections.buttons_sync_applied = false;
        SaveSettings();
    }
    g_recording_tainted = true;
    g_run_pb_eligible = false;
    batch.active = true;
    s_sections.press_batch = batch;
    MarkCheatAction(CheatAction::Buttons);
    Log((L"Press All: queued " + std::to_wstring(batch.count) +
         L" currently available buttons; native progress " +
         std::to_wstring(state->buttons)).c_str());
    Notify(L"Press All: pressing " + std::to_wstring(batch.count) +
           L" available buttons (assisted)");
    return true;
}

void UpdatePressAllAvailableButtons() {
    auto& batch = s_sections.press_batch;
    if (!batch.active) return;
    const LevelState* state = PracticeLevelState();
    const ULONGLONG now = GetTickCount64();
    const auto stop = [&](const std::wstring& reason) {
        Log((L"Press All: stopped after " + std::to_wstring(batch.pressed) +
             L" presses: " + reason).c_str());
        Notify(L"Press All stopped: " + reason);
        batch = {};
    };
    if (!state || state->resets != batch.resets_at_start ||
        !g_player || g_player != batch.player ||
        !IsReadable(g_player, sizeof(MyCharacter)) ||
        g_full_level_reload_requested || g_restart_requested ||
        g_preloaded_restart_requested || g_practice_restart_requested ||
        (g_practice_active && g_practice_phase != PracticePhase::Ready &&
         g_practice_phase != PracticePhase::Idle)) {
        stop(L"game restarted or practice started");
        return;
    }
    if (batch.awaiting_progress) {
        if (state->buttons == batch.progress_before_press + 1) {
            ++batch.pressed;
            ++batch.next;
            batch.awaiting_progress = false;
            batch.next_allowed_at = now + 80;
        } else if (state->buttons != batch.progress_before_press) {
            stop(L"native progress advanced unexpectedly");
            return;
        } else if (now - batch.sent_at > 2500) {
            stop(L"native button did not activate");
            return;
        } else {
            return;  // Wait for the game to confirm the previous press.
        }
    }
    if (now < batch.next_allowed_at) return;
    // Native progress may have consumed an actor, or the game may have
    // disabled a button after the previous press. Skip rather than force it.
    while (batch.next < batch.count) {
        const int index = batch.indices[batch.next];
        if (index < state->buttons) {
            ++batch.skipped;
            ++batch.next;
            continue;
        }
        if (!s_sections.buttons_initialized ||
            s_sections.buttons[index].actor != g_practice_button_actors[index]) {
            stop(L"button actor cache changed");
            return;
        }
        const auto& button = s_sections.buttons[index];
        bool pressed = true;
        if (!ReadButtonPressed(button, pressed) || !button.get_fn ||
            button.get_result_offset < 0 || button.get_result_offset >= 128) {
            stop(L"native button state unreadable");
            return;
        }
        alignas(16) std::array<uint8_t, 128> args{};
        g_process_event(button.actor, button.get_fn, args.data());
        if (pressed || args[button.get_result_offset] == 0) {
            ++batch.skipped;
            ++batch.next;
            continue;
        }
        batch.progress_before_press = state->buttons;
        if (!ActivatePracticeButton(index, g_player)) {
            stop(L"native press failed at button " + std::to_wstring(index + 1));
            return;
        }
        batch.awaiting_progress = true;
        batch.sent_at = now;
        return;
    }
    Log((L"Press All: completed " + std::to_wstring(batch.pressed) +
         L" native presses; skipped " + std::to_wstring(batch.skipped) +
         L" no-longer-available buttons").c_str());
    Notify(L"Press All: pressed " + std::to_wstring(batch.pressed) +
           L", skipped " + std::to_wstring(batch.skipped));
    batch = {};
}

std::wstring SectionButtonSyncStatus() {
    if (!g_buttons_sync_with_sections)
        return L"Buttons sync with sections: OFF  < >";
    if (s_sections.ready && s_sections.buttons_initialized &&
        s_sections.buttons_sync_applied)
        return L"Buttons sync with sections: ON  < >";
    return L"Buttons sync with sections: ON (WAITING)  < >";
}

std::wstring SectionHeightStatus() {
    if (s_sections.mismatched_preset >= 0)
        return L"Preset heights: map mismatch at section " +
               std::to_wstring(s_sections.mismatched_preset + 1);
    return s_sections.ready ? L"Preset heights: 31/31 READY" :
                              L"Preset heights: built in (waiting for level)";
}

SectionPlacement GetSectionPlacement(int one_based_section) {
    if (one_based_section < 1 || one_based_section > kPracticeClusterCount)
        return SectionPlacement::Auto;
    return s_sections.modes[one_based_section - 1];
}

// The UI adjusts by 50 Unreal units (1 UU = 1 cm). This is an offset,
// not an absolute world-Z value: Auto/Raised/Lowered continue to use the
// same verified native geometry, with the offset added afterwards.
namespace {
constexpr float kSectionHeightStep = 50.0f;
constexpr float kSectionHeightMaxOffset = 10000.0f;

bool ApplySectionPositionOffsets(
    const std::array<float, kPracticeClusterCount>& proposed_x,
    const std::array<float, kPracticeClusterCount>& proposed_y,
    const std::array<float, kPracticeClusterCount>& proposed_z) {
    if (!EnsureSectionPlacementsReady()) {
        Notify(L"Sections unavailable: try again after the platforms settle");
        return false;
    }
    const auto old_x = s_sections.x_offsets;
    const auto old_y = s_sections.y_offsets;
    const auto old_z = s_sections.height_offsets;
    const auto old_anchors = s_sections.xy_anchors;
    const auto old_valid = s_sections.xy_anchor_valid;
    const auto old_transitions = s_sections.transitions;
    auto new_anchors = old_anchors;
    auto new_valid = old_valid;
    std::array<Vec3, kPracticeClusterCount> original_positions{};
    std::array<bool, kPracticeClusterCount> changed{};
    bool any_change = false;

    // Preflight every changed actor and its native height presets before
    // touching the world; an invalid operation must never partly move a map.
    for (int i = 0; i < kPracticeClusterCount; ++i) {
        const float x = proposed_x[i], y = proposed_y[i], z = proposed_z[i];
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) ||
            std::abs(x) > kSectionHeightMaxOffset ||
            std::abs(y) > kSectionHeightMaxOffset ||
            std::abs(z) > kSectionHeightMaxOffset) {
            Notify(L"Section position limit: +/-10000 cm per axis");
            return false;
        }
        changed[i] = x != old_x[i] || y != old_y[i] || z != old_z[i];
        if (!changed[i]) continue;
        any_change = true;
        if (!GetSectionRootLocation(g_practice_level_actors[i], original_positions[i])) {
            Notify(L"Section position unavailable: native actor missing");
            return false;
        }
        Vec3 source{};
        float raised_z = 0.0f, lowered_z = 0.0f;
        if (!ReadSectionSource(i, source) ||
            !section_heights::Target(static_cast<std::size_t>(i), true,
                                     source.z, raised_z) ||
            !section_heights::Target(static_cast<std::size_t>(i), false,
                                     source.z, lowered_z) ||
            std::abs(raised_z + z) > 100000.0f ||
            std::abs(lowered_z + z) > 100000.0f) {
            Notify(L"Section height preset mismatch; no changes applied");
            return false;
        }
        if (!new_valid[i]) {
            // The first edit must use the actual actor XY, not SourcePosition:
            // SourcePosition is primarily the game's rise/lower reference.
            new_anchors[i] = original_positions[i];
            new_valid[i] = true;
        }
        const float final_x = new_anchors[i].x + x;
        const float final_y = new_anchors[i].y + y;
        if (!std::isfinite(final_x) || !std::isfinite(final_y) ||
            std::abs(final_x) > 10000000.0f ||
            std::abs(final_y) > 10000000.0f) {
            Notify(L"Section XY position outside safe bounds");
            return false;
        }
    }
    if (!any_change) return true;

    s_sections.x_offsets = proposed_x;
    s_sections.y_offsets = proposed_y;
    s_sections.height_offsets = proposed_z;
    s_sections.xy_anchors = new_anchors;
    s_sections.xy_anchor_valid = new_valid;
    int failed_at = -1;
    for (int i = 0; i < kPracticeClusterCount; ++i) {
        if (!changed[i]) continue;
        const bool xy_only_auto = s_sections.modes[i] == SectionPlacement::Auto &&
            proposed_z[i] == old_z[i] && !old_transitions[i].active;
        if (!MoveSectionToMode(i, s_sections.modes[i], g_animate_section_changes,
                               xy_only_auto)) {
            failed_at = i;
            break;
        }
    }
    if (failed_at >= 0) {
        bool restored = true;
        for (int i = 0; i <= failed_at; ++i) {
            if (!changed[i]) continue;
            restored = SetSectionRootLocation(g_practice_level_actors[i],
                                              original_positions[i]) && restored;
        }
        s_sections.x_offsets = old_x;
        s_sections.y_offsets = old_y;
        s_sections.height_offsets = old_z;
        s_sections.xy_anchors = old_anchors;
        s_sections.xy_anchor_valid = old_valid;
        s_sections.transitions = old_transitions;
        Log(restored ? L"Section position adjustment rolled back" :
            L"Section position rollback incomplete; restart map to recover");
        Notify(restored ? L"Section movement failed; changes rolled back" :
                          L"Section movement failed; restart map to recover");
        return false;
    }
    if (const auto* state = PracticeLevelState())
        s_sections.last_resets = state->resets;
    g_recording_tainted = true;
    MarkCheatAction(CheatAction::Sections);
    if (!g_pb_cheats_allowed_for_run) g_run_pb_eligible = false;
    if (g_buttons_sync_with_sections && !SyncSectionButtons(true))
        Log(L"World sections: button sync deferred after position adjustment");
    return true;
}

bool ApplySectionHeightOffsets(
    const std::array<float, kPracticeClusterCount>& proposed) {
    return ApplySectionPositionOffsets(s_sections.x_offsets, s_sections.y_offsets,
                                       proposed);
}

} // namespace

float GetSectionHeightOffset(int one_based_section) {
    if (one_based_section < 1 || one_based_section > kPracticeClusterCount)
        return 0.0f;
    return s_sections.height_offsets[one_based_section - 1];
}

bool AdjustSectionHeightOffset(int one_based_section, int direction) {
    if (one_based_section < 1 || one_based_section > kPracticeClusterCount ||
        direction == 0) return false;
    auto proposed = s_sections.height_offsets;
    proposed[one_based_section - 1] += direction < 0 ? -kSectionHeightStep : kSectionHeightStep;
    if (!ApplySectionHeightOffsets(proposed)) return false;
    Notify(L"Section " + std::to_wstring(one_based_section) + L" height offset: " +
           std::to_wstring(static_cast<int>(proposed[one_based_section - 1])) + L" cm");
    return true;
}

bool AdjustAllSectionHeightOffsets(int direction) {
    if (direction == 0) return false;
    auto proposed = s_sections.height_offsets;
    for (float& offset : proposed)
        offset += direction < 0 ? -kSectionHeightStep : kSectionHeightStep;
    if (!ApplySectionHeightOffsets(proposed)) return false;
    Notify(direction < 0 ? L"All 31 sections lowered by 50 cm" :
                           L"All 31 sections raised by 50 cm");
    return true;
}

bool ResetSectionHeightOffset(int one_based_section) {
    if (one_based_section < 1 || one_based_section > kPracticeClusterCount)
        return false;
    auto proposed = s_sections.height_offsets;
    proposed[one_based_section - 1] = 0.0f;
    if (!ApplySectionHeightOffsets(proposed)) return false;
    Notify(L"Section " + std::to_wstring(one_based_section) +
           L" height restored to default");
    return true;
}

bool ResetAllSectionHeightOffsets() {
    std::array<float, kPracticeClusterCount> zeros{};
    if (!ApplySectionHeightOffsets(zeros)) return false;
    Notify(L"All 31 section heights restored to default");
    return true;
}

float GetSectionPositionOffset(int one_based_section, int axis) {
    if (one_based_section < 1 || one_based_section > kPracticeClusterCount)
        return 0.0f;
    const int index = one_based_section - 1;
    if (axis == 0) return s_sections.x_offsets[index];
    if (axis == 1) return s_sections.y_offsets[index];
    if (axis == 2) return s_sections.height_offsets[index];
    return 0.0f;
}

bool AdjustSectionPositionOffset(int one_based_section, int axis, int direction) {
    if (one_based_section < 1 || one_based_section > kPracticeClusterCount ||
        axis < 0 || axis > 2 || direction == 0) return false;
    auto x = s_sections.x_offsets;
    auto y = s_sections.y_offsets;
    auto z = s_sections.height_offsets;
    auto* selected = axis == 0 ? &x : axis == 1 ? &y : &z;
    (*selected)[one_based_section - 1] += direction < 0 ? -kSectionHeightStep : kSectionHeightStep;
    if (!ApplySectionPositionOffsets(x, y, z)) return false;
    const wchar_t* axis_name = axis == 0 ? L"X" : axis == 1 ? L"Y" : L"Z";
    Notify(L"Section " + std::to_wstring(one_based_section) + L" " + axis_name +
           L" offset: " + std::to_wstring(static_cast<int>((*selected)[one_based_section - 1])) +
           L" cm");
    return true;
}

bool AdjustAllSectionPositionOffsets(int axis, int direction) {
    if (axis < 0 || axis > 2 || direction == 0) return false;
    auto x = s_sections.x_offsets;
    auto y = s_sections.y_offsets;
    auto z = s_sections.height_offsets;
    auto* selected = axis == 0 ? &x : axis == 1 ? &y : &z;
    for (float& value : *selected)
        value += direction < 0 ? -kSectionHeightStep : kSectionHeightStep;
    if (!ApplySectionPositionOffsets(x, y, z)) return false;
    const wchar_t* axis_name = axis == 0 ? L"X" : axis == 1 ? L"Y" : L"Z";
    Notify(L"All 31 sections: " + std::wstring(axis_name) +
           (direction < 0 ? L" -50 cm" : L" +50 cm"));
    return true;
}

bool ResetSectionPositionOffsets(int one_based_section) {
    if (one_based_section < 1 || one_based_section > kPracticeClusterCount) return false;
    auto x = s_sections.x_offsets;
    auto y = s_sections.y_offsets;
    auto z = s_sections.height_offsets;
    x[one_based_section - 1] = 0.0f;
    y[one_based_section - 1] = 0.0f;
    z[one_based_section - 1] = 0.0f;
    if (!ApplySectionPositionOffsets(x, y, z)) return false;
    Notify(L"Section " + std::to_wstring(one_based_section) +
           L" X/Y/Z restored to default");
    return true;
}

bool ResetAllSectionPositionOffsets() {
    const std::array<float, kPracticeClusterCount> zeros{};
    if (!ApplySectionPositionOffsets(zeros, zeros, zeros)) return false;
    Notify(L"All 31 section positions restored to default");
    return true;
}

bool CycleSectionPlacement(int one_based_section, int direction) {
    if (one_based_section < 1 || one_based_section > kPracticeClusterCount) return false;
    if (!EnsureSectionPlacementsReady()) {
        Notify(L"Sections unavailable: try again after the platforms settle");
        return false;
    }
    auto& mode = s_sections.modes[one_based_section - 1];
    const auto next = static_cast<SectionPlacement>((static_cast<int>(mode) +
        (direction < 0 ? 2 : 1)) % 3);
    float original_z = 0.f;
    if (!SectionTargetZ(one_based_section - 1, SectionPlacement::Auto, original_z)) {
        Notify(L"Cannot read the current native section height");
        return false;
    }
    float target_z = 0.f;
    if (!SectionTargetZ(one_based_section - 1, next, target_z)) {
        Notify(L"Preset height mismatch for section " + std::to_wstring(one_based_section) +
               L"; see RefunctUtility.log");
        return false;
    }
    if (!MoveSectionToMode(one_based_section - 1, next, g_animate_section_changes)) {
        Notify(L"Section movement failed; see RefunctUtility.log");
        return false;
    }
    mode = next;
    if (g_buttons_sync_with_sections &&
        !SyncSectionButtons(true))
        Log(L"World sections: button rule sync deferred after individual movement");
    if (const auto* state = PracticeLevelState())
        s_sections.last_resets = state->resets;
    g_recording_tainted = true;
    MarkCheatAction(CheatAction::Sections);
    const wchar_t* label = mode == SectionPlacement::Auto ? L"AUTO" :
        (mode == SectionPlacement::Raised ? L"RAISED" : L"LOWERED");
    Notify(L"Section " + std::to_wstring(one_based_section) + L": " + label);
    return true;
}

bool SetAllSectionPlacements(SectionPlacement mode) {
    if (mode == SectionPlacement::Auto) return false;
    if (!EnsureSectionPlacementsReady()) {
        Notify(L"Sections unavailable: try again after the platforms settle");
        return false;
    }
    // All-or-nothing preflight: target AND original native state must be
    // measured. A user must always be able to return the map to AUTO.
    std::array<Vec3, kPracticeClusterCount> original_positions{};
    for (int i = 0; i < kPracticeClusterCount; ++i) {
        float target_z = 0.0f, auto_z = 0.0f;
        if (!SectionTargetZ(i, mode, target_z) ||
            !SectionTargetZ(i, SectionPlacement::Auto, auto_z)) {
            Notify(L"Missing native height for section " + std::to_wstring(i + 1) +
                   L"; map geometry may differ (no sections moved)");
            return false;
        }
        if (!GetSectionRootLocation(g_practice_level_actors[i], original_positions[i])) {
            Notify(L"Cannot move all sections: an actor is unavailable");
            return false;
        }
    }
    const auto old_modes = s_sections.modes;
    const auto old_transitions = s_sections.transitions;
    int moved = 0;
    for (int i = 0; i < kPracticeClusterCount; ++i) {
        if (!MoveSectionToMode(i, mode, g_animate_section_changes)) break;
        s_sections.modes[i] = mode;
        ++moved;
    }
    if (moved != kPracticeClusterCount) {
        bool rollback_ok = true;
        // Include the failing actor: its native write may have partially
        // succeeded even though the verified postcondition returned false.
        for (int i = 0; i <= moved && i < kPracticeClusterCount; ++i)
            rollback_ok = SetSectionRootLocation(g_practice_level_actors[i],
                                                original_positions[i]) && rollback_ok;
        s_sections.modes = old_modes;
        s_sections.transitions = old_transitions;
        Log(rollback_ok ? L"World sections: partial bulk operation rolled back" :
             L"World sections: FAILED to restore one or more sections after bulk error");
        Notify(rollback_ok ? L"Section movement failed; all changes rolled back" :
                             L"Section movement failed; restart map to recover");
        return false;
    }
    if (const auto* state = PracticeLevelState())
        s_sections.last_resets = state->resets;
    if (moved > 0) {
        g_recording_tainted = true;
        MarkCheatAction(CheatAction::Sections);
    }
    if (g_buttons_sync_with_sections &&
        moved > 0 && !SyncSectionButtons(true))
        Log(L"World sections: button rule sync deferred after bulk movement");
    Notify(mode == SectionPlacement::Raised ? L"Raised all 31 sections" :
           L"Lowered all 31 sections");
    return true;
}

void ResetAllSectionPlacements() {
    if (s_sections.ready) {
        for (int index = 0; index < kPracticeClusterCount; ++index) {
            if (s_sections.modes[index] != SectionPlacement::Auto ||
                s_sections.transitions[index].active) {
                if (!MoveSectionToMode(index, SectionPlacement::Auto,
                                       g_animate_section_changes)) {
                    Notify(L"Could not reset every section; see RefunctUtility.log");
                    return;
                }
            }
        }
    }
    s_sections.modes.fill(SectionPlacement::Auto);
    if (g_buttons_sync_with_sections &&
        !SyncSectionButtons(true))
        Log(L"World sections: button rule sync deferred after auto reset");
    if (const auto* state = PracticeLevelState())
        s_sections.last_resets = state->resets;
    g_recording_tainted = true;
    MarkCheatAction(CheatAction::Sections);
    Notify(L"Section controls: all returned to AUTO");
}

void FinishSectionPlacementAnimations() {
    if (!s_sections.ready) return;
    const LevelState* state = PracticeLevelState();
    if (!state || (s_sections.last_resets != -1 &&
                   state->resets != s_sections.last_resets)) {
        ResetSectionPlacementInternals();
        return;
    }
    for (int i = 0; i < kPracticeClusterCount; ++i) {
        auto& motion = s_sections.transitions[i];
        if (!motion.active) continue;
        Vec3 location{};
        if (GetSectionRootLocation(g_practice_level_actors[i], location)) {
            location.z = motion.target_z;
            SetSectionRootLocation(g_practice_level_actors[i], location);
        }
        motion.active = false;
    }
}

void UpdateSectionPlacements() {
    // Handle colour-picker results and restore customised materials independently
    // of section discovery and gameplay progress.
    UpdateButtonColours();
    // Persisted ON settings initialise lazily. Discovery only retries every
    // few seconds, so unavailable level actors cannot cause frame stutter.
    if (g_buttons_sync_with_sections &&
        !g_section_button_contact_hook_available) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            Log(L"World sections: saved button sync ON, but no contact hook; disabling for this launch");
            Notify(L"Section button control unavailable (see RefunctUtility.log)");
        }
        g_buttons_sync_with_sections = false;
    }
    if (!s_sections.ready) {
        // Check native actors against embedded geometry at a throttled rate.
        // No calibration or disk I/O is required.
        const ULONGLONG now = GetTickCount64();
        if (now < s_sections.next_button_discovery) return;
        s_sections.next_button_discovery = now + 2500;
        if (!EnsureSectionPlacementsReady()) return;
    }
    if (!s_sections.ready) return;
    const LevelState* state = PracticeLevelState();
    if (!state || (s_sections.last_resets != -1 &&
                   state->resets != s_sections.last_resets)) {
        ResetSectionPlacementInternals();
        return;
    }
    if (g_practice_active && g_practice_phase != PracticePhase::Ready &&
        g_practice_phase != PracticePhase::Idle) {
        if (s_sections.press_batch.active) {
            s_sections.press_batch = {};
            Log(L"Press All: canceled because practice loading started");
            Notify(L"Press All canceled: practice loading started");
        }
        return;
    }
    const ULONGLONG now = GetTickCount64();
    // Previously all 31 override targets could require reflected native calls
    // on every render frame. 20 Hz is sufficient to re-enforce manually
    // overridden positions, while animation stays at the game-tick rate.
    const bool check_override = now >= s_sections.next_override_check_ms;
    if (check_override) s_sections.next_override_check_ms = now + 50;
    for (int i = 0; i < kPracticeClusterCount; ++i) {
            if (s_sections.transitions[i].active) {
                AdvanceSectionTransition(i, now);
            } else if (check_override &&
                       (s_sections.modes[i] != SectionPlacement::Auto ||
                        s_sections.height_offsets[i] != 0.0f ||
                        s_sections.x_offsets[i] != 0.0f ||
                        s_sections.y_offsets[i] != 0.0f)) {
                // Re-enforce raised/lowered modes and custom offsets if the
                // native game moves the section itself.
                // An X/Y-only Auto override must track native section movement.
                const bool preserve_native_z =
                    s_sections.modes[i] == SectionPlacement::Auto &&
                    s_sections.height_offsets[i] == 0.0f;
                MoveSectionToMode(i, s_sections.modes[i], false, preserve_native_z);
            }
    }
    if (g_buttons_sync_with_sections && !SyncSectionButtons(false)) {
        // Only log on a low-frequency retry, not every rendered frame.
        if (now >= s_sections.next_button_discovery) {
            Log(L"World sections: native button-state sync unavailable");
            s_sections.next_button_discovery = now + 2500;
        }
    }
    UpdatePressAllAvailableButtons();
    UpdateButtonFix();
}

// ---------------------------------------------------------------------------
// World > Sections: Refunct's native BP_LevelRoot_C.Speed property controls
// normal automatic rises. With Animate Section Changes OFF, apply a high but
// finite native rise speed so new platforms snap to height in one game tick,
// without rewriting LevelState or repeatedly teleporting actor transforms.
// ON restores native speed multiplied by Section Speed. The temporary TAS
// fast-rise feature has priority and restores our currently applied rate.
namespace {
struct NativeSectionSpeed {
    void* actor = nullptr;
    float original = 0.0f;
    float last_written = 0.0f;
    bool modified = false;
};
std::array<NativeSectionSpeed, kPracticeClusterCount> s_native_section_speeds{};
ULONGLONG s_native_section_speed_next_ms = 0;
ULONGLONG s_native_section_speed_discovery_ms = 0;
int s_native_section_speed_logged_percent = -1;

// Kept below the existing validated 10M native-property threshold. This is
// much faster than any ordinary rise but cannot overflow the stored float.
constexpr float kInstantAutoRiseSpeed = 9000000.0f;

bool ValidNativeSectionSpeed(float value) {
    return std::isfinite(value) && value > 0.0f && value < 10000000.0f;
}

void SetSectionSpeedPercent(int percent) {
    if (percent == g_section_speed_percent) return;
    g_section_speed_percent = percent;
    g_recording_tainted = true;
    if (!g_pb_cheats_allowed_for_run) g_run_pb_eligible = false;
    g_practice_gold_anchor_button = 0;
    g_practice_gold_anchor_seconds = -1.0;
    s_native_section_speed_next_ms = 0;
    s_native_section_speed_logged_percent = -1;
    SaveSettings();
    Notify(L"Section speed: " + std::to_wstring(percent) + L"%" +
           (percent == 100 ? L" (restart to restore record eligibility)"
                           : L" (assisted run)"));
}
} // namespace

void SetSectionSpeedProfilePercent(int percent) { if (percent >= 25) SetSectionSpeedPercent(percent); }
int GetSectionSpeedPercent() { return g_section_speed_percent; }
bool IsSectionSpeedModified() { return g_section_speed_percent != 100; }
void AdjustSectionSpeed(int direction) {
    if (!direction) return;
    constexpr int kStep = 25;
    constexpr int kMinimum = 25;
    if (direction < 0) {
        if (g_section_speed_percent > kMinimum)
            SetSectionSpeedPercent(std::max(kMinimum, g_section_speed_percent - kStep));
    } else if (g_section_speed_percent <=
               (std::numeric_limits<int>::max)() - kStep) {
        SetSectionSpeedPercent(g_section_speed_percent + kStep);
    }
}
void ResetSectionSpeed() { SetSectionSpeedPercent(100); }

void UpdateNativeSectionSpeed() {
    if (g_restart_requested || g_full_level_reload_requested ||
        g_preloaded_restart_requested || g_practice_restart_requested ||
        g_practice_speeds_overridden) return;
    // Animated 100% speed needs no scanning unless an override must be
    // restored. Treat native pointers as disposable across map reloads.
    const bool instant_mode = !g_animate_section_changes;
    const bool pending_restore = std::any_of(s_native_section_speeds.begin(),
        s_native_section_speeds.end(),
        [](const NativeSectionSpeed& entry) { return entry.modified; });
    if (!instant_mode && g_section_speed_percent == 100 && !pending_restore) return;
    const ULONGLONG now = GetTickCount64();
    if (now < s_native_section_speed_next_ms) return;
    s_native_section_speed_next_ms = now + 100; // <= 10 lightweight sweeps/sec.
    if (!g_practice_level_class || g_practice_speed_offset < 0 ||
        !g_practice_level_actors[0]) {
        if (now < s_native_section_speed_discovery_ms) return;
        s_native_section_speed_discovery_ms = now + 2500;
        if (!DiscoverPracticeButtons() || g_practice_speed_offset < 0) return;
    }
    // Validate the entire actor set before writing any native Speed field.
    std::array<float*, kPracticeClusterCount> values{};
    for (int i = 0; i < kPracticeClusterCount; ++i) {
        void* actor = g_practice_level_actors[i];
        if (!IsReadable(actor, sizeof(UObjectHeader)) ||
            static_cast<UObjectHeader*>(actor)->class_ptr != g_practice_level_class) return;
        auto* field = reinterpret_cast<float*>(
            static_cast<uint8_t*>(actor) + g_practice_speed_offset);
        if (!IsReadable(field, sizeof(float)) || !ValidNativeSectionSpeed(*field)) return;
        values[i] = field;
    }
    for (int i = 0; i < kPracticeClusterCount; ++i) {
        auto& cached = s_native_section_speeds[i];
        const float observed = *values[i];
        if (cached.actor != g_practice_level_actors[i]) {
            cached = {g_practice_level_actors[i], observed, observed, false};
        } else if (cached.modified &&
                   std::abs(observed - cached.last_written) >
                       std::max(0.001f, std::abs(cached.last_written) * 0.0001f)) {
            // Native gameplay changed Speed independently. Rebase rather than
            // repeatedly overwriting a legitimate per-section speed change.
            cached.original = observed;
            cached.modified = false;
        } else if (!cached.modified) {
            cached.original = observed;
        }
        const double desired = instant_mode
            ? static_cast<double>(kInstantAutoRiseSpeed)
            : (g_section_speed_percent == 100)
                ? static_cast<double>(cached.original)
                : std::clamp(static_cast<double>(cached.original) *
                        static_cast<double>(g_section_speed_percent) / 100.0,
                        0.0001, 9999999.0);
        const float target = static_cast<float>(desired);
        if (std::abs(observed - target) >
                std::max(0.001f, std::abs(target) * 0.0001f)) {
            *values[i] = target;
        }
        cached.last_written = target;
        cached.modified = instant_mode || g_section_speed_percent != 100;
    }
    if (instant_mode) {
        // Instant automatic rises are a gameplay advantage. Persist the
        // eligibility loss even if the player toggles animation back ON.
        g_recording_tainted = true;
        if (!g_pb_cheats_allowed_for_run) g_run_pb_eligible = false;
    }
    const int logged_mode = instant_mode ? 0 : g_section_speed_percent;
    if (s_native_section_speed_logged_percent != logged_mode) {
        s_native_section_speed_logged_percent = logged_mode;
        if (instant_mode)
            Log(L"World sections: automatic rises set to instant (assisted run)");
        else
            Log((L"Section speed: applied " +
                 std::to_wstring(g_section_speed_percent) +
                 L"% native rise speed to 31 level roots").c_str());
    }
}

// ---------------------------------------------------------------------------
// World > Buttons > Button Colours. Runtime is separate from native button
// enable/disable/press machinery: it changes only the Button component's
// displayed material and never touches IsPressed, collisions, or progression.
// ---------------------------------------------------------------------------
namespace {
struct ButtonColourSetting {
    bool loaded = false;
    bool enabled = false;
    uint32_t rgb = 0xFF4080;
};
std::array<ButtonColourSetting, 2> s_button_colours{}; // 0=active, 1=pressed
struct ButtonTintCache {
    void* actor = nullptr;
    void* mesh = nullptr;
    void* material_instance = nullptr;
    void* source_material = nullptr; // original component material: restore on OFF
    // For the border, a tint-capable game material may differ from the original.
    // The centre uses source_material as before; these extra fields stay unused.
    void* colour_source_material = nullptr;
    FNameView colour_parameter{};
    uint32_t last_rgb = 0;
};
std::array<ButtonTintCache, kRefunctButtonCount> s_button_tint_cache{};
// Independent actor/component/MID cache: never reuse the centre button MID.
std::array<ButtonTintCache, kRefunctButtonCount> s_button_border_tint_cache{};
// Separate settings, sharing existing border material instances per actor.
std::array<ButtonColourSetting, 2> s_button_border_colours{{
    {false, false, 0xFFFFFFu}, // unpressed
    {false, false, 0xFFFFFFu}  // pressed
}};
bool s_button_border_source_logged = false;
bool s_button_border_abi_reported = false;

struct BeaconSetting {
    bool loaded = false;
    bool visible = true;
    bool custom_colour = false;
    uint32_t rgb = 0xFF2020u;
};
BeaconSetting s_beacon_setting{};
struct BeaconCache {
    void* actor = nullptr;
    void* component = nullptr;
    bool forced_hidden = false;
    bool previous_visible = false;
    ButtonTintCache tint{};
};
std::array<BeaconCache, kRefunctButtonCount> s_beacon_cache{};
bool s_beacon_visibility_reported = false;
bool s_beacon_colour_reported = false;
bool s_beacon_source_logged = false;

ULONGLONG s_button_tint_next_tick = 0;
int s_button_tint_cursor = 0;
int s_button_colour_dirty_remaining = 0;
void RequestButtonColourRefresh() {
    s_button_tint_next_tick = 0;
    s_button_tint_cursor = 0;
    s_button_colour_dirty_remaining = kRefunctButtonCount;
}
ULONGLONG s_button_tint_next_discovery = 0;
bool s_button_tint_abi_reported = false;

constexpr uint32_t kButtonPickerEmpty = 0xFFFFFFFFu;
constexpr uint32_t kButtonPickerCancel = 0x01000000u;
constexpr uint32_t kButtonPickerFailure = 0x01000001u;
std::atomic<uint32_t> s_button_picker_result{kButtonPickerEmpty};
std::atomic<bool> s_button_picker_open{false};
std::atomic<int> s_button_picker_target{0};
std::atomic<uint32_t> s_button_picker_initial{0};

std::wstring ButtonHex(uint32_t rgb) {
    wchar_t text[8]{};
    swprintf_s(text, L"#%06X", rgb & 0xFFFFFFu);
    return text;
}
void ButtonColourDiagnostic(const wchar_t* message) {
    if (s_button_tint_abi_reported) return;
    s_button_tint_abi_reported = true;
    Log((std::wstring(L"Button Colours: ") + message).c_str());
    Notify(std::wstring(L"Button colours unavailable: ") + message);
}
ButtonColourSetting& ButtonColour(bool pressed) {
    auto& setting = s_button_colours[pressed ? 1 : 0];
    if (setting.loaded || g_settings_path.empty()) return setting;
    wchar_t value[32]{};
    GetPrivateProfileStringW(L"ButtonColours", pressed ? L"PressedHex" : L"ActiveHex",
                            pressed ? L"#44DDFF" : L"#FF4080", value,
                            static_cast<DWORD>(std::size(value)), g_settings_path.c_str());
    const wchar_t* digit = value[0] == L'#' ? value + 1 : value;
    if (wcslen(digit) == 6) {
        uint32_t parsed = 0;
        bool valid = true;
        for (int i = 0; i < 6; ++i) {
            const wchar_t c = digit[i];
            const int n = c >= L'0' && c <= L'9' ? c - L'0' :
                          c >= L'A' && c <= L'F' ? c - L'A' + 10 :
                          c >= L'a' && c <= L'f' ? c - L'a' + 10 : -1;
            if (n < 0) { valid = false; break; }
            parsed = (parsed << 4) | static_cast<uint32_t>(n);
        }
        if (valid) setting.rgb = parsed;
    }
    setting.enabled = GetPrivateProfileIntW(L"ButtonColours",
        pressed ? L"PressedCustom" : L"ActiveCustom", 0,
        g_settings_path.c_str()) != 0;
    setting.loaded = true;
    return setting;
}
ButtonColourSetting& ButtonBorderColour(bool pressed) {
    auto& setting = s_button_border_colours[pressed ? 1 : 0];
    if (setting.loaded || g_settings_path.empty()) return setting;
    wchar_t legacy_hex[32]{};
    wchar_t hex[32]{};
    // Migrate the V4 single border colour on first load. Users who had the old
    // option enabled keep that colour for both states until edited separately.
    GetPrivateProfileStringW(L"ButtonColours", L"BorderHex", L"#FFFFFF", legacy_hex,
                            static_cast<DWORD>(std::size(legacy_hex)), g_settings_path.c_str());
    GetPrivateProfileStringW(L"ButtonColours",
                            pressed ? L"PressedBorderHex" : L"UnpressedBorderHex",
                            legacy_hex, hex, static_cast<DWORD>(std::size(hex)),
                            g_settings_path.c_str());
    const wchar_t* digits = hex[0] == L'#' ? hex + 1 : hex;
    if (wcslen(digits) == 6) {
        uint32_t rgb = 0;
        bool valid = true;
        for (int i = 0; i < 6; ++i) {
            const wchar_t c = digits[i];
            const int n = c >= L'0' && c <= L'9' ? c - L'0' :
                          c >= L'A' && c <= L'F' ? c - L'A' + 10 :
                          c >= L'a' && c <= L'f' ? c - L'a' + 10 : -1;
            if (n < 0) { valid = false; break; }
            rgb = (rgb << 4) | static_cast<uint32_t>(n);
        }
        if (valid) setting.rgb = rgb;
    }
    const int legacy_enabled = GetPrivateProfileIntW(
        L"ButtonColours", L"BorderCustom", 0, g_settings_path.c_str());
    setting.enabled = GetPrivateProfileIntW(L"ButtonColours",
        pressed ? L"PressedBorderCustom" : L"UnpressedBorderCustom",
        legacy_enabled, g_settings_path.c_str()) != 0;
    setting.loaded = true;
    return setting;
}
void PersistButtonBorderColour(bool pressed) {
    if (g_settings_path.empty()) return;
    const auto& setting = ButtonBorderColour(pressed);
    const auto hex = ButtonHex(setting.rgb);
    const bool ok1 = WritePrivateProfileStringW(L"ButtonColours",
        pressed ? L"PressedBorderHex" : L"UnpressedBorderHex",
        hex.c_str(), g_settings_path.c_str()) != 0;
    const bool ok2 = WritePrivateProfileStringW(L"ButtonColours",
        pressed ? L"PressedBorderCustom" : L"UnpressedBorderCustom",
        setting.enabled ? L"1" : L"0", g_settings_path.c_str()) != 0;
    if (!ok1 || !ok2) Log(L"Button border colour: failed to save preference");
}
BeaconSetting& ButtonBeaconSetting() {
    if (s_beacon_setting.loaded || g_settings_path.empty()) return s_beacon_setting;
    s_beacon_setting.visible = GetPrivateProfileIntW(
        L"ButtonColours", L"BeaconVisible", 1, g_settings_path.c_str()) != 0;
    s_beacon_setting.custom_colour = GetPrivateProfileIntW(
        L"ButtonColours", L"BeaconCustom", 0, g_settings_path.c_str()) != 0;
    wchar_t hex[32]{};
    GetPrivateProfileStringW(L"ButtonColours", L"BeaconHex", L"#FF2020",
        hex, static_cast<DWORD>(std::size(hex)), g_settings_path.c_str());
    const wchar_t* digits = hex[0] == L'#' ? hex + 1 : hex;
    if (wcslen(digits) == 6) {
        uint32_t parsed = 0;
        bool valid = true;
        for (int i = 0; i < 6; ++i) {
            const wchar_t c = digits[i];
            const int nibble = c >= L'0' && c <= L'9' ? c - L'0' :
                               c >= L'A' && c <= L'F' ? c - L'A' + 10 :
                               c >= L'a' && c <= L'f' ? c - L'a' + 10 : -1;
            if (nibble < 0) { valid = false; break; }
            parsed = (parsed << 4) | static_cast<uint32_t>(nibble);
        }
        if (valid) s_beacon_setting.rgb = parsed;
    }
    s_beacon_setting.loaded = true;
    return s_beacon_setting;
}
void PersistButtonBeaconSetting() {
    if (g_settings_path.empty()) return;
    const auto& setting = ButtonBeaconSetting();
    const auto hex = ButtonHex(setting.rgb);
    const bool a = WritePrivateProfileStringW(L"ButtonColours", L"BeaconVisible",
        setting.visible ? L"1" : L"0", g_settings_path.c_str()) != 0;
    const bool b = WritePrivateProfileStringW(L"ButtonColours", L"BeaconCustom",
        setting.custom_colour ? L"1" : L"0", g_settings_path.c_str()) != 0;
    const bool c = WritePrivateProfileStringW(L"ButtonColours", L"BeaconHex",
        hex.c_str(), g_settings_path.c_str()) != 0;
    if (!a || !b || !c) Log(L"Button beacons: failed saving settings");
}
void PersistButtonColour(bool pressed) {
    if (g_settings_path.empty()) return;
    const auto& setting = ButtonColour(pressed);
    const std::wstring colour = ButtonHex(setting.rgb);
    const bool ok1 = WritePrivateProfileStringW(L"ButtonColours",
        pressed ? L"PressedHex" : L"ActiveHex", colour.c_str(),
        g_settings_path.c_str()) != 0;
    const bool ok2 = WritePrivateProfileStringW(L"ButtonColours",
        pressed ? L"PressedCustom" : L"ActiveCustom",
        setting.enabled ? L"1" : L"0", g_settings_path.c_str()) != 0;
    if (!ok1 || !ok2) Log(L"Button Colours: failed to save preference");
}

DWORD WINAPI ButtonColourPickerThread(void*) {
    COLORREF custom[16]{};
    CHOOSECOLORW choice{};
    choice.lStructSize = sizeof(choice);
    const uint32_t rgb = s_button_picker_initial.load(std::memory_order_acquire);
    choice.rgbResult = RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
    choice.lpCustColors = custom;
    choice.Flags = CC_RGBINIT | CC_FULLOPEN | CC_ANYCOLOR;
    if (ChooseColorW(&choice)) {
        const uint32_t result =
            (static_cast<uint32_t>(GetRValue(choice.rgbResult)) << 16) |
            (static_cast<uint32_t>(GetGValue(choice.rgbResult)) << 8) |
            static_cast<uint32_t>(GetBValue(choice.rgbResult));
        s_button_picker_result.store(result, std::memory_order_release);
    } else {
        s_button_picker_result.store(
            CommDlgExtendedError() ? kButtonPickerFailure : kButtonPickerCancel,
            std::memory_order_release);
    }
    s_button_picker_open.store(false, std::memory_order_release);
    return 0;
}

// v9.30 reflection report gives BP_Button_C::Button @0x2C8,
// ButtonPressedMaterial @0x2EC and ButtonActiveMaterial @0x2F0.
// Verify runtime reflection and property types, never assume the offsets alone.
struct ButtonPropertyCache {
    void* owner_class = nullptr;
    int32_t offset = -1;
};
// These five native Blueprint fields are the only ones used for cosmetic
// customisations. Cache a fully checked class/field pair, not an ABI guess.
int ButtonPropertyIndex(const wchar_t* property) {
    constexpr const wchar_t* known[] = {
        L"Button", L"Button base", L"ButtonActiveMaterial",
        L"ButtonPressedMaterial", L"Beacon"
    };
    for (int i = 0; i < static_cast<int>(std::size(known)); ++i)
        if (wcscmp(property, known[i]) == 0) return i;
    return -1;
}
bool ButtonProperty(void* object, const wchar_t* property, int expected,
                    void*& value) {
    value = nullptr;
    if (!IsReadable(object, sizeof(UObjectHeader))) return false;
    const auto* obj = static_cast<const UObjectHeader*>(object);
    void* cls = obj->class_ptr;
    if (!IsReadable(cls, sizeof(UStructHeader))) return false;
    const int index = ButtonPropertyIndex(property);
    if (index < 0) return false;
    static std::array<ButtonPropertyCache, 5> verified{};
    ButtonPropertyCache& cached = verified[static_cast<size_t>(index)];
    int32_t actual_offset = cached.owner_class == cls ? cached.offset : -1;
    if (actual_offset < 0) {
        if (UnrealName(static_cast<const UObjectHeader*>(cls)->name) != L"BP_Button_C")
            return false;
        actual_offset = FindClassPropertyOffset(cls, property);
        if (actual_offset < 0 || actual_offset > 0x1000 ||
            (expected >= 0 && actual_offset != expected)) return false;
        bool right_type = false;
        void* current = cls;
        for (int depth = 0; depth < 16 && current && !right_type; ++depth) {
            if (!IsReadable(current, sizeof(UStructHeader))) break;
            void* field = static_cast<const UStructHeader*>(current)->children;
            for (int n = 0; n < 2048 && IsReadable(field, sizeof(UFieldHeader)); ++n) {
                const auto* f = static_cast<const UFieldHeader*>(field);
                if (UnrealName(f->object.name) == property) {
                    right_type = IsReadable(f->object.class_ptr, sizeof(UObjectHeader)) &&
                        UnrealName(static_cast<const UObjectHeader*>(f->object.class_ptr)->name)
                            == L"ObjectProperty";
                    break;
                }
                field = f->next;
            }
            current = static_cast<const UStructHeader*>(current)->super_struct;
        }
        if (!right_type) return false;
        cached = {cls, actual_offset};
    }
    if ((expected >= 0 && actual_offset != expected) ||
        !IsReadable(static_cast<const uint8_t*>(object) + actual_offset,
                    sizeof(void*))) return false;
    std::memcpy(&value, static_cast<const uint8_t*>(object) + actual_offset,
                sizeof(void*));
    return IsReadable(value, sizeof(UObjectHeader));
}

bool ButtonFunctionParameter(void* function, const wchar_t* name,
                             const wchar_t* kind, int32_t& offset) {
    offset = -1;
    if (!IsReadable(function, sizeof(UStructHeader))) return false;
    void* field = static_cast<const UStructHeader*>(function)->children;
    for (int n = 0; n < 48 && IsReadable(field, sizeof(UFieldHeader)); ++n) {
        const auto* p = static_cast<const UFieldHeader*>(field);
        if (UnrealName(p->object.name) == name &&
            IsReadable(p->object.class_ptr, sizeof(UObjectHeader)) &&
            UnrealName(static_cast<const UObjectHeader*>(p->object.class_ptr)->name) == kind) {
            offset = FindClassPropertyOffset(function, name);
            return offset >= 0 && offset < 96;
        }
        field = p->next;
    }
    return false;
}
struct ButtonMaterialCall {
    void* function = nullptr;
    int32_t first = -1;
    int32_t second = -1;
};
ButtonMaterialCall ButtonCall(void* object, int operation) {
    if (!g_process_event || !IsReadable(object, sizeof(UObjectHeader))) return {};
    void* cls = static_cast<const UObjectHeader*>(object)->class_ptr;
    if (!IsReadable(cls, sizeof(UObjectHeader))) return {};
    static std::unordered_map<void*, ButtonMaterialCall> reads, writes, tints;
    auto& cache = operation == 0 ? reads : (operation == 1 ? writes : tints);
    if (auto it = cache.find(cls); it != cache.end() &&
        IsReadable(it->second.function, sizeof(UStructHeader))) return it->second;
    ButtonMaterialCall call{};
    const wchar_t* name = operation == 0 ? L"GetMaterial" :
                          operation == 1 ? L"SetMaterial" : L"SetVectorParameterValue";
    call.function = FindReflectedFunction(object, name);
    if (!call.function && operation == 2)
        call.function = FindReflectedFunction(object, L"K2_SetVectorParameterValue");
    if (operation == 2) {
        if (!ButtonFunctionParameter(call.function, L"ParameterName", L"NameProperty", call.first) ||
            !ButtonFunctionParameter(call.function, L"Value", L"StructProperty", call.second) ||
            call.first > 88 || call.second > 80) return {};
    } else {
        if (!ButtonFunctionParameter(call.function, L"ElementIndex", L"IntProperty", call.first) ||
            !ButtonFunctionParameter(call.function, operation == 0 ? L"ReturnValue" : L"Material",
                                      L"ObjectProperty", call.second) ||
            call.first != 0 || call.second != 4) return {};
    }
    if (cache.size() < 1024) cache[cls] = call;
    return call;
}
void* ButtonGetMaterial(void* mesh) {
    if (!mesh || !g_process_event) return nullptr;
    const auto call = ButtonCall(mesh, 0);
    if (!call.function) return nullptr;
    alignas(16) std::array<uint8_t, 96> args{}; // slot zero
    g_process_event(mesh, call.function, args.data());
    void* material = nullptr;
    std::memcpy(&material, args.data() + call.second, sizeof(material));
    return IsReadable(material, sizeof(UObjectHeader)) ? material : nullptr;
}
bool ButtonSetMaterial(void* mesh, void* material) {
    if (!IsReadable(material, sizeof(UObjectHeader)) || !g_process_event)
        return false;
    const auto call = ButtonCall(mesh, 1);
    if (!call.function) return false;
    alignas(16) std::array<uint8_t, 96> args{};
    std::memcpy(args.data() + call.second, &material, sizeof(material));
    g_process_event(mesh, call.function, args.data());
    return ButtonGetMaterial(mesh) == material;
}

bool ButtonTintParameter(void* material, FNameView& parameter) {
    // The native ButtonActiveMaterial may itself be a runtime MID. Walk its
    // MaterialInstance parent chain to find the stock MIC's vector override.
    for (int depth = 0; depth < 5 && IsReadable(material, sizeof(UObjectHeader));
         ++depth) {
        void* cls = static_cast<const UObjectHeader*>(material)->class_ptr;
        if (!IsReadable(cls, sizeof(UStructHeader))) return false;
        const int32_t offset = FindClassPropertyOffset(cls, L"VectorParameterValues");
        if (offset >= 0 && offset <= 0x300 &&
            IsReadable(static_cast<const uint8_t*>(material) + offset,
                       sizeof(UObjectPointerArray))) {
            UObjectPointerArray values{};
            std::memcpy(&values, static_cast<const uint8_t*>(material) + offset,
                        sizeof(values));
            // Do not guess which parameter drives tint on a multi-vector shader.
            if (values.count > 1 || values.count < 0) return false;
            if (values.count == 1) {
                if (!values.data || !IsReadable(values.data, sizeof(parameter)))
                    return false;
                std::memcpy(&parameter, values.data, sizeof(parameter));
                const std::wstring name = UnrealName(parameter);
                return !name.empty() && name != L"None" && name.size() <= 100;
            }
        }
        const int32_t parent_offset = FindClassPropertyOffset(cls, L"Parent");
        if (parent_offset < 0 || parent_offset > 0x300 ||
            !IsReadable(static_cast<const uint8_t*>(material) + parent_offset,
                        sizeof(void*))) return false;
        void* parent = nullptr;
        std::memcpy(&parent, static_cast<const uint8_t*>(material) + parent_offset,
                    sizeof(parent));
        if (parent == material) return false;
        material = parent;
    }
    return false;
}

void* ButtonCreateMID(void* mesh, void* source) {
    if (!mesh || !source || !g_process_event) return nullptr;
    struct Factory {
        const wchar_t* name; const wchar_t* parent; bool optional_name;
    };
    constexpr Factory factories[] = {
        {L"CreateDynamicMaterialInstance", L"SourceMaterial", true},
        {L"CreateAndSetMaterialInstanceDynamicFromMaterial", L"Parent", false},
        {L"CreateAndSetMaterialInstanceDynamic", nullptr, false}
    };
    void* initial = ButtonGetMaterial(mesh);
    for (const auto& factory : factories) {
        void* fn = FindReflectedFunction(mesh, factory.name);
        int32_t index = -1, result = -1, parent = -1, optional_name = -1;
        if (!ButtonFunctionParameter(fn, L"ElementIndex", L"IntProperty", index) ||
            !ButtonFunctionParameter(fn, L"ReturnValue", L"ObjectProperty", result) ||
            index < 0 || index > 92 || result < 0 || result > 92 ||
            (factory.parent &&
             (!ButtonFunctionParameter(fn, factory.parent, L"ObjectProperty", parent) ||
              parent > 92)) ||
            (factory.optional_name &&
             (!ButtonFunctionParameter(fn, L"OptionalName", L"NameProperty", optional_name) ||
              optional_name > 88))) continue;
        // Legacy one-arg form uses the currently installed material as source.
        if (!factory.parent && ButtonGetMaterial(mesh) != source &&
            !ButtonSetMaterial(mesh, source)) break;
        alignas(16) std::array<uint8_t, 96> args{};
        if (parent >= 0)
            std::memcpy(args.data() + parent, &source, sizeof(source));
        g_process_event(mesh, fn, args.data());
        void* mid = nullptr;
        std::memcpy(&mid, args.data() + result, sizeof(mid));
        if (!IsReadable(mid, sizeof(UObjectHeader)) || ButtonGetMaterial(mesh) != mid)
            continue;
        const auto* object = static_cast<const UObjectHeader*>(mid);
        if (!IsReadable(object->class_ptr, sizeof(UObjectHeader)) ||
            UnrealName(static_cast<const UObjectHeader*>(object->class_ptr)->name)
                .find(L"MaterialInstanceDynamic") == std::wstring::npos) continue;
        return mid;
    }
    // Do not leave a different material in the component on a failed factory.
    if (initial && ButtonGetMaterial(mesh) != initial)
        ButtonSetMaterial(mesh, initial);
    return nullptr;
}

bool ButtonSetTint(void* mid, FNameView parameter, uint32_t rgb) {
    if (!IsReadable(mid, sizeof(UObjectHeader)) || !g_process_event) return false;
    const auto call = ButtonCall(mid, 2);
    if (!call.function) return false;
    const auto linear = [](uint32_t channel) -> float {
        const float s = static_cast<float>(channel) / 255.0f;
        return s <= 0.04045f ? s / 12.92f :
            std::pow((s + 0.055f) / 1.055f, 2.4f);
    };
    const LinearColor tint{
        linear((rgb >> 16) & 0xFF), linear((rgb >> 8) & 0xFF),
        linear(rgb & 0xFF), 1.0f
    };
    alignas(16) std::array<uint8_t, 96> args{};
    std::memcpy(args.data() + call.first, &parameter, sizeof(parameter));
    std::memcpy(args.data() + call.second, &tint, sizeof(tint));
    g_process_event(mid, call.function, args.data());
    return true;
}

void ApplyButtonTint(int index, void* actor) {
    if (index < 0 || index >= kRefunctButtonCount) return;
    auto& cache = s_button_tint_cache[index];
    if (cache.actor != actor) cache = {};
    cache.actor = actor;
    void* mesh = nullptr;
    void* active = nullptr;
    void* pressed = nullptr;
    if (!ButtonProperty(actor, L"Button", 712, mesh) ||
        !ButtonProperty(actor, L"ButtonActiveMaterial", 752, active) ||
        !ButtonProperty(actor, L"ButtonPressedMaterial", 748, pressed)) {
        ButtonColourDiagnostic(L"native Button/material properties unavailable");
        return;
    }
    if (cache.mesh != mesh) { cache = {}; cache.actor = actor; cache.mesh = mesh; }
    void* displayed = ButtonGetMaterial(mesh);
    if (!displayed) {
        ButtonColourDiagnostic(L"Button mesh slot 0 could not be read");
        return;
    }
    const bool our_material = displayed == cache.material_instance &&
        cache.material_instance && cache.source_material;
    void* source = our_material ? cache.source_material : displayed;
    int role = -1; // Native current material determines active vs pressed.
    if (source == active) role = 0;
    else if (source == pressed) role = 1;
    // Do not recolour the inactive shell, beacon, base, or any unrecognised material.
    if (role < 0) {
        static bool reported_unrecognised = false;
        if (!reported_unrecognised && index < 3 &&
            (ButtonColour(false).enabled || ButtonColour(true).enabled)) {
            reported_unrecognised = true;
            const auto* d = static_cast<const UObjectHeader*>(displayed);
            const auto* a = static_cast<const UObjectHeader*>(active);
            const auto* p = static_cast<const UObjectHeader*>(pressed);
            Log((L"Button Colours: displayed material does not match button's "
                 L"native active/pressed materials; current=" + UnrealName(d->name) +
                 L" active=" + UnrealName(a->name) +
                 L" pressed=" + UnrealName(p->name)).c_str());
        }
        return;
    }
    const auto& setting = ButtonColour(role == 1);
    if (!setting.enabled) {
        if (our_material && ButtonSetMaterial(mesh, source)) {
            cache.material_instance = nullptr;
            cache.source_material = nullptr;
        }
        return;
    }
    if (our_material && cache.last_rgb == setting.rgb) return;
    FNameView parameter{};
    if (!ButtonTintParameter(source, parameter)) {
        ButtonColourDiagnostic(L"button material lacks a single tint vector parameter");
        return;
    }
    // Reuse an attached MID only, to avoid dangling Unreal-owned objects.
    void* mid = our_material ? cache.material_instance : ButtonCreateMID(mesh, source);
    if (!mid || !ButtonSetTint(mid, parameter, setting.rgb)) {
        if (mid && !our_material) ButtonSetMaterial(mesh, source);
        ButtonColourDiagnostic(L"dynamic button material creation/tint failed");
        return;
    }
    cache.material_instance = mid;
    cache.source_material = source;
    cache.last_rgb = setting.rgb;
}

// Determine actual displayed state using the native centre material, not
// IsPressed: that flag also locks buttons which are not actually pressed.
// Preserve the source material when our centre tint has installed a MID.
int ButtonVisualColourRole(int index, void* actor, void* centre) {
    void* active = nullptr;
    void* pressed = nullptr;
    if (!ButtonProperty(actor, L"ButtonActiveMaterial", 752, active) ||
        !ButtonProperty(actor, L"ButtonPressedMaterial", 748, pressed)) return -1;
    void* displayed = ButtonGetMaterial(centre);
    const auto& centre_cache = s_button_tint_cache[index];
    if (displayed && centre_cache.actor == actor && centre_cache.mesh == centre &&
        centre_cache.material_instance == displayed && centre_cache.source_material)
        displayed = centre_cache.source_material;
    if (displayed == pressed) return 1;
    if (displayed == active) return 0;
    return -1; // Native inactive/unknown material: do not guess a state.
}

// The outer white ring is BP_Button_C::"Button base" @0x2D0 (720), separate
// from BP_Button_C::"Button" @0x2C8 (712). Original border materials can have
// a *fixed* white colour with no exposed tint parameter. If so, a per-component
// MID using a verified, tint-capable material already present on this button
// is a fallback. Geometry and button interaction state are never touched.
// Switching custom OFF restores precisely the border component's old material.
void ApplyButtonBorderTint(int index, void* actor) {
    if (index < 0 || index >= kRefunctButtonCount) return;
    auto& cache = s_button_border_tint_cache[index];
    if (cache.actor != actor) cache = {};
    cache.actor = actor;

    void* border = nullptr;
    void* centre = nullptr;
    if (!ButtonProperty(actor, L"Button base", 720, border) ||
        !ButtonProperty(actor, L"Button", 712, centre) || border == centre) {
        if (!s_button_border_abi_reported) {
            s_button_border_abi_reported = true;
            Log(L"Button border colour: separate Button base component not found; unchanged");
        }
        return;
    }
    if (cache.mesh != border) {
        cache = {};
        cache.actor = actor;
        cache.mesh = border;
    }
    void* displayed = ButtonGetMaterial(border);
    if (!displayed) {
        if (!s_button_border_abi_reported) {
            s_button_border_abi_reported = true;
            Log(L"Button border colour: Button base slot 0 GetMaterial failed");
        }
        return;
    }

    const bool ours = cache.material_instance && cache.source_material &&
                      displayed == cache.material_instance;
    // If the game swapped the border material since the last update, forget
    // the old MID and honour the newly installed material as the base state.
    if (!ours && cache.material_instance) {
        cache.material_instance = nullptr;
        cache.source_material = nullptr;
        cache.colour_source_material = nullptr;
        cache.colour_parameter = {};
        cache.last_rgb = 0;
    }
    void* original = ours ? cache.source_material : displayed;
    const int role = ButtonVisualColourRole(index, actor, centre);
    const auto& setting = ButtonBorderColour(role == 1);
    // Inactive/unknown buttons should keep their native border. Switching
    // between active and pressed states retints the same attached MID.
    if (role < 0 || !setting.enabled) {
        if (ours && ButtonSetMaterial(border, original)) {
            cache.material_instance = nullptr;
            cache.source_material = nullptr;
            cache.colour_source_material = nullptr;
            cache.colour_parameter = {};
            cache.last_rgb = 0;
        }
        return;
    }
    if (ours && cache.last_rgb == setting.rgb) return;

    void* shader_source = nullptr;
    FNameView parameter{};
    if (ours && IsReadable(cache.colour_source_material, sizeof(UObjectHeader))) {
        // Reuse the same tint shader after changing HEX/picker values. Never
        // re-detect from the original fixed-white material after first use.
        shader_source = cache.colour_source_material;
        parameter = cache.colour_parameter;
    } else {
        void* active = nullptr;
        void* pressed = nullptr;
        const bool active_ok = ButtonProperty(actor, L"ButtonActiveMaterial", 752, active);
        const bool pressed_ok = ButtonProperty(actor, L"ButtonPressedMaterial", 748, pressed);

        // First try the button's known active/pressed colour materials. This
        // also handles a border material with a vector parameter that doesn't
        // drive the actual visible colour of the white rim.
        if (active_ok && ButtonTintParameter(active, parameter)) shader_source = active;
        else if (pressed_ok && ButtonTintParameter(pressed, parameter)) shader_source = pressed;
        else if (ButtonTintParameter(original, parameter)) shader_source = original;

        if (!shader_source) {
            if (!s_button_border_abi_reported) {
                s_button_border_abi_reported = true;
                const auto* obj = static_cast<const UObjectHeader*>(original);
                Log((L"Button border colour: no tint-capable shader found; base=" +
                     UnrealName(obj->name) +
                     L"; active=" + (active_ok ? UnrealName(
                         static_cast<const UObjectHeader*>(active)->name) : L"unavailable") +
                     L"; pressed=" + (pressed_ok ? UnrealName(
                         static_cast<const UObjectHeader*>(pressed)->name) : L"unavailable")).c_str());
                Notify(L"Border shader has no usable tint source; see RefunctUtility.log");
            }
            return;
        }
        if (!s_button_border_source_logged) {
            s_button_border_source_logged = true;
            Log((L"Button border colour: base material=" +
                 UnrealName(static_cast<const UObjectHeader*>(original)->name) +
                 L"; tint material=" +
                 UnrealName(static_cast<const UObjectHeader*>(shader_source)->name) +
                 L"; parameter=" + UnrealName(parameter) +
                 (shader_source == original ? L" (native base)" :
                     L" (button material fallback; ring texture may differ)")).c_str());
        }
    }

    void* mid = ours ? cache.material_instance : ButtonCreateMID(border, shader_source);
    if (!mid || !ButtonSetTint(mid, parameter, setting.rgb)) {
        // A failed replacement must not leave the ring displaying a different
        // shader. The old border material is always the rollback target.
        if (!ours && ButtonGetMaterial(border) != original)
            ButtonSetMaterial(border, original);
        if (!s_button_border_abi_reported) {
            s_button_border_abi_reported = true;
            Log(L"Button border colour: failed creating or tinting a border MID");
            Notify(L"Border material creation failed; see RefunctUtility.log");
        }
        return;
    }
    cache.material_instance = mid;
    cache.source_material = original;
    cache.colour_source_material = shader_source;
    cache.colour_parameter = parameter;
    cache.last_rgb = setting.rgb;
}

// Native Beacon is a separate BP_Button_C object property, not the Button
// press/collision mesh. Resolve its offset and function signatures through
// reflection so an unknown component type or game update fails inertly.
bool BeaconGetVisible(void* component, bool& visible) {
    void* fn = FindReflectedFunction(component, L"IsVisible");
    int32_t result = -1;
    if (!ButtonFunctionParameter(fn, L"ReturnValue", L"BoolProperty", result) ||
        result != 0 || !g_process_event) return false;
    alignas(16) std::array<uint8_t, 96> args{};
    g_process_event(component, fn, args.data());
    visible = args[0] != 0;
    return true;
}
bool BeaconSetVisible(void* component, bool visible) {
    void* fn = FindReflectedFunction(component, L"SetVisibility");
    int32_t state = -1, children = -1;
    if (!ButtonFunctionParameter(fn, L"bNewVisibility", L"BoolProperty", state) ||
        !ButtonFunctionParameter(fn, L"bPropagateToChildren", L"BoolProperty", children) ||
        state != 0 || children != 1 || !g_process_event) return false;
    alignas(16) std::array<uint8_t, 96> args{};
    args[state] = visible ? 1 : 0;
    args[children] = 1; // Hide the entire attached beacon, if it has children.
    g_process_event(component, fn, args.data());
    bool current = false;
    return BeaconGetVisible(component, current) && current == visible;
}
void ApplyBeaconColour(BeaconCache& cache, bool custom, uint32_t rgb) {
    auto& tint = cache.tint;
    void* displayed = ButtonGetMaterial(cache.component);
    if (!displayed) {
        if (custom && !s_beacon_colour_reported) {
            s_beacon_colour_reported = true;
            Log(L"Button beacon colour: Beacon has no GetMaterial slot 0 (may be a particle/beam component); tint not applied");
            Notify(L"Beacon material cannot be tinted; see RefunctUtility.log");
        }
        return;
    }
    const bool ours = tint.material_instance && tint.source_material &&
                      displayed == tint.material_instance;
    // A game replacement invalidates our cached MID. Do not overwrite it.
    if (!ours && tint.material_instance) tint = {};
    void* original = ours ? tint.source_material : displayed;
    if (!custom) {
        if (ours && ButtonSetMaterial(cache.component, original)) tint = {};
        return;
    }
    if (ours && tint.last_rgb == rgb) return;
    FNameView parameter{};
    if (ours) {
        parameter = tint.colour_parameter;
    } else if (!ButtonTintParameter(original, parameter)) {
        if (!s_beacon_colour_reported) {
            s_beacon_colour_reported = true;
            Log((L"Button beacon colour: material has no unambiguous vector tint parameter; " +
                UnrealName(static_cast<const UObjectHeader*>(original)->name)).c_str());
            Notify(L"Beacon shader has no usable tint parameter; see log");
        }
        return;
    }
    void* mid = ours ? tint.material_instance :
        ButtonCreateMID(cache.component, original);
    if (!mid || !ButtonSetTint(mid, parameter, rgb)) {
        if (!ours && ButtonGetMaterial(cache.component) != original)
            ButtonSetMaterial(cache.component, original);
        if (!s_beacon_colour_reported) {
            s_beacon_colour_reported = true;
            Log(L"Button beacon colour: failed to create/tint material instance");
        }
        return;
    }
    tint.material_instance = mid;
    tint.source_material = original;
    tint.colour_parameter = parameter;
    tint.last_rgb = rgb;
    if (!s_beacon_source_logged) {
        s_beacon_source_logged = true;
        Log((L"Button beacon colour: tinting Beacon material " +
             UnrealName(static_cast<const UObjectHeader*>(original)->name) +
             L" parameter " + UnrealName(parameter)).c_str());
    }
}
void ApplyButtonBeacon(int index, void* actor, const BeaconSetting& setting) {
    void* component = nullptr;
    if (!ButtonProperty(actor, L"Beacon", -1, component)) {
        if (!s_beacon_visibility_reported) {
            s_beacon_visibility_reported = true;
            Log(L"Button beacons: BP_Button_C.Beacon component unavailable or wrong type");
        }
        return;
    }
    auto& cache = s_beacon_cache[index];
    if (cache.actor != actor || cache.component != component) {
        // Never restore a material to a different component after a level reload.
        cache = {};
        cache.actor = actor;
        cache.component = component;
    }
    bool currently_visible = false;
    if (!BeaconGetVisible(component, currently_visible)) {
        if (!s_beacon_visibility_reported) {
            s_beacon_visibility_reported = true;
            Log(L"Button beacons: reflected IsVisible() unavailable; visibility unchanged");
        }
    } else if (!setting.visible) {
        if (currently_visible) {
            if (BeaconSetVisible(component, false)) {
                cache.forced_hidden = true;
                cache.previous_visible = true;
            }
            else if (!s_beacon_visibility_reported) {
                s_beacon_visibility_reported = true;
                Log(L"Button beacons: reflected SetVisibility(bool,bool) failed");
            }
        }
    } else if (cache.forced_hidden) {
        // Don't light a beacon that the game has already consumed/pressed.
        const LevelState* progress = PracticeLevelState();
        const bool completed = progress && index < progress->buttons;
        if (!completed && cache.previous_visible && !currently_visible)
            BeaconSetVisible(component, true);
        cache.forced_hidden = false;
        cache.previous_visible = false;
    }
    // Material is independent of visibility. Hidden beacons retain their tint
    // and resume displaying it if the visibility toggle is re-enabled.
    if (setting.custom_colour || cache.tint.material_instance)
        ApplyBeaconColour(cache, setting.custom_colour, setting.rgb);
}
} // namespace

bool IsButtonCustomColourEnabled(bool pressed) {
    return ButtonColour(pressed).enabled;
}
void SetButtonCustomColourEnabled(bool pressed, bool enabled) {
    auto& setting = ButtonColour(pressed);
    setting.enabled = enabled;
    PersistButtonColour(pressed);
    RequestButtonColourRefresh();
    Notify(std::wstring(pressed ? L"Pressed" : L"Active") +
           (enabled ? L" button custom colour ON" : L" button game colour restored"));
}
std::wstring GetButtonCustomColourHex(bool pressed) {
    return ButtonHex(ButtonColour(pressed).rgb);
}
void SetButtonCustomColourHex(bool pressed, uint32_t rgb) {
    auto& setting = ButtonColour(pressed);
    setting.rgb = rgb & 0xFFFFFFu;
    setting.enabled = true;
    PersistButtonColour(pressed);
    RequestButtonColourRefresh();
    Notify(std::wstring(pressed ? L"Pressed" : L"Active") +
           L" button colour: " + ButtonHex(setting.rgb));
}
void OpenButtonColourPicker(bool pressed) {
    if (s_button_picker_open.exchange(true, std::memory_order_acq_rel)) {
        Notify(L"Button colour picker is already open");
        return;
    }
    s_button_picker_target.store(pressed ? 1 : 0, std::memory_order_release);
    s_button_picker_initial.store(ButtonColour(pressed).rgb, std::memory_order_release);
    s_button_picker_result.store(kButtonPickerEmpty, std::memory_order_release);
    HANDLE thread = CreateThread(nullptr, 0, ButtonColourPickerThread, nullptr, 0, nullptr);
    if (!thread) {
        s_button_picker_open.store(false, std::memory_order_release);
        Notify(L"Could not open Windows colour picker");
        return;
    }
    CloseHandle(thread);
    Notify(L"Windows colour picker opened (Alt+Tab if behind fullscreen game)");
}
bool IsButtonBorderCustomColourEnabled(bool pressed) {
    return ButtonBorderColour(pressed).enabled;
}
void SetButtonBorderCustomColourEnabled(bool pressed, bool enabled) {
    auto& setting = ButtonBorderColour(pressed);
    setting.enabled = enabled;
    PersistButtonBorderColour(pressed);
    RequestButtonColourRefresh();
    Notify(std::wstring(pressed ? L"Pressed" : L"Unpressed") +
           (enabled ? L" border custom colour ON" : L" border game colour restored"));
}
std::wstring GetButtonBorderColourHex(bool pressed) {
    return ButtonHex(ButtonBorderColour(pressed).rgb);
}
void SetButtonBorderColourHex(bool pressed, uint32_t rgb) {
    auto& setting = ButtonBorderColour(pressed);
    setting.rgb = rgb & 0xFFFFFFu;
    setting.enabled = true;
    PersistButtonBorderColour(pressed);
    RequestButtonColourRefresh();
    Notify(std::wstring(pressed ? L"Pressed" : L"Unpressed") +
           L" border colour: " + ButtonHex(setting.rgb));
}
void OpenButtonBorderColourPicker(bool pressed) {
    if (s_button_picker_open.exchange(true, std::memory_order_acq_rel)) {
        Notify(L"Button colour picker is already open");
        return;
    }
    s_button_picker_target.store(pressed ? 3 : 2, std::memory_order_release);
    s_button_picker_initial.store(ButtonBorderColour(pressed).rgb, std::memory_order_release);
    s_button_picker_result.store(kButtonPickerEmpty, std::memory_order_release);
    HANDLE thread = CreateThread(nullptr, 0, ButtonColourPickerThread, nullptr, 0, nullptr);
    if (!thread) {
        s_button_picker_open.store(false, std::memory_order_release);
        Notify(L"Could not open Windows colour picker");
        return;
    }
    CloseHandle(thread);
    Notify(std::wstring(pressed ? L"Pressed" : L"Unpressed") +
           L" border Windows colour picker opened (Alt+Tab if behind fullscreen game)");
}
bool IsButtonBeaconVisible() {
    return ButtonBeaconSetting().visible;
}
void SetButtonBeaconVisible(bool visible) {
    ButtonBeaconSetting().visible = visible;
    PersistButtonBeaconSetting();
    RequestButtonColourRefresh();
    Notify(visible ? L"Button beacons visible" : L"Button beacons hidden");
}
bool IsButtonBeaconCustomColourEnabled() {
    return ButtonBeaconSetting().custom_colour;
}
void SetButtonBeaconCustomColourEnabled(bool enabled) {
    ButtonBeaconSetting().custom_colour = enabled;
    PersistButtonBeaconSetting();
    RequestButtonColourRefresh();
    Notify(enabled ? L"Beacon custom colour ON" : L"Beacon game colour restored");
}
std::wstring GetButtonBeaconColourHex() {
    return ButtonHex(ButtonBeaconSetting().rgb);
}
void SetButtonBeaconColourHex(uint32_t rgb) {
    auto& setting = ButtonBeaconSetting();
    setting.rgb = rgb & 0xFFFFFFu;
    setting.custom_colour = true;
    PersistButtonBeaconSetting();
    RequestButtonColourRefresh();
    Notify(L"Beacon colour: " + ButtonHex(setting.rgb));
}
void OpenButtonBeaconColourPicker() {
    if (s_button_picker_open.exchange(true, std::memory_order_acq_rel)) {
        Notify(L"Button colour picker is already open");
        return;
    }
    s_button_picker_target.store(4, std::memory_order_release);
    s_button_picker_initial.store(ButtonBeaconSetting().rgb, std::memory_order_release);
    s_button_picker_result.store(kButtonPickerEmpty, std::memory_order_release);
    HANDLE thread = CreateThread(nullptr, 0, ButtonColourPickerThread, nullptr, 0, nullptr);
    if (!thread) {
        s_button_picker_open.store(false, std::memory_order_release);
        Notify(L"Could not open Windows colour picker");
        return;
    }
    CloseHandle(thread);
    Notify(L"Beacon colour picker opened (Alt+Tab if behind fullscreen game)");
}
void UpdateButtonColours() {
    const uint32_t picker = s_button_picker_result.exchange(kButtonPickerEmpty,
                                                              std::memory_order_acq_rel);
    if (picker <= 0xFFFFFFu) {
        const int target = s_button_picker_target.load(std::memory_order_acquire);
        if (target == 4) SetButtonBeaconColourHex(picker);
        else if (target == 2 || target == 3) SetButtonBorderColourHex(target == 3, picker);
        else SetButtonCustomColourHex(target == 1, picker);
    } else if (picker == kButtonPickerFailure) {
        Log(L"Button Colours: Windows ChooseColor dialog failed");
        Notify(L"Windows colour picker failed");
    }
    const bool active = ButtonColour(false).enabled;
    const bool pressed = ButtonColour(true).enabled;
    const bool border = ButtonBorderColour(false).enabled ||
                        ButtonBorderColour(true).enabled;
    const auto& beacon_setting = ButtonBeaconSetting();
    const bool beacon_override = !beacon_setting.visible || beacon_setting.custom_colour;
    bool has_previous = false;
    for (const auto& cache : s_button_tint_cache)
        if (cache.material_instance) { has_previous = true; break; }
    if (!has_previous) {
        for (const auto& cache : s_button_border_tint_cache)
            if (cache.material_instance) { has_previous = true; break; }
    }
    if (!has_previous) {
        for (const auto& cache : s_beacon_cache) {
            if (cache.forced_hidden || cache.tint.material_instance) {
                has_previous = true;
                break;
            }
        }
    }
    if (!active && !pressed && !border && !beacon_override && !has_previous) return;
    const ULONGLONG now = GetTickCount64();
    if (now < s_button_tint_next_tick && s_button_colour_dirty_remaining == 0) return;
    const bool urgent = s_button_colour_dirty_remaining > 0;
    // During an edit, spread potentially expensive material creation across
    // frames; in steady state, validate existing MIDs every 200 ms.
    s_button_tint_next_tick = now + (urgent ? 0 : 200);
    if (!g_process_event || !g_practice_button_actors_ready) {
        if (now >= s_button_tint_next_discovery) {
            s_button_tint_next_discovery = now + 2500;
            DiscoverPracticeButtons();
        }
        return;
    }
    constexpr int kButtonsPerBatch = 6;
    ColourWorkBudget tint_budget(1000);
    for (int step = 0; step < kButtonsPerBatch; ++step) {
        if (step > 0 && tint_budget.expired()) break;
        if (s_button_colour_dirty_remaining) --s_button_colour_dirty_remaining;
        const int i = s_button_tint_cursor;
        s_button_tint_cursor = (s_button_tint_cursor + 1) % kRefunctButtonCount;
        void* actor = g_practice_button_actors[i];
        if (!IsReadable(actor, sizeof(UObjectHeader))) continue;
        // Preserve the existing centre-only runtime when border colouring is off.
        // After disabling either custom setting, restore only its own attached MID.
        if (active || pressed || s_button_tint_cache[i].material_instance)
            ApplyButtonTint(i, actor);
        if (border || s_button_border_tint_cache[i].material_instance)
            ApplyButtonBorderTint(i, actor);
        if (beacon_override || s_beacon_cache[i].forced_hidden ||
            s_beacon_cache[i].tint.material_instance)
            ApplyButtonBeacon(i, actor, beacon_setting);
    }
}

} // namespace refunct

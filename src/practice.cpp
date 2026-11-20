#include "refunct/internal_api.hpp"
#include <new>

namespace refunct {
namespace {
// Keep two real button contacts (B and N), including stance and velocity.
// Persist them across game launches. Synthetic practice setup presses must
// never be mistaken for a real contact.
struct ButtonContact {
    Checkpoint checkpoint{};
    bool valid = false;
    bool crouched = false;
    bool stance_valid = false;
    Vec3 velocity{};
    bool velocity_valid = false;
};
ButtonContact s_last_contact{};
ButtonContact s_second_last_contact{};
// Independent teleport history: the last *real* button pressed in any world.
// Unlike B/N checkpoint history, this survives restarts and map changes.
ButtonContact s_global_last_contact{};
// Set by the real ALiftBase contact hook and consumed by TrackLastButton.
// An event-time pose must not be overwritten by an end-of-frame sample.
bool s_native_global_contact_since_tick = false;
// User-requested global teleport, never triggered automatically by a button.
// Defer until practice/map setup is stable and verify across subsequent ticks.
struct PendingGlobalTeleport {
    Vec3 destination{};
    int button_number = 0;
    ULONGLONG requested_ms = 0;
    int settle_frames = 0;
    bool active = false;
    bool placed = false;
    bool waiting_logged = false;
};
PendingGlobalTeleport s_pending_global_teleport{};
ButtonContact s_selected_contact{};
bool s_button_history_loaded = false;

bool s_last_button_stance_pending = false;
bool s_last_button_stance_target = false;
int s_last_button_stance_attempts = 0;
Vec3 s_last_button_momentum_target{};
bool s_last_button_momentum_pending = false;
// Native hooks also see our synthetic practice activations; those need to
// bypass the section gate or checkpoint/section loading would stop working.
thread_local unsigned s_synthetic_button_contact_depth = 0;
struct ScopedSyntheticButtonContact {
    ScopedSyntheticButtonContact() { ++s_synthetic_button_contact_depth; }
    ~ScopedSyntheticButtonContact() { --s_synthetic_button_contact_depth; }
    ScopedSyntheticButtonContact(const ScopedSyntheticButtonContact&) = delete;
    ScopedSyntheticButtonContact& operator=(const ScopedSyntheticButtonContact&) = delete;
};

bool ValidLastButtonVelocity(const Vec3& velocity) {
    constexpr float kMaximumSafeComponent = 100000.0f;
    return std::isfinite(velocity.x) && std::isfinite(velocity.y) &&
           std::isfinite(velocity.z) &&
           std::abs(velocity.x) <= kMaximumSafeComponent &&
           std::abs(velocity.y) <= kMaximumSafeComponent &&
           std::abs(velocity.z) <= kMaximumSafeComponent;
}

void RestoreLastButtonMomentum(MyCharacter* player) {
    if (!s_last_button_momentum_pending) return;
    // Set velocity only once, after stabilization and stance restoration.
    // Earlier practice phases repeatedly zero it while teleporting.
    s_last_button_momentum_pending = false;
    if (!player || !IsReadable(player, sizeof(*player)) ||
        !IsReadable(player->movement, sizeof(MovementComponent)) ||
        !ValidLastButtonVelocity(s_last_button_momentum_target)) {
        Log(L"Load last button: saved momentum could not be restored");
        return;
    }
    player->movement->velocity = s_last_button_momentum_target;
}

// Button history is independent of F1's save-state.ini. Store both exact
// contacts in one small, versioned file inside the existing game-local folder.
// The commit marker prevents a partial write from being accepted on next boot.
std::wstring ButtonHistoryPath() {
    return g_storage_dir.empty() ? L"" : g_storage_dir + L"\\button-history.ini";
}

bool ValidButtonContact(const ButtonContact& contact) {
    return contact.valid && ValidCheckpoint(contact.checkpoint) &&
           contact.checkpoint.buttons >= 1 &&
           contact.checkpoint.buttons <= kRefunctButtonCount &&
           (!contact.velocity_valid || ValidLastButtonVelocity(contact.velocity));
}

std::wstring EncodeButtonContact(const ButtonContact& contact) {
    if (!ValidButtonContact(contact)) return L"none";
    const auto& cp = contact.checkpoint;
    wchar_t buffer[384]{};
    swprintf_s(buffer,
        L"%d|%.9g|%.9g|%.9g|%.9g|%.9g|%.9g|%d|%d|%.9g|%.9g|%.9g|%d",
        cp.buttons,
        static_cast<double>(cp.location.x),
        static_cast<double>(cp.location.y),
        static_cast<double>(cp.location.z),
        static_cast<double>(cp.rotation.pitch),
        static_cast<double>(cp.rotation.yaw),
        static_cast<double>(cp.rotation.roll),
        contact.crouched ? 1 : 0, contact.stance_valid ? 1 : 0,
        static_cast<double>(contact.velocity.x),
        static_cast<double>(contact.velocity.y),
        static_cast<double>(contact.velocity.z),
        contact.velocity_valid ? 1 : 0);
    return buffer;
}

bool DecodeButtonContact(const std::wstring& encoded, ButtonContact& result) {
    ButtonContact contact{};
    const wchar_t* cursor = encoded.c_str();
    const auto read_int = [&](int& output, bool final = false) -> bool {
        wchar_t* end = nullptr;
        errno = 0;
        const long value = std::wcstol(cursor, &end, 10);
        if (end == cursor || errno == ERANGE ||
            value < std::numeric_limits<int>::min() ||
            value > std::numeric_limits<int>::max() ||
            *end != (final ? L'\0' : L'|')) return false;
        output = static_cast<int>(value);
        cursor = final ? end : end + 1;
        return true;
    };
    const auto read_float = [&](float& output) -> bool {
        wchar_t* end = nullptr;
        errno = 0;
        const float value = std::wcstof(cursor, &end);
        if (end == cursor || errno == ERANGE || !std::isfinite(value) ||
            *end != L'|') return false;
        output = value;
        cursor = end + 1;
        return true;
    };
    int crouched = 0, stance_valid = 0, velocity_valid = 0;
    if (!read_int(contact.checkpoint.buttons) ||
        !read_float(contact.checkpoint.location.x) ||
        !read_float(contact.checkpoint.location.y) ||
        !read_float(contact.checkpoint.location.z) ||
        !read_float(contact.checkpoint.rotation.pitch) ||
        !read_float(contact.checkpoint.rotation.yaw) ||
        !read_float(contact.checkpoint.rotation.roll) ||
        !read_int(crouched) || !read_int(stance_valid) ||
        !read_float(contact.velocity.x) ||
        !read_float(contact.velocity.y) ||
        !read_float(contact.velocity.z) ||
        !read_int(velocity_valid, true)) return false;
    if ((crouched != 0 && crouched != 1) ||
        (stance_valid != 0 && stance_valid != 1) ||
        (velocity_valid != 0 && velocity_valid != 1) ||
        !ValidLastButtonVelocity(contact.velocity)) return false;
    contact.valid = true;
    contact.crouched = crouched != 0;
    contact.stance_valid = stance_valid != 0;
    contact.velocity_valid = velocity_valid != 0;
    if (!ValidButtonContact(contact)) return false;
    result = contact;
    return true;
}

// Persist button history off the character tick. The previous implementation
// performed five synchronous Win32 INI writes on EVERY real button press.
// The worker commits one complete snapshot atomically, coalescing bursts of
// presses without ever writing a stale snapshot after a newer one.
struct ButtonHistorySnapshot {
    std::wstring path;
    ButtonContact last{};
    ButtonContact second{};
    ButtonContact global{};
};

struct ButtonHistoryWriter {
    SRWLOCK lock = SRWLOCK_INIT;
    HANDLE wake = nullptr; // Auto-reset event.
    ButtonHistorySnapshot latest{};
    bool pending = false;
};

// Process-lifetime worker: the ASI is never hot-unloaded. No thread waits or
// joins are performed under the DLL loader lock.
ButtonHistoryWriter* s_button_history_writer = nullptr;

bool WriteButtonHistorySnapshot(const ButtonHistorySnapshot& snapshot) {
    if (snapshot.path.empty()) return false;
    const std::wstring content =
        L"[History]\r\nVersion=1\r\nValid=1\r\nLast=" +
        EncodeButtonContact(snapshot.last) + L"\r\nSecond=" +
        EncodeButtonContact(snapshot.second) + L"\r\nGlobal=" +
        EncodeButtonContact(snapshot.global) + L"\r\n";
    // The existing contact format is ASCII, and Windows' Unicode INI APIs
    // read this representation without requiring a UTF-16 BOM.
    std::string bytes;
    bytes.reserve(content.size());
    for (const wchar_t ch : content) {
        if (ch > 0x7F) return false;
        bytes.push_back(static_cast<char>(ch));
    }
    const std::wstring temporary = snapshot.path + L".tmp";
    const HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0,
                                    nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool complete = WriteFile(file, bytes.data(),
                                    static_cast<DWORD>(bytes.size()),
                                    &written, nullptr) != 0 &&
                          written == bytes.size() && FlushFileBuffers(file) != 0;
    CloseHandle(file);
    if (!complete || !MoveFileExW(temporary.c_str(), snapshot.path.c_str(),
                                 MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

DWORD WINAPI ButtonHistoryWriterMain(void* context) {
    auto* worker = static_cast<ButtonHistoryWriter*>(context);
    for (;;) {
        if (WaitForSingleObject(worker->wake, INFINITE) != WAIT_OBJECT_0) return 1;
        for (;;) {
            AcquireSRWLockExclusive(&worker->lock);
            if (!worker->pending) {
                ReleaseSRWLockExclusive(&worker->lock);
                break;
            }
            ButtonHistorySnapshot snapshot = worker->latest;
            worker->pending = false;
            ReleaseSRWLockExclusive(&worker->lock);
            bool saved = false;
            for (int attempt = 0; attempt < 3 && !saved; ++attempt) {
                if (attempt) Sleep(50); // Worker thread, never the game tick.
                saved = WriteButtonHistorySnapshot(snapshot);
            }
            if (!saved) Log(L"Button history: asynchronous save failed");
            // A newer snapshot arriving during this write is saved next.
        }
    }
}

void StartButtonHistoryWriter() {
    if (s_button_history_writer) return;
    auto* worker = new (std::nothrow) ButtonHistoryWriter;
    if (!worker) return;
    worker->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!worker->wake) { delete worker; return; }
    HANDLE thread = CreateThread(nullptr, 0, ButtonHistoryWriterMain,
                                 worker, 0, nullptr);
    if (!thread) {
        CloseHandle(worker->wake);
        delete worker;
        return;
    }
    CloseHandle(thread);
    s_button_history_writer = worker;
}

bool QueueButtonHistorySnapshot(const std::wstring& path) {
    if (!s_button_history_writer) return false;
    auto* worker = s_button_history_writer;
    AcquireSRWLockExclusive(&worker->lock);
    worker->latest.path = path;
    worker->latest.last = s_last_contact;
    worker->latest.second = s_second_last_contact;
    worker->latest.global = s_global_last_contact;
    worker->pending = true;
    ReleaseSRWLockExclusive(&worker->lock);
    if (SetEvent(worker->wake)) return true;
    // Failed wakeup: cancel the queued snapshot so a later wake cannot
    // overwrite the synchronous fallback with an out-of-date record.
    AcquireSRWLockExclusive(&worker->lock);
    worker->pending = false;
    ReleaseSRWLockExclusive(&worker->lock);
    return false;
}

void LoadButtonHistoryOnce() {
    if (s_button_history_loaded || g_storage_dir.empty()) return;
    s_button_history_loaded = true;
    // Pre-start before the first real button, not in its contact handler.
    StartButtonHistoryWriter();
    const std::wstring path = ButtonHistoryPath();
    if (!IsRegularFile(path) ||
        GetPrivateProfileIntW(L"History", L"Version", 0, path.c_str()) != 1 ||
        GetPrivateProfileIntW(L"History", L"Valid", 0, path.c_str()) != 1) return;

    wchar_t last_buffer[512]{}, second_buffer[512]{}, global_buffer[512]{};
    GetPrivateProfileStringW(L"History", L"Last", L"", last_buffer,
        static_cast<DWORD>(std::size(last_buffer)), path.c_str());
    GetPrivateProfileStringW(L"History", L"Second", L"", second_buffer,
        static_cast<DWORD>(std::size(second_buffer)), path.c_str());
    // Optional in older history files: retain the previous format and migrate
    // its last real contact to Global automatically at the next save.
    GetPrivateProfileStringW(L"History", L"Global", L"", global_buffer,
        static_cast<DWORD>(std::size(global_buffer)), path.c_str());

    ButtonContact last{}, second{}, global{};
    if (last_buffer[0] && wcscmp(last_buffer, L"none") != 0) {
        if (!DecodeButtonContact(last_buffer, last)) {
            Log(L"Button history: saved last contact missing or invalid; ignoring run history");
        } else {
            if (second_buffer[0] && wcscmp(second_buffer, L"none") != 0 &&
                (!DecodeButtonContact(second_buffer, second) ||
                 second.checkpoint.buttons != last.checkpoint.buttons - 1)) {
                Log(L"Button history: invalid second contact ignored");
                second = {};
            }
            s_last_contact = last;
            s_second_last_contact = second;
            g_last_button_checkpoint = last.checkpoint;
            g_last_button_checkpoint_valid = true;
            Log(second.valid ? L"Button history: restored last and second-last contacts"
                             : L"Button history: restored last contact");
        }
    }
    if (global_buffer[0] && wcscmp(global_buffer, L"none") != 0 &&
        !DecodeButtonContact(global_buffer, global)) {
        Log(L"Button history: invalid global teleport contact ignored");
    }
    // Upgrade existing v1 files without discarding the run checkpoint.
    if (!ValidButtonContact(global) && ValidButtonContact(last))
        global = last;
    if (ValidButtonContact(global)) {
        s_global_last_contact = global;
        Log(L"Button history: restored global last button teleport target");
    }
}

void PersistButtonHistory() {
    const std::wstring path = ButtonHistoryPath();
    if (path.empty()) return;
    if (QueueButtonHistorySnapshot(path)) return;
    // Only used if the worker cannot start or its event fails. It is better
    // to preserve button history than silently discard it in that case.
    const ButtonHistorySnapshot snapshot{
        path, s_last_contact, s_second_last_contact, s_global_last_contact};
    if (!WriteButtonHistorySnapshot(snapshot))
        Log(L"Button history: unable to save both button contacts");
}

// Refunct's older UE4 x86 UBoolProperty layout: UProperty::Offset_Internal
// is already reflected at 0x3C by FindClassPropertyOffset; the four native
// bool descriptor bytes are at 0x58..0x5B. Always validate before reading.
// If the game's layout differs, do not interpret an arbitrary memory byte.
// The Shift fallback below can still support the stock crouch key.
struct ReflectedCrouchState {
    bool valid = false;
    bool value = false;
};

ReflectedCrouchState ReadCrouchedState(MyCharacter* player) {
    if (!IsReadable(player, sizeof(UObjectHeader))) return {};
    void* klass = reinterpret_cast<const UObjectHeader*>(player)->class_ptr;
    for (int depth = 0; depth < 16 && IsReadable(klass, sizeof(UStructHeader)); ++depth) {
        const auto* cls = static_cast<const UStructHeader*>(klass);
        void* field = cls->children;
        for (int count = 0; count < 2048 && IsReadable(field, sizeof(UFieldHeader)); ++count) {
            const auto* entry = static_cast<const UFieldHeader*>(field);
            if (UnrealName(entry->object.name) == L"bIsCrouched") {
                if (!IsReadable(entry->object.class_ptr, sizeof(UObjectHeader)) ||
                    UnrealName(static_cast<const UObjectHeader*>(entry->object.class_ptr)->name) !=
                        L"BoolProperty" ||
                    !IsReadable(field, 0x5C)) return {};
                constexpr size_t kBoolDescriptor = 0x58;
                const auto* bytes = static_cast<const uint8_t*>(field) + kBoolDescriptor;
                const uint8_t field_size = bytes[0];
                const uint8_t byte_offset = bytes[1];
                const uint8_t byte_mask = bytes[2];
                const uint8_t field_mask = bytes[3];
                const bool one_bit_mask = byte_mask != 0 && (byte_mask & (byte_mask - 1)) == 0;
                if (field_size < 1 || field_size > 8 || byte_offset >= field_size ||
                    !(one_bit_mask || byte_mask == 0xFF) ||
                    (field_mask != byte_mask && field_mask != 0xFF)) return {};
                const int32_t offset = FindClassPropertyOffset(klass, L"bIsCrouched");
                if (offset < 0 || offset >= 0x4000 - 8) return {};
                const auto* value = reinterpret_cast<const uint8_t*>(player) + offset + byte_offset;
                if (!IsReadable(value, 1)) return {};
                return {true, (*value & byte_mask) != 0};
            }
            field = entry->next;
        }
        klass = cls->super_struct;
    }
    return {};
}

// Use Unreal's own Character::Crouch/UnCrouch so the engine updates its
// collision capsule and animation rather than forging the stance flag.
bool RequestNativeCrouch(MyCharacter* player, bool crouched) {
    if (!g_process_event || !IsReadable(player, sizeof(UObjectHeader))) return false;
    void* function = FindReflectedFunction(player, crouched ? L"Crouch" : L"UnCrouch");
    if (!function) return false;
    // bClientSimulation=false: locally controlled player, normal game path.
    alignas(8) std::array<uint8_t, 8> params{};
    g_process_event(player, function, params.data());
    return true;
}

void RestoreLastButtonStance(MyCharacter* player) {
    if (!s_last_button_stance_pending) return;
    if (s_last_button_stance_attempts > 0) {
        const auto actual = ReadCrouchedState(player);
        if (actual.valid && actual.value == s_last_button_stance_target) {
            s_last_button_stance_pending = false;
            return;
        }
    }
    // Limit retries; never pin the user into crouch or override later input.
    if (s_last_button_stance_attempts >= 6 ||
        !RequestNativeCrouch(player, s_last_button_stance_target)) {
        if (s_last_button_stance_attempts == 0)
            Log(L"Load last button: native Crouch/UnCrouch not available on this build");
        s_last_button_stance_pending = false;
        return;
    }
    ++s_last_button_stance_attempts;
}
} // namespace
// Do not suppress native contacts intentionally triggered by practice replay.
bool IsSyntheticPracticeButtonContact() {
    return s_synthetic_button_contact_depth != 0;
}
// Buttons 1..37 are 1-based in the utility UI; the table is zero-based.
static constexpr std::array<Vec3, kRefunctButtonCount> kPracticeButtonLocations{{
    {-1000.000f, -1000.000f, 732.000f},
    {-2000.000f, 0.000f, 857.000f},
    {2125.000f, -250.000f, 1107.000f},
    {-2725.000f, -875.000f, 193.000f},
    {-5000.000f, -875.000f, 857.000f},
    {-3250.000f, -2250.000f, 1607.000f},
    {-4625.000f, -3000.000f, 107.000f},
    {-4625.000f, -3625.000f, 107.000f},
    {-2750.000f, -3750.000f, 1607.000f},
    {-625.000f, -3375.000f, 1607.000f},
    {-25.000f, -2375.000f, 107.000f},
    {2000.000f, -2375.000f, 232.000f},
    {1875.000f, 975.000f, 232.000f},
    {2375.000f, -500.000f, 107.000f},
    {600.000f, 2625.000f, 232.000f},
    {-875.000f, 2500.000f, 232.000f},
    {-375.000f, 1625.000f, 732.000f},
    {-2750.000f, 1500.000f, 857.000f},
    {-1875.000f, 1125.000f, 1107.000f},
    {-5125.000f, -1750.000f, 107.000f},
    {-4250.000f, -4000.000f, 1607.000f},
    {2000.000f, -3875.000f, 1232.000f},
    {4250.000f, -2125.000f, 1107.000f},
    {2750.000f, -4100.000f, 68.000f},
    {3000.000f, -1000.000f, 232.000f},
    {2500.000f, 2250.000f, 607.000f},
    {375.000f, 4750.000f, 1357.000f},
    {4512.500f, 4625.000f, 232.000f},
    {3125.000f, 6120.000f, 232.000f},
    {1375.000f, 6500.000f, 232.000f},
    {-875.000f, 5625.000f, 232.000f},
    {-1375.000f, 3000.000f, 982.000f},
    {-4875.000f, 1750.000f, 1357.000f},
    {-5250.000f, -250.000f, 1357.000f},
    {4887.500f, 2500.000f, 607.000f},
    {3750.000f, -500.000f, 318.000f},
    {2625.000f, -2250.000f, 1357.000f}
}};

// UE4 object registry, as defined in refunct-tas native reflection source.

bool ResolveButtonBoundsCenter(int button, Vec3& out_center) {
    if (button < 1 || button > kRefunctButtonCount ||
        !g_practice_button_actors_ready || !g_process_event) return false;
    void* actor = g_practice_button_actors[button - 1];
    if (!IsReadable(actor, sizeof(UObjectHeader))) return false;
    void* function = FindReflectedFunction(actor, L"GetActorBounds");
    if (!function) return false;

    constexpr size_t kParamsSize = 256;
    const int32_t origin_offset = FindClassPropertyOffset(function, L"Origin");
    const int32_t extent_offset = FindClassPropertyOffset(function, L"BoxExtent");
    if (origin_offset < 0 || extent_offset < 0 ||
        static_cast<size_t>(origin_offset) > kParamsSize - sizeof(Vec3) ||
        static_cast<size_t>(extent_offset) > kParamsSize - sizeof(Vec3)) return false;

    // Both input booleans are false, so the query covers all components of
    // the button actor without requiring collision to be enabled.
    alignas(16) std::array<uint8_t, kParamsSize> params{};
    g_process_event(actor, function, params.data());
    Vec3 origin{}, extent{};
    std::memcpy(&origin, params.data() + origin_offset, sizeof(origin));
    std::memcpy(&extent, params.data() + extent_offset, sizeof(extent));

    const auto& expected = kPracticeButtonLocations[button - 1];
    if (!std::isfinite(origin.x) || !std::isfinite(origin.y) ||
        !std::isfinite(extent.x) || !std::isfinite(extent.y) ||
        extent.x <= 0.0f || extent.y <= 0.0f ||
        extent.x > 2000.0f || extent.y > 2000.0f ||
        std::abs(origin.x - expected.x) > 600.0f ||
        std::abs(origin.y - expected.y) > 600.0f) return false;
    out_center = {origin.x, origin.y, expected.z};
    return true;
}

// Prefer the actual pressable Button mesh world location, falling back to
// the actor origin. Unlike practice teleport validation this deliberately
// does not compare with a hard-coded coordinate indexed by array order.
bool ResolveReplayButtonCenter(int button, Vec3& out_center) {
    if (button < 1 || button > kRefunctButtonCount ||
        !g_practice_button_actors_ready || !g_process_event) return false;
    void* actor = g_practice_button_actors[button - 1];
    if (!IsReadable(actor, sizeof(UObjectHeader))) return false;
    const auto query = [&](void* object, const wchar_t* primary,
                           const wchar_t* alternate) {
        if (!IsReadable(object, sizeof(UObjectHeader))) return false;
        void* fn = FindReflectedFunction(object, primary);
        if (!fn) fn = FindReflectedFunction(object, alternate);
        if (!fn) return false;
        const int offset = FindClassPropertyOffset(fn, L"ReturnValue");
        alignas(16) std::array<uint8_t, 128> args{};
        if (offset < 0 || static_cast<size_t>(offset) > args.size() - sizeof(Vec3))
            return false;
        g_process_event(object, fn, args.data());
        Vec3 value{};
        std::memcpy(&value, args.data() + offset, sizeof(value));
        if (!std::isfinite(value.x) || !std::isfinite(value.y) ||
            !std::isfinite(value.z) || std::abs(value.x) > 100000.f ||
            std::abs(value.y) > 100000.f || std::abs(value.z) > 100000.f) return false;
        out_center = value;
        return true;
    };
    const int member = FindClassPropertyOffset(
        static_cast<const UObjectHeader*>(actor)->class_ptr, L"Button");
    if (member >= 0 && member < 0x4000 &&
        IsReadable(static_cast<const uint8_t*>(actor) + member, sizeof(void*))) {
        void* component = nullptr;
        std::memcpy(&component, static_cast<const uint8_t*>(actor) + member, sizeof(component));
        if (query(component, L"K2_GetComponentLocation", L"GetComponentLocation")) return true;
    }
    return query(actor, L"K2_GetActorLocation", L"GetActorLocation");
}

// The 31 BP_LevelRoot_C actors own the Buttons arrays in physical run order.
// Resolve them after each restart so we don't reuse actors from an old world.
// This is the same cluster/button ordering used by the original Refunct TAS.
bool DiscoverPracticeButtons() {
    // A prior fast-rise session must be restored before actor references change.
    RestorePracticeClusterSpeeds();
    g_practice_button_actors.fill(nullptr);
    g_practice_button_actors_ready = false;
    g_practice_level_actors.fill(nullptr);
    g_practice_level_class = nullptr;
    g_practice_speed_offset = -1;
    if (!g_game_module) return false;
    const auto base = reinterpret_cast<uintptr_t>(g_game_module);
    const auto* objects = reinterpret_cast<const GlobalObjectArray*>(base + kGlobalObjectArrayRva);
    if (!IsReadable(objects, sizeof(*objects)) || !objects->objects ||
        objects->num_elements < 1 || objects->num_elements > 500000 ||
        objects->max_elements < objects->num_elements ||
        !IsReadable(objects->objects, sizeof(GlobalObjectItem) *
                                     static_cast<size_t>(objects->num_elements))) return false;

    std::array<std::vector<void*>, kPracticeClusterCount> per_cluster;
    std::array<void*, kPracticeClusterCount> level_actors{};
    int32_t level_index_offset = -1;
    int32_t buttons_offset = -1;
    int32_t speed_offset = -1;
    void* level_root_class = nullptr;
    // Cache the classname check so resolving every UObject isn't expensive.
    std::unordered_map<void*, bool> class_cache;
    class_cache.reserve(128);
    for (int32_t i = 0; i < objects->num_elements; ++i) {
        const auto& item = objects->objects[i];
        if (!item.object || (static_cast<uint32_t>(item.flags) & 0x30000000u)) continue;
        if (!IsReadable(item.object, sizeof(UObjectHeader))) continue;
        void* klass = item.object->class_ptr;
        if (!klass || !IsReadable(klass, sizeof(UObjectHeader))) continue;
        if (level_root_class) {
            if (klass != level_root_class) continue;
        } else {
            const auto cached = class_cache.find(klass);
            const bool level_root = cached != class_cache.end()
                ? cached->second
                : class_cache.emplace(klass,
                    UnrealName(static_cast<const UObjectHeader*>(klass)->name) == L"BP_LevelRoot_C")
                  .first->second;
            if (!level_root) continue;
            level_root_class = klass;
            level_index_offset = FindClassPropertyOffset(klass, L"LevelIndex");
            buttons_offset = FindClassPropertyOffset(klass, L"Buttons");
            speed_offset = FindClassPropertyOffset(klass, L"Speed");
            if (level_index_offset < 0 || buttons_offset < 0) {
                Log(L"Section practice: failed to reflect BP_LevelRoot_C properties");
                return false;
            }
        }
        const auto* data = reinterpret_cast<const uint8_t*>(item.object);
        if (!IsReadable(data + level_index_offset, sizeof(int32_t)) ||
            !IsReadable(data + buttons_offset, sizeof(UObjectPointerArray))) continue;
        const int32_t cluster = *reinterpret_cast<const int32_t*>(data + level_index_offset);
        if (cluster < 0 || cluster >= kPracticeClusterCount) continue;
        level_actors[cluster] = item.object;
        const auto& arr = *reinterpret_cast<const UObjectPointerArray*>(data + buttons_offset);
        if (arr.count < 0 || arr.count > 16 || arr.capacity < arr.count ||
            (arr.count > 0 && (!arr.data || !IsReadable(arr.data, sizeof(void*) * arr.count)))) continue;
        if (arr.count > static_cast<int32_t>(per_cluster[cluster].size())) {
            std::vector<void*> buttons;
            buttons.reserve(arr.count);
            bool valid = true;
            for (int32_t k = 0; k < arr.count; ++k) {
                void* actor = arr.data[k];
                if (!IsReadable(actor, sizeof(UObjectHeader))) { valid = false; break; }
                buttons.push_back(actor);
            }
            if (valid) per_cluster[cluster] = std::move(buttons);
        }
    }
    size_t count = 0;
    for (const auto& cluster : per_cluster) {
        for (void* button : cluster) {
            if (count >= g_practice_button_actors.size()) return false;
            g_practice_button_actors[count++] = button;
        }
    }
    if (count != kRefunctButtonCount) {
        Log((L"Section practice: expected 37 button actors, found " +
             std::to_wstring(count)).c_str());
        g_practice_button_actors.fill(nullptr);
        return false;
    }
    g_practice_button_actors_ready = true;
    // Missing Speed is non-fatal: section selection must still work even if
    // this specific game build does not expose the fast-rise property.
    // Section placement needs the level-root actors even if the optional
    // fast-rise Speed property is absent on this game build.
    if (std::all_of(level_actors.begin(), level_actors.end(),
                    [](void* p) { return p != nullptr; })) {
        g_practice_level_actors = level_actors;
        g_practice_level_class = level_root_class;
        g_practice_speed_offset = speed_offset;
    } else {
        Log(L"Section practice: incomplete level-root actor list");
    }
    if (speed_offset < 0)
        Log(L"Section practice: Speed reflection unavailable; using normal level animation");
    Log(L"Section practice: discovered 37 native button actors");
    return true;
}

// Native ALiftBase::AddBasedCharacter/RemoveBasedCharacter; the official TAS
// calls this pair to activate buttons, including buttons not currently raised.
bool ActivatePracticeButton(int zero_based, MyCharacter* player) {
    if (!g_practice_button_actors_ready || !player ||
        zero_based < 0 || zero_based >= kRefunctButtonCount ||
        !g_add_based_character || !g_remove_based_character) return false;
    void* actor = g_practice_button_actors[zero_based];
    if (!IsReadable(actor, sizeof(UObjectHeader))) return false;
    // This is an intentional progress-replay action; the real button-contact
    // hook must filter only physical presses, not the practice loader.
    const ScopedSyntheticButtonContact synthetic_contact{};
    g_add_based_character(actor, player);
    g_remove_based_character(actor, player);
    return true;
}

// Refunct TAS sets player RootComponent.location directly. Reapply it over
// several post-reset game ticks: a reset/initialisation tick can otherwise
// overwrite our one-time teleport, leaving the player at the beginning.
// The capsule centre sits above the button's visual surface. A section/last
// button replay resolves X/Y from its actual actor bounds after the reset.
Vec3 PracticeSpawnForButton(int button) {
    Vec3 spot = kPracticeButtonLocations[button - 1];
    spot.z += 90.0f; // Character capsule centre, rather than button surface.
    return spot;
}

bool SetPracticePosition(MyCharacter* player, const Vec3& destination) {
    if (!player || !IsReadable(player, sizeof(*player)) ||
        !IsReadable(player->root_component, sizeof(SceneComponent)) ||
        !IsReadable(player->movement, sizeof(MovementComponent))) return false;
    player->root_component->location = destination;
    player->movement->velocity = Vec3{};
    player->movement->acceleration = Vec3{};
    if (g_practice_restore_rotation) {
        if (!IsReadable(player->controller, sizeof(PlayerController))) return false;
        player->controller->control_rotation = g_practice_spawn_rotation;
    }
    return true;
}

const LevelState* PracticeLevelState() {
    void* instance = GetGameInstance();
    if (!instance) return nullptr;
    const auto* state = reinterpret_cast<const LevelState*>(
        static_cast<const uint8_t*>(instance) + 0x9C);
    if (!IsReadable(state, sizeof(LevelState))) return nullptr;
    if (state->level < 0 || state->level > 31 ||
        state->buttons < 0 || state->buttons > kRefunctButtonCount) return nullptr;
    return state;
}

namespace {
// The native object arrays are stable during progression in one world.
// The first verified full scan can be reused for subsequent unlocked levels;
// a new character, world, reset or inconsistent native counter invalidates it.
struct LoadedNativeObjectCounts {
    bool valid = false;
    MyCharacter* player = nullptr;
    void* world = nullptr;
    int resets = -1;
    std::array<int, kPracticeClusterCount> cubes{};
    std::array<int, kPracticeClusterCount> tiles{};
};
LoadedNativeObjectCounts s_loaded_native_counts{};

bool SummarizeLoadedCounts(const LoadedNativeObjectCounts& cache,
                           const LevelState& state, ProgressCounters& result) {
    if (!cache.valid) return false;
    const int unlocked = std::clamp(state.level + 1, 0, kPracticeClusterCount);
    int cubes = 0, tiles = 0;
    for (int i = 0; i < unlocked; ++i) {
        cubes += cache.cubes[i];
        tiles += cache.tiles[i];
    }
    if (cubes < state.cubes || tiles < state.platforms) return false;
    result.loaded_cubes = cubes;
    result.loaded_tiles = tiles;
    result.loaded_valid = true;
    return true;
}
} // namespace

void UpdateProgressCounters() {
    const LevelState* state = PracticeLevelState();
    if (!state || state->cubes < 0 || state->cubes > kTotalCubes ||
        state->platforms < 0 || state->platforms > kTotalColourableTiles) {
        g_progress_counters.valid = false;
        g_progress_counters.loaded_valid = false;
        g_progress_counters.cached_level = -1;
        s_loaded_native_counts.valid = false;
        return;
    }
    g_progress_counters.cubes = state->cubes;
    g_progress_counters.tiles = state->platforms;
    g_progress_counters.valid = true;

    // The whole-map totals are cheap and do not need any object discovery.
    if (g_cube_display_mode != ProgressDisplayMode::Loaded &&
        g_tile_display_mode != ProgressDisplayMode::Loaded) return;

    // A level unlock changes the denominator, but not the native 31-root
    // arrays. Scan the world only once per world/restart, rather than on
    // every level progression. Failures retain the existing retry policy.
    auto& progress = g_progress_counters;
    if (progress.loaded_valid && s_loaded_native_counts.valid &&
        s_loaded_native_counts.player == g_player &&
        progress.cached_level == state->level &&
        progress.cached_resets == state->resets) return;
    const ULONGLONG now = GetTickCount64();
    if (progress.cached_level == state->level &&
        progress.cached_resets == state->resets &&
        s_loaded_native_counts.player == g_player &&
        now < progress.next_loaded_refresh) return;
    progress.cached_level = state->level;
    progress.cached_resets = state->resets;
    progress.next_loaded_refresh = now + 1000;
    progress.loaded_valid = false;

    if (!g_game_module) return;
    const uintptr_t base = reinterpret_cast<uintptr_t>(g_game_module);
    const auto* world_slot = reinterpret_cast<void* const*>(base + kGlobalWorldRva);
    if (!IsReadable(world_slot, sizeof(void*))) return;
    void* world = *world_slot;
    if (!world) return;
    if (s_loaded_native_counts.valid &&
        s_loaded_native_counts.player == g_player &&
        s_loaded_native_counts.world == world &&
        s_loaded_native_counts.resets == state->resets) {
        if (SummarizeLoadedCounts(s_loaded_native_counts, *state, progress)) return;
        // Native counters no longer agree with the snapshot: rescan instead
        // of showing an incorrect denominator.
    }
    s_loaded_native_counts.valid = false;
    const auto* objects = reinterpret_cast<const GlobalObjectArray*>(base + kGlobalObjectArrayRva);
    if (!IsReadable(objects, sizeof(*objects)) || !objects->objects ||
        objects->num_elements < 1 || objects->num_elements > 500000 ||
        objects->max_elements < objects->num_elements ||
        !IsReadable(objects->objects, sizeof(GlobalObjectItem) *
                                     static_cast<size_t>(objects->num_elements))) return;

    std::array<int, kPracticeClusterCount> cubes_by_level{};
    std::array<int, kPracticeClusterCount> tiles_by_level{};
    std::array<bool, kPracticeClusterCount> found{};
    void* level_root_class = nullptr;
    int32_t index_offset = -1;
    int32_t cubes_offset = -1;
    int32_t tiles_offset = -1;
    std::unordered_map<void*, bool> class_cache;
    class_cache.reserve(128);
    for (int32_t i = 0; i < objects->num_elements; ++i) {
        const auto& item = objects->objects[i];
        if (!item.object || (static_cast<uint32_t>(item.flags) & 0x30000000u) ||
            !IsReadable(item.object, sizeof(UObjectHeader))) continue;
        void* klass = item.object->class_ptr;
        if (!klass || !IsReadable(klass, sizeof(UObjectHeader))) continue;
        if (level_root_class && klass != level_root_class) continue;
        if (!level_root_class) {
            auto iter = class_cache.find(klass);
            bool is_root = false;
            if (iter == class_cache.end()) {
                is_root = UnrealName(static_cast<const UObjectHeader*>(klass)->name) == L"BP_LevelRoot_C";
                class_cache.emplace(klass, is_root);
            } else is_root = iter->second;
            if (!is_root) continue;
            level_root_class = klass;
            index_offset = FindClassPropertyOffset(klass, L"LevelIndex");
            cubes_offset = FindClassPropertyOffset(klass, L"Collectibles");
            tiles_offset = FindClassPropertyOffset(klass, L"FertileLands");
            if (index_offset < 0 || cubes_offset < 0 || tiles_offset < 0) return;
        }

        const auto* data = reinterpret_cast<const uint8_t*>(item.object);
        if (!IsReadable(data + index_offset, sizeof(int32_t)) ||
            !IsReadable(data + cubes_offset, sizeof(UObjectPointerArray)) ||
            !IsReadable(data + tiles_offset, sizeof(UObjectPointerArray))) continue;
        const int index = *reinterpret_cast<const int32_t*>(data + index_offset);
        if (index < 0 || index >= kPracticeClusterCount) continue;
        const auto& cubes = *reinterpret_cast<const UObjectPointerArray*>(data + cubes_offset);
        const auto& tiles = *reinterpret_cast<const UObjectPointerArray*>(data + tiles_offset);
        const auto valid_array = [](const UObjectPointerArray& array, int max) {
            return array.count >= 0 && array.count <= max &&
                array.capacity >= array.count && array.capacity <= 10000 &&
                (array.count == 0 || (array.data &&
                    IsReadable(array.data, sizeof(void*) * static_cast<size_t>(array.count))));
        };
        if (!valid_array(cubes, kTotalCubes) ||
            !valid_array(tiles, kTotalColourableTiles)) continue;
        // Prefer the most recently registered valid root when an old world
        // is still awaiting garbage collection after a restart.
        found[index] = true;
        cubes_by_level[index] = cubes.count;
        tiles_by_level[index] = tiles.count;
    }
    if (!std::all_of(found.begin(), found.end(), [](bool value) { return value; })) return;
    int total_cubes = 0, total_tiles = 0;
    for (int i = 0; i < kPracticeClusterCount; ++i) {
        total_cubes += cubes_by_level[i];
        total_tiles += tiles_by_level[i];
    }
    // Verify the source arrays truly represent the same 18/251 objects as
    // Refunct's native progress counters. Wrong builds stay unreadable, not
    // misleadingly treated as zero.
    if (total_cubes != kTotalCubes || total_tiles != kTotalColourableTiles) return;

    int loaded_cubes = 0, loaded_tiles = 0;
    const int unlocked_count = std::clamp(state->level + 1, 0, kPracticeClusterCount);
    for (int i = 0; i < unlocked_count; ++i) {
        loaded_cubes += cubes_by_level[i];
        loaded_tiles += tiles_by_level[i];
    }
    if (loaded_cubes < state->cubes || loaded_tiles < state->platforms) return;
    s_loaded_native_counts.player = g_player;
    s_loaded_native_counts.world = world;
    s_loaded_native_counts.resets = state->resets;
    s_loaded_native_counts.cubes = cubes_by_level;
    s_loaded_native_counts.tiles = tiles_by_level;
    s_loaded_native_counts.valid = true;
    progress.loaded_cubes = loaded_cubes;
    progress.loaded_tiles = loaded_tiles;
    progress.loaded_valid = true;
}

// Save states are for the original sequential Refunct map. Reject corrupt
// disk values instead of passing arbitrary coordinates into the game.
void SaveCurrentCheckpoint(MyCharacter* player) {
    if (g_practice_active && g_practice_phase != PracticePhase::Ready) {
        Notify(L"Finish loading the section before saving a state");
        return;
    }
    Checkpoint checkpoint{};
    if (!CaptureCheckpoint(player, checkpoint)) {
        Notify(L"Save state unavailable: no valid game state");
        return;
    }
    g_saved_checkpoint = checkpoint;
    g_saved_checkpoint_valid = true;
    const bool saved_to_disk = PersistCheckpoint(checkpoint);
    Notify(L"Saved state: " + std::to_wstring(checkpoint.buttons) +
           L" buttons" + (saved_to_disk ? L"" : L" (memory only)"));
    if (!saved_to_disk) Log(L"Save state: write failed; checkpoint remains in memory");
}

void PracticeFailed(const wchar_t* reason) {
    g_practice_gold_anchor_button = 0;
    g_practice_gold_anchor_seconds = -1.0;
    g_practice_section_anchor_button = 0;
    g_practice_section_anchor_seconds = -1.0;
    RestorePracticeClusterSpeeds();
    Log(reason);
    Notify(reason);
    g_practice_phase = PracticePhase::Failed;
    g_practice_active = true; // Still prevents a modified run from recording a PB.
    g_split_popup_expires = 0;
}

std::wstring PracticeStatus() {
    switch (g_practice_phase) {
        case PracticePhase::Idle: return L"Not active (select button 2-37 to jump)";
        case PracticePhase::AwaitRestart: return L"Restarting run...";
        case PracticePhase::Warmup: return L"Preparing level...";
        case PracticePhase::MoveWaypoint:
        case PracticePhase::WaitForButton:
            return (g_progress_rise_animation_this_load ? L"Raising platforms: " : L"Loading button ") +
                   ButtonLabel(g_practice_completed_buttons + 1) +
                   L" (" + std::to_wstring(g_practice_completed_buttons + 1) +
                   L"/" + std::to_wstring(g_practice_target_button - 1) + L")";
        case PracticePhase::PlaceTarget: return L"Placing player...";
        case PracticePhase::Stabilize: return L"Stabilizing position...";
        case PracticePhase::WaitForTargetButton:
            return L"Raising ground from button " + ButtonLabel(g_practice_animate_button) + L"...";
        case PracticePhase::Ready:
            if (g_practice_origin == PracticeOrigin::SavedState)
                return L"Saved state restored (" +
                       std::to_wstring(g_practice_target_button - 1) + L" buttons)";
            if (g_practice_origin == PracticeOrigin::ProgressRestart)
                return L"Progress restored: " +
                       std::to_wstring(g_practice_target_button - 1) +
                       (g_progress_rise_animation_this_load
                           ? L" buttons (animated; at spawn; records disabled)"
                           : L" buttons (at spawn; records disabled)");
            if (g_practice_origin == PracticeOrigin::LastButton)
                return L"Last button " + ButtonLabel(g_practice_animate_button) +
                       L" replayed (ground rising)";
            if (g_practice_animate_button > 0)
                return L"Button " + ButtonLabel(g_practice_animate_button) +
                       L" replayed (ground rising)";
            return L"Ready at button " + ButtonLabel(g_practice_target_button) +
                   (g_practice_fast_rise_succeeded ? L" (fast load)" : L" (normal load)");
        case PracticePhase::Failed: return L"Failed; restart or retry";
    }
    return L"Unknown";
}

// Shared restore path: replay N native button activations, then place the
// character at either a built-in section coordinate or a saved exact pose.
// Allow N=37 (all buttons); target_button is then 38 as a count sentinel.
void RequestPreparedPractice(int buttons_to_restore, Vec3 spawn,
                             const Rotator* rotation, PracticeOrigin origin,
                             int animate_button) {
    if (buttons_to_restore < 0 || buttons_to_restore > kRefunctButtonCount) return;
    if (g_practice_active && g_practice_phase != PracticePhase::Idle &&
        g_practice_phase != PracticePhase::Ready &&
        g_practice_phase != PracticePhase::Failed) {
        Notify(L"A checkpoint is already loading");
        return;
    }
    CancelNormalFastRise();
    RestorePracticeClusterSpeeds();
    if (animate_button != 0 && animate_button != buttons_to_restore + 1) return;
    g_practice_target_button = buttons_to_restore + 1;
    g_practice_origin = origin;
    s_last_button_stance_pending =
        origin == PracticeOrigin::LastButton && s_selected_contact.stance_valid;
    s_last_button_stance_target = s_selected_contact.crouched;
    s_last_button_stance_attempts = 0;
    s_last_button_momentum_pending =
        origin == PracticeOrigin::LastButton && g_keep_last_button_momentum &&
        s_selected_contact.velocity_valid;
    s_last_button_momentum_target = s_selected_contact.velocity;
    g_practice_animate_button = animate_button;
    g_progress_rise_animation_this_load =
        origin == PracticeOrigin::ProgressRestart && g_progress_rise_animation_enabled;
    g_progress_next_button_at = 0;
    g_practice_gold_anchor_button = 0;
    g_practice_gold_anchor_seconds = -1.0;
    g_practice_section_anchor_button = 0;
    g_practice_section_anchor_seconds = -1.0;
    g_practice_spawn = spawn;
    g_practice_restore_rotation = rotation != nullptr;
    g_practice_spawn_rotation = rotation ? *rotation : Rotator{};
    g_practice_capture_reset_spawn = origin == PracticeOrigin::ProgressRestart;
    const LevelState* before_reset = PracticeLevelState();
    g_practice_resets_at_request = before_reset ? before_reset->resets : -1;
    g_practice_fast_rise_attempted = false;
    g_practice_fast_rise_succeeded = false;
    g_practice_restart_requested = true; // Perform restart *after* original character tick.
    g_practice_active = true;
    // A loaded checkpoint is a cheat, not an unconditional record ban.
    // Keep it tainted throughout the native restart and synthetic button
    // replay. The per-run Save With Cheats mode controls what may be saved.
    // Progress Restart is deliberately excluded from this permission model.
    if (origin != PracticeOrigin::ProgressRestart) {
        g_recording_tainted = true;
        // A checkpoint starts its own run; don't inherit a fast-R restriction
        // from the previous run. The checkpoint taint still remains in force.
        g_normal_fast_restart_assisted = false;
    }
    g_run_pb_eligible = false;
    if (origin != PracticeOrigin::ProgressRestart)
        MarkCheatAction(CheatAction::Checkpoint);
    g_practice_phase = PracticePhase::AwaitRestart;
    g_practice_completed_buttons = 0;
    g_practice_waypoint = 0;
    g_practice_button_actors_ready = false;
    g_practice_button_actors.fill(nullptr);
    g_practice_level_actors.fill(nullptr);
    g_practice_level_class = nullptr;
    g_practice_speed_offset = -1;
    g_practice_retries = 0;
    g_practice_started = GetTickCount64();
    // Prime the fast-rise speed BEFORE RestartGame as well. This covers the
    // earliest part of Refunct's reset animation; after restart we re-resolve
    // all actors and reapply to the fresh world in Warmup.
    if (!g_progress_rise_animation_this_load && DiscoverPracticeButtons()) {
        if (AcceleratePracticeClusters())
            Log(L"Section practice: fast rise primed before restart");
    } else if (g_progress_rise_animation_this_load) {
        Log(L"Progress restart: native rise speeds retained (animated mode)");
    }
    if (origin == PracticeOrigin::Section)
        Notify(L"Preparing section " + ButtonLabel(g_practice_target_button));
    else if (origin == PracticeOrigin::ProgressRestart)
        Notify(L"Restarting at spawn; keeping " +
               std::to_wstring(buttons_to_restore) + L" pressed buttons");
    else
        Notify(L"Restoring state: " + std::to_wstring(buttons_to_restore) + L" buttons");
}

// All button-based jumps use this single path. The origin only selects the
// status message; F2 and B get identical position, progression, and animation.
void RequestPractice(int target_button,
                     PracticeOrigin origin) {
    if (target_button < 1 || target_button > kRefunctButtonCount) return;
    // Restore every earlier button, place the player at the true centre of
    // this button, and activate it at vanilla speed so its ground rises.
    RequestPreparedPractice(target_button - 1, PracticeSpawnForButton(target_button),
                            nullptr, origin, target_button);
}

void LoadSavedCheckpoint() {
    if (!g_saved_checkpoint_valid || !ValidCheckpoint(g_saved_checkpoint)) {
        Notify(L"No saved state yet - save one first");
        return;
    }
    const Checkpoint snapshot = g_saved_checkpoint;
    RequestPreparedPractice(snapshot.buttons, snapshot.location,
                            &snapshot.rotation, PracticeOrigin::SavedState);
}

namespace {
void LoadButtonContact(const ButtonContact& contact, const wchar_t* empty_message) {
    if (!contact.valid || !ValidCheckpoint(contact.checkpoint) ||
        contact.checkpoint.buttons < 1 ||
        contact.checkpoint.buttons > kRefunctButtonCount) {
        Notify(empty_message);
        return;
    }
    // RequestPreparedPractice uses this snapshot only when loading a saved
    // button. F1/F2/H continue through their normal restore paths.
    s_selected_contact = contact;
    const int button = contact.checkpoint.buttons;
    Vec3 spawn = PracticeSpawnForButton(button);
    // The normal section spawn holds the capsule 90 units above the button.
    // For B/N checkpoint replays that leaves a slight visible air gap.
    // Lower only these replays to the usual standing capsule half-height;
    // the game's collision can resolve the final resting contact naturally.
    // Keep the button centre, rotation, crouch and optional velocity unchanged.
    constexpr float kLastButtonSpawnHeightCorrection = 2.0f;
    spawn.z -= kLastButtonSpawnHeightCorrection;
    RequestPreparedPractice(button - 1, spawn,
                            &s_selected_contact.checkpoint.rotation,
                            PracticeOrigin::LastButton, button);
}
} // namespace

void LoadLastButtonCheckpoint() {
    LoadButtonHistoryOnce();
    if (!g_last_button_checkpoint_valid) {
        Notify(L"No last button yet - press a button first");
        return;
    }
    LoadButtonContact(s_last_contact,
                      L"No last button yet - press a button first");
}

void TeleportToLastPressedButton(MyCharacter* player) {
    LoadButtonHistoryOnce();
    // Position-only action. The last button is global, but the currently
    // selected map is deliberately not changed by teleporting.
    if (!ValidButtonContact(s_global_last_contact)) {
        Log(L"Button teleport: request rejected - no recorded real button");
        Notify(L"No last button yet - press a button first");
        return;
    }
    if (!player || !IsReadable(player, sizeof(*player))) {
        Log(L"Button teleport: request rejected - player unavailable");
        Notify(L"Teleport unavailable: player is not ready");
        return;
    }
    const auto& checkpoint = s_global_last_contact.checkpoint;
    s_pending_global_teleport = {};
    s_pending_global_teleport.active = true;
    s_pending_global_teleport.destination = checkpoint.location;
    s_pending_global_teleport.button_number = checkpoint.buttons;
    s_pending_global_teleport.requested_ms = GetTickCount64();
    Log((L"Button teleport: REQUEST global button " +
         std::to_wstring(checkpoint.buttons) + L" X=" +
         CheckpointFloat(checkpoint.location.x) + L" Y=" +
         CheckpointFloat(checkpoint.location.y) + L" Z=" +
         CheckpointFloat(checkpoint.location.z) +
         L" custom_map=" + (CustomMapActive() ? L"yes" : L"no")).c_str());
    Notify(L"Teleport to global last button queued");
}

void UpdateGlobalLastButtonTeleport(MyCharacter* player) {
    if (!s_pending_global_teleport.active) return;
    auto& request = s_pending_global_teleport;
    constexpr ULONGLONG kTimeoutMs = 10000;
    const ULONGLONG now = GetTickCount64();
    if (now - request.requested_ms > kTimeoutMs) {
        Log(L"Button teleport: FAILED - map/checkpoint never became ready");
        Notify(L"Last-button teleport timed out; check RefunctUtility log");
        request = {};
        return;
    }
    if (CustomMapApplying() ||
        (g_practice_active && g_practice_phase != PracticePhase::Ready) ||
        g_restart_requested || g_practice_restart_requested ||
        g_full_level_reload_requested || g_preloaded_restart_requested) {
        if (!request.waiting_logged) {
            Log(L"Button teleport: waiting for map/restart/checkpoint to finish");
            request.waiting_logged = true;
        }
        return;
    }
    if (!player || !IsReadable(player, sizeof(*player)) ||
        !IsReadable(player->root_component, sizeof(SceneComponent)) ||
        !IsReadable(player->movement, sizeof(MovementComponent))) {
        Log(L"Button teleport: FAILED - player actor unavailable");
        Notify(L"Last-button teleport failed: player unavailable");
        request = {};
        return;
    }
    if (!request.placed) {
        // Execute after the original Refunct tick, not during menu input.
        // Otherwise a native checkpoint spawn can immediately overwrite it.
        if (!TeleportToCoordinates(player, request.destination, true)) {
            Log(L"Button teleport: FAILED - coordinate teleport rejected");
            request = {};
            return;
        }
        request.placed = true;
        request.settle_frames = 3;
        Log(L"Button teleport: APPLY position-only teleport after native tick");
        return;
    }
    // The last three native ticks may settle collision/section transforms.
    // Hold the original contact pose briefly (without button replay).
    // Never touch controller rotation, button counts or map geometry.
    const Vec3 target = request.destination;
    const Vec3 current = player->root_component->location;
    if (!std::isfinite(current.x) || !std::isfinite(current.y) ||
        !std::isfinite(current.z)) {
        Log(L"Button teleport: FAILED - invalid player location readback");
        request = {};
        return;
    }
    if (std::abs(current.x - target.x) > 2.f ||
        std::abs(current.y - target.y) > 2.f ||
        std::abs(current.z - target.z) > 2.f) {
        player->root_component->location = target;
        player->movement->velocity = Vec3{};
        player->movement->acceleration = Vec3{};
    }
    if (--request.settle_frames > 0) return;
    const Vec3 final_position = player->root_component->location;
    Log((L"Button teleport: COMPLETE button " +
         std::to_wstring(request.button_number) + L" player X=" +
         CheckpointFloat(final_position.x) + L" Y=" +
         CheckpointFloat(final_position.y) + L" Z=" +
         CheckpointFloat(final_position.z)).c_str());
    Notify(L"Teleported to global last button " + ButtonLabel(request.button_number) +
           L" (progress unchanged)");
    request = {};
}

int SecondLastButtonNumber() {
    LoadButtonHistoryOnce();
    if (!g_last_button_checkpoint_valid || !ValidCheckpoint(g_last_button_checkpoint) ||
        g_last_button_checkpoint.buttons < 2) return 0;
    // If a saved-state restore skipped the previous real press, still allow N
    // to load the previous numbered section without inventing a saved pose.
    const int predecessor = g_last_button_checkpoint.buttons - 1;
    return ValidButtonContact(s_second_last_contact) &&
                   s_second_last_contact.checkpoint.buttons == predecessor
        ? s_second_last_contact.checkpoint.buttons : predecessor;
}

void LoadSecondLastButtonCheckpoint() {
    const int button = SecondLastButtonNumber();
    if (!button) {
        Notify(L"No second last button yet - press two buttons first");
        return;
    }
    if (ValidButtonContact(s_second_last_contact) &&
        s_second_last_contact.checkpoint.buttons == button) {
        LoadButtonContact(s_second_last_contact,
                          L"No second last button yet - press two buttons first");
        return;
    }
    // A missing real contact cannot restore stance, velocity, or view angle.
    // Use vanilla section orientation at the regular B/N adjusted spawn.
    s_selected_contact = {};
    Vec3 spawn = PracticeSpawnForButton(button);
    constexpr float kLastButtonSpawnHeightCorrection = 2.0f;
    spawn.z -= kLastButtonSpawnHeightCorrection;
    RequestPreparedPractice(button - 1, spawn, nullptr,
                            PracticeOrigin::LastButton, button);
}

// The utility's explicit R restart resets the observer counters before the
// next game tick. Clear the on-disk history at the same time, otherwise the
// previous run would reappear after quitting and reopening the game.
void ClearButtonHistoryOnRestart() {
    // Read the saved Global target before clearing this run's B/N contacts.
    // This also preserves global history if R is pressed immediately on launch.
    LoadButtonHistoryOnce();
    s_last_contact = {};
    s_second_last_contact = {};
    s_selected_contact = {};
    s_last_button_stance_pending = false;
    s_last_button_momentum_pending = false;
    g_last_button_checkpoint = {};
    g_last_button_checkpoint_valid = false;
    PersistButtonHistory();
}

// Restart with Progress uses the original button-restoration path only.
// It restarts Refunct, replays the buttons already pressed, and never travels
// into map travel or read Refunct.sav. Assisted PB/gold saving stays blocked.
void RequestProgressRestart() {
    if (g_progress_restart_pending) {
        Notify(L"A progress restart is already requested");
        return;
    }
    if (g_practice_active && g_practice_phase != PracticePhase::Idle &&
        g_practice_phase != PracticePhase::Ready &&
        g_practice_phase != PracticePhase::Failed) {
        Notify(L"Wait for the current checkpoint to finish loading");
        return;
    }
    if (g_preloaded_restart_armed || g_preloaded_reset_seen ||
        g_preloaded_restart_requested || g_restart_requested ||
        g_practice_restart_requested) {
        Notify(L"Wait for the current restart to finish");
        return;
    }

    const LevelState* state = PracticeLevelState();
    if (!state || state->buttons < 0 || state->buttons > kRefunctButtonCount) {
        Notify(L"Cannot read current Refunct button progression");
        return;
    }
    const int pressed = state->buttons;
    if (pressed == 0) {
        g_restart_requested = true;
        Notify(L"No pressed buttons to keep; restarting normally");
        return;
    }
    // Replays the game's real button activations. Native spawn handling and
    // existing position stabilisation are retained by RequestPreparedPractice.
    RequestPreparedPractice(pressed, Vec3{}, nullptr, PracticeOrigin::ProgressRestart);
    if (g_practice_phase != PracticePhase::AwaitRestart ||
        g_practice_origin != PracticeOrigin::ProgressRestart) return;

    g_progress_restart_active = true;
    g_progress_restart_pending = true;
    g_recording_tainted = true;
    g_run_pb_eligible = false;
    g_practice_gold_anchor_button = 0;
    g_practice_gold_anchor_seconds = -1.0;
    g_practice_section_anchor_button = 0;
    g_practice_section_anchor_seconds = -1.0;
    Log((L"Restart with progress: button restoration for " +
         std::to_wstring(pressed) + L" pressed buttons (" +
         (g_progress_rise_animation_this_load ? L"animated" : L"fast") +
         L" rise)").c_str());
}

// TAS New Game Actions -> All Buttons (tool/newgame.re): at the native
// AMyCharacter::ForcedUnCrouch event, set LevelState.level to 29 *before*
// calling the original function. The game processes the new-game transition
// itself, including spawning the player at the genuine beginning.
// Do not call this hook directly: MinHook invokes it from Refunct's game thread.
void __fastcall NewGameEventHook(MyCharacter* player, void* /*edx*/) {
    if (g_preloaded_restart_armed && !g_preloaded_new_game_seen) {
        if (LevelState* state = const_cast<LevelState*>(PracticeLevelState())) {
            state->level = 29;
            g_preloaded_new_game_seen = true;
            Log(L"Preloaded restart: TAS new-game event, LevelState.level=29");
        }
    }
    // A native menu restart can bypass the utility R hotkey. Do not do a
    // fresh global object scan *inside* this engine callback; the map may be
    // tearing down. Clear the old assisted-run flag only when this is a new,
    // unmodified native restart (not a preloaded/checkpoint/fast-R transition).
    if (!g_normal_fast_rise_active && !g_preloaded_restart_armed &&
        !g_preloaded_reset_seen && !g_preloaded_restart_active &&
        !g_practice_active && !g_progress_restart_active)
        g_normal_fast_restart_assisted = false;
    if (g_original_new_game_event) g_original_new_game_event(player);
}

// Follow the second half of the TAS sequence: on the actual reset-counter
// transition, set the level counter back to zero. Never fake the counter,
// button activations or the player position.
void UpdatePreloadedRestart() {
    if (!g_preloaded_restart_armed && !g_preloaded_reset_seen) return;
    const ULONGLONG now = GetTickCount64();
    const LevelState* read_state = PracticeLevelState();
    if (g_preloaded_restart_armed && g_preloaded_new_game_seen && read_state &&
        read_state->buttons == 0 &&
        (g_preloaded_base_resets < 0 || read_state->resets != g_preloaded_base_resets)) {
        auto* state = const_cast<LevelState*>(read_state);
        state->level = 0;
        g_preloaded_restart_armed = false;
        g_preloaded_reset_seen = true;
        g_preloaded_restart_confirmed = true;
        g_preloaded_ready_at = now + 1500;
        Log(L"Preloaded restart: native reset observed; level counter restored to zero");
    }
    // A missed new-game callback should fail visibly instead of pretending
    // the game loaded a pre-raised map. Remain ineligible for records.
    if (g_preloaded_restart_armed && now - g_preloaded_started_at > 10000) {
        g_preloaded_restart_armed = false;
        g_preloaded_reset_seen = false;
        g_preloaded_restart_failed = true;
        RestorePracticeClusterSpeeds();
        Notify(L"Preloaded restart not confirmed; check RefunctUtility log");
        Log(L"WARNING: preloaded restart timed out without native reset confirmation");
    }
    if (g_preloaded_reset_seen && now >= g_preloaded_ready_at) {
        g_preloaded_reset_seen = false;
        RestorePracticeClusterSpeeds();
        Notify(g_preloaded_fast_rise_used
            ? L"Preloaded map: restart complete (records disabled)"
            : L"Preloaded map requested (rise speed not modified)");
        Log(L"Preloaded restart: setup concluded; best-segment/PB saving stays blocked");
    }
}

// Capture the latest *real* press on the native ALiftBase contact edge.
// Polling the total button count once per frame misses contacts if multiple
// buttons are triggered in one tick (possible on custom TAS maps). This
// snapshot preserves the position *at the press*, not at a later game tick.
void CaptureGlobalLastButtonContact(MyCharacter* player,
                                    int before_buttons, int after_buttons) {
    if (!player || before_buttons < 0 ||
        after_buttons <= before_buttons ||
        after_buttons > kRefunctButtonCount ||
        IsSyntheticPracticeButtonContact() ||
        (g_practice_active && g_practice_phase != PracticePhase::Ready)) return;
    LoadButtonHistoryOnce();
    Checkpoint snapshot{};
    if (!CaptureCheckpoint(player, snapshot) ||
        snapshot.buttons != after_buttons) {
        Log((L"Button teleport: native press advanced to " +
             std::to_wstring(after_buttons) +
             L" but the live player position was unavailable").c_str());
        return;
    }
    ButtonContact contact{};
    contact.checkpoint = snapshot;
    contact.valid = true;
    const auto native_crouch = ReadCrouchedState(player);
    const bool crouch_key = !g_flying && !g_noclip &&
        HasFocusedGameWindow() &&
        (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    contact.crouched =
        (native_crouch.valid && native_crouch.value) || crouch_key;
    contact.stance_valid = true;
    if (IsReadable(player->movement, sizeof(MovementComponent))) {
        const Vec3 measured = player->movement->velocity;
        if (ValidLastButtonVelocity(measured)) {
            contact.velocity = measured;
            contact.velocity_valid = true;
        }
    }
    s_global_last_contact = contact;
    s_native_global_contact_since_tick = true;
    PersistButtonHistory();
    Log((L"Button teleport: native last button " +
         std::to_wstring(after_buttons) + L" at X=" +
         CheckpointFloat(snapshot.location.x) + L" Y=" +
         CheckpointFloat(snapshot.location.y) + L" Z=" +
         CheckpointFloat(snapshot.location.z)).c_str());
}

// Called once per game tick after the original engine tick. Retain the
// vanilla B/N checkpoint logic; only the global position comes from the
// exact native contact when it was observed in this frame.
void TrackLastButton(MyCharacter* player) {
    LoadButtonHistoryOnce();
    const bool native_global_contact = s_native_global_contact_since_tick;
    s_native_global_contact_since_tick = false;
    const LevelState* state = PracticeLevelState();
    if (!state) return;
    if (g_observed_buttons < 0 || g_observed_resets < 0) {
        g_observed_buttons = state->buttons;
        g_observed_resets = state->resets;
        return;
    }
    const bool reset = state->buttons < g_observed_buttons ||
                       state->resets != g_observed_resets;
    if (reset && !g_practice_active) {
        if (!g_preloaded_restart_armed && !g_preloaded_reset_seen)
            g_preloaded_restart_active = false;
        g_last_button_checkpoint_valid = false;
        s_last_contact = {};
        s_second_last_contact = {};
        s_selected_contact = {};
        s_last_button_stance_pending = false;
        s_last_button_momentum_pending = false;
        PersistButtonHistory();
        // Also cover Refunct's native restart action when it bypasses the
        // utility's R binding. The internal checkpoint loader is excluded.
        if (state->buttons == 0) ResetMovementOnNormalRestart(player);
    }
    if (!reset && state->buttons == g_observed_buttons + 1 &&
        (!g_practice_active || g_practice_phase == PracticePhase::Ready)) {
        Checkpoint snapshot{};
        if (CaptureCheckpoint(player, snapshot) &&
            snapshot.buttons == state->buttons) {
            ButtonContact contact{};
            contact.checkpoint = snapshot;
            contact.valid = true;
            const auto native_crouch = ReadCrouchedState(player);
            // Refunct can still be sliding before bIsCrouched is set.
            // Respect the stock Shift key unless flying/noclip is active.
            const bool crouch_key = !g_flying && !g_noclip &&
                HasFocusedGameWindow() &&
                (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            contact.crouched =
                (native_crouch.valid && native_crouch.value) || crouch_key;
            contact.stance_valid = true;
            if (IsReadable(player->movement, sizeof(MovementComponent))) {
                const Vec3 measured = player->movement->velocity;
                if (ValidLastButtonVelocity(measured)) {
                    contact.velocity = measured;
                    contact.velocity_valid = true;
                }
            }
            // Prefer the last checkpoint if the run advanced normally.
            // After N loads the earlier checkpoint, that earlier contact is
            // still the correct predecessor when the next button is pressed.
            // All other jumps/branches discard unrelated history.
            if (g_last_button_checkpoint_valid && s_last_contact.valid &&
                s_last_contact.checkpoint.buttons == g_observed_buttons) {
                s_second_last_contact = s_last_contact;
            } else if (!(g_last_button_checkpoint_valid &&
                         s_second_last_contact.valid &&
                         s_second_last_contact.checkpoint.buttons == g_observed_buttons)) {
                s_second_last_contact = {};
            }
            s_last_contact = contact;
            // No native contact edge was observed this frame: keep the
            // original polling fallback for an unavailable contact hook.
            // Otherwise do not overwrite the exact event-time position.
            if (!native_global_contact) s_global_last_contact = contact;
            g_last_button_checkpoint = snapshot;
            g_last_button_checkpoint_valid = true;
            PersistButtonHistory();
        }
    }
    g_observed_buttons = state->buttons;
    g_observed_resets = state->resets;
}

// Called on the game thread after the original tick. Every earlier button
// is triggered via Refunct's native game logic and its real counter checked.
void UpdatePractice(MyCharacter* player) {
    if (!g_practice_active || !player) return;
    g_run_pb_eligible = false;
    if (g_practice_phase == PracticePhase::Ready) {
        if (g_practice_origin == PracticeOrigin::LastButton) {
            RestoreLastButtonStance(player);
            if (!s_last_button_stance_pending)
                RestoreLastButtonMomentum(player);
        }
        return;
    }
    if (g_practice_phase == PracticePhase::Idle ||
        g_practice_phase == PracticePhase::Failed) return;
    const ULONGLONG now = GetTickCount64();
    const auto* state = PracticeLevelState();
    if (g_practice_phase == PracticePhase::AwaitRestart) {
        const bool reset_confirmed = !g_practice_capture_reset_spawn ||
            (state && g_practice_resets_at_request >= 0 &&
             state->resets != g_practice_resets_at_request);
        if (state && state->buttons == 0 && state->level <= 1 && reset_confirmed) {
            if (g_practice_capture_reset_spawn) {
                if (!IsReadable(player->root_component, sizeof(SceneComponent))) {
                    PracticeFailed(L"Progress restart: cannot read native spawn location");
                    return;
                }
                const Vec3 native_spawn = player->root_component->location;
                if (!std::isfinite(native_spawn.x) || !std::isfinite(native_spawn.y) ||
                    !std::isfinite(native_spawn.z)) {
                    PracticeFailed(L"Progress restart: spawn coordinates invalid");
                    return;
                }
                g_practice_spawn = native_spawn;
                g_practice_capture_reset_spawn = false;
                if (g_progress_rise_animation_this_load) {
                    // Position the player at the fresh native spawn BEFORE
                    // activating buttons. This allows the rise animation to
                    // be watched from spawn rather than teleporting afterward.
                    if (!SetPracticePosition(player, g_practice_spawn)) {
                        PracticeFailed(L"Progress restart: cannot place character at spawn");
                        return;
                    }
                    g_progress_next_button_at = now + 350;
                }
                Log((L"Progress restart: captured native spawn (" +
                     std::to_wstring(native_spawn.x) + L", " +
                     std::to_wstring(native_spawn.y) + L", " +
                     std::to_wstring(native_spawn.z) + L")").c_str());
            }
            g_practice_phase = PracticePhase::Warmup;
            g_practice_frames_left = 2;
            g_practice_step_started = now;
        } else if (now - g_practice_started > 12000) {
            PracticeFailed(L"Section practice: restart did not complete");
        }
        return;
    }
    if (!state) {
        if (now - g_practice_started > 12000)
            PracticeFailed(L"Section practice: game state not available");
        return;
    }
    if (g_practice_origin == PracticeOrigin::ProgressRestart) {
        // The native restart placed the character at the start already.
        // Keep them there while buttons are reconstructed: a delayed platform
        // rise or movement/collision update must not drop them under the map.
        // This deliberately does not apply to section or saved-state loading.
        if (!SetPracticePosition(player, g_practice_spawn)) {
            PracticeFailed(L"Progress restart: unable to hold native spawn during restoration");
            return;
        }
    }
    if (g_practice_phase == PracticePhase::Warmup) {
        if (--g_practice_frames_left > 0) return;
        if (!g_practice_button_actors_ready && !DiscoverPracticeButtons()) {
            if (now - g_practice_step_started > 8000) {
                PracticeFailed(L"Section practice: unable to find level buttons");
            } else {
                g_practice_frames_left = 12;
            }
            return;
        }
        if (!g_practice_fast_rise_attempted) {
            g_practice_fast_rise_attempted = true;
            if (g_progress_rise_animation_this_load) {
                // Native defaults: never enable the TAS fast-rise override.
                RestorePracticeClusterSpeeds();
                g_practice_fast_rise_succeeded = false;
            } else {
                g_practice_fast_rise_succeeded = AcceleratePracticeClusters();
                if (!g_practice_fast_rise_succeeded)
                    Log(L"Section practice: fast rise unavailable; continuing with vanilla speeds");
            }
        }
        // Space each native button press so the game has real time to animate
        // the platforms. No artificial level state or simulated input is used.
        if (g_progress_rise_animation_this_load && now < g_progress_next_button_at)
            return;
        g_practice_phase = g_practice_completed_buttons >= g_practice_target_button - 1
            ? PracticePhase::PlaceTarget : PracticePhase::MoveWaypoint;
        g_practice_step_started = now;
        return;
    }
    if (g_practice_phase == PracticePhase::MoveWaypoint) {
        if (g_practice_completed_buttons >= g_practice_target_button - 1) {
            g_practice_phase = PracticePhase::PlaceTarget;
            return;
        }
        if (state->buttons != g_practice_completed_buttons) {
            PracticeFailed(L"Section practice: unexpected button progression");
            return;
        }
        if (!ActivatePracticeButton(g_practice_completed_buttons, player)) {
            PracticeFailed(L"Section practice: native button activation unavailable");
            return;
        }
        g_practice_phase = PracticePhase::WaitForButton;
        g_practice_frames_left = 12;
        g_practice_step_started = now;
        return;
    }
    if (g_practice_phase == PracticePhase::WaitForButton) {
        if (state->buttons == g_practice_completed_buttons + 1) {
            ++g_practice_completed_buttons;
            g_practice_retries = 0;
            if (g_progress_rise_animation_this_load)
                g_progress_next_button_at = now + kProgressRiseIntervalMs;
            g_practice_phase = PracticePhase::Warmup;
            // Checkpoint loads can advance sooner after a confirmed native
            // button press *only* when Refunct's fast-rise override worked.
            // Still allow another game tick before the next activation, and
            // keep the original spacing for progress restarts, animations,
            // or failed fast-rise setup. Position stabilisation is unchanged.
            const bool fast_checkpoint =
                g_practice_origin != PracticeOrigin::ProgressRestart &&
                !g_progress_rise_animation_this_load &&
                g_practice_fast_rise_succeeded;
            g_practice_frames_left = fast_checkpoint ? 2 : 6;
            g_split_popup_expires = 0;
            return;
        }
        if (state->buttons > g_practice_completed_buttons + 1) {
            PracticeFailed(L"Section practice: multiple buttons activated unexpectedly");
            return;
        }
        if (--g_practice_frames_left > 0) return;
        if (++g_practice_retries >= 4 || now - g_practice_step_started > 5000) {
            const std::wstring error = L"Section practice: button " +
                ButtonLabel(g_practice_completed_buttons + 1) + L" did not activate";
            PracticeFailed(error.c_str());
            return;
        }
        if (!ActivatePracticeButton(g_practice_completed_buttons, player)) {
            PracticeFailed(L"Section practice: native button activation unavailable");
            return;
        }
        g_practice_frames_left = 12;
        return;
    }
    if (g_practice_phase == PracticePhase::PlaceTarget) {
        if (state->buttons != g_practice_target_button - 1) {
            PracticeFailed(L"Section practice: earlier buttons were not all activated");
            return;
        }
        // Earlier buttons were activated during fast setup. The selected
        // button is intentionally still unpressed so its own rise uses the
        // original Refunct animation speed, not the TAS fast-rise speed.
        RestorePracticeClusterSpeeds();
        if (g_practice_origin == PracticeOrigin::ProgressRestart) {
            // No final teleport: the captured spawn was sampled during reset
            // and may be stale by the time all platforms have finished rising.
            // The player is already held at the native spawn above while the
            // buttons are restored, so simply release them to normal physics.
            g_practice_phase = PracticePhase::Ready;
            g_run_pb_eligible = false;
            g_split_popup_expires = 0;
            Log(L"Progress restart: button restoration complete; released native spawn hold (no final teleport)");
            Notify(PracticeStatus());
            return;
        }
        if (g_practice_animate_button > 0) {
            Vec3 visual_center{};
            if (ResolveButtonBoundsCenter(g_practice_animate_button, visual_center)) {
                g_practice_spawn.x = visual_center.x;
                g_practice_spawn.y = visual_center.y;
                Log(L"Section practice: spawned at reflected button bounds centre");
            } else {
                Log(L"Section practice: using TAS button centre (bounds unavailable)");
            }
        }
        if (!SetPracticePosition(player, g_practice_spawn)) {
            PracticeFailed(L"Section practice: character teleport unavailable");
            return;
        }
        g_practice_phase = PracticePhase::Stabilize;
        g_practice_frames_left = kPracticeSpawnHoldFrames;
        g_practice_step_started = now;
        g_split_popup_expires = 0;
        return;
    }
    if (g_practice_phase == PracticePhase::Stabilize) {
        // Run after the original character tick, exactly as TAS does at the
        // new-game and reset boundaries. This prevents an in-progress reset
        // from silently putting the player back at the original spawn point.
        if (!SetPracticePosition(player, g_practice_spawn)) {
            PracticeFailed(L"Section practice: character was replaced while teleporting");
            return;
        }
        if (--g_practice_frames_left > 0 && now - g_practice_step_started < 1500)
            return;
        g_run_pb_eligible = false;
        g_split_popup_expires = 0;
        Log((L"Section practice: player placed at (" +
             std::to_wstring(g_practice_spawn.x) + L", " +
             std::to_wstring(g_practice_spawn.y) + L", " +
             std::to_wstring(g_practice_spawn.z) + L")").c_str());
        if (g_practice_animate_button > 0) {
            // If normal physics already touched the button during the hold,
            // don't press it twice. Otherwise trigger it via the same native
            // ALiftBase callback the original TAS uses.
            const int count_before = state->buttons;
            if (count_before < g_practice_animate_button - 1 ||
                count_before > g_practice_animate_button) {
                PracticeFailed(L"Section practice: unexpected final button count");
                return;
            }
            // ProcessEvent-style activation may immediately alter the world
            // (especially at button 37). Do not read state again afterward.
            if (count_before == g_practice_animate_button - 1 &&
                !ActivatePracticeButton(g_practice_animate_button - 1, player)) {
                PracticeFailed(L"Section practice: final button activation failed");
                return;
            }
            g_practice_phase = PracticePhase::WaitForTargetButton;
            g_practice_step_started = now;
            return;
        }
        g_practice_phase = PracticePhase::Ready;
        // An exact saved state has no auto-activated target button. Use its
        // restored progress as the starting boundary for the next real
        // segment. Measuring a split is independent of permission to save it.
        if (g_practice_origin == PracticeOrigin::SavedState &&
            g_engine_timer_valid) {
            g_practice_gold_anchor_button = state->buttons;
            g_practice_gold_anchor_seconds = g_elapsed_seconds;
        }
        Notify(PracticeStatus());
        return;
    }
    if (g_practice_phase == PracticePhase::WaitForTargetButton) {
        if (state->buttons == g_practice_animate_button) {
            g_practice_phase = PracticePhase::Ready;
            // The freshly activated target button is the practice segment's
            // starting boundary. Time spent rebuilding the level is excluded.
            if (g_engine_timer_valid) {
                g_practice_gold_anchor_button = state->buttons;
                // Use the native button-change timestamp when available, not
                // a tick later when the loader notices Ready.
                double anchor = g_elapsed_seconds;
                if (!g_button_splits.empty()) {
                    const auto& last = g_button_splits.back();
                    if (last.button_number == state->buttons &&
                        last.run_seconds <= anchor &&
                        anchor - last.run_seconds <= 0.25)
                        anchor = last.run_seconds;
                }
                g_practice_gold_anchor_seconds = anchor;
            }
            // This anchor is for visual comparisons only. It is independent
            // of Save With Cheats and does not change PB recording eligibility.
            if (g_practice_origin == PracticeOrigin::Section && g_engine_timer_valid) {
                g_practice_section_anchor_button = state->buttons;
                g_practice_section_anchor_seconds = g_elapsed_seconds;
                if (!g_button_splits.empty()) {
                    const auto& last = g_button_splits.back();
                    if (last.button_number == state->buttons &&
                        last.run_seconds <= g_elapsed_seconds &&
                        g_elapsed_seconds - last.run_seconds <= 0.25)
                        g_practice_section_anchor_seconds = last.run_seconds;
                }
            }
            g_run_pb_eligible = false;
            g_split_popup_expires = 0;
            Notify(PracticeStatus());
            return;
        }
        if (state->buttons > g_practice_animate_button ||
            now - g_practice_step_started > 3000) {
            PracticeFailed(L"Section practice: target button failed to register");
            return;
        }
        return;
    }
}

// Times are formatted in mm:ss.mmm, matching the native game timer.

} // namespace refunct

#include <deque>
#include "refunct/internal_api.hpp"
#include "refunct/ghost_data.hpp"
#include "refunct/ghost_furthest_data.hpp"
#include "refunct/ghost_data_paths.hpp"
#include "refunct/ghost_replay_progress.hpp"
#include "refunct/features/tiles.hpp"
#include "refunct/features/cubes.hpp"
#include "refunct/features/platforms.hpp"

namespace refunct {
namespace {
struct Capture {
    bool started = false;
    bool complete = false;
    bool usable = false;
    bool overflow_reported = false;
    double start_clock = 0.0;
    double last_elapsed = 0.0;
    std::vector<ghost_data::Sample> samples;
};
Capture s_capture{};
std::vector<ghost_data::Sample> s_playback{};
bool s_show = true;
// True by default: render the ghost in the 3D world rather than as a
// camera-facing HUD overlay. This is render-only, with no collision.
bool s_pass_through = true;
// Appearance preferences are independent for screen-space and world-space ghosts.
// Both default to their original translucent appearance.
bool s_transparency_2d = true;
bool s_transparency_3d = true;
// Shared across both renderers. 0x45EBFF matches the original cyan tint.
uint32_t s_ghost_colour_rgb = 0x45EBFFu;
constexpr uint32_t kDefaultGhostColour = 0x45EBFFu;
LinearColor GhostColourLinear() {
    return {static_cast<float>((s_ghost_colour_rgb >> 16) & 0xFFu) / 255.f,
            static_cast<float>((s_ghost_colour_rgb >> 8) & 0xFFu) / 255.f,
            static_cast<float>(s_ghost_colour_rgb & 0xFFu) / 255.f, 1.f};
}
bool s_button_sync = false;
bool s_use_fastest_sections = false; // Normal ghost setting, separate from Replay Viewer.
// Replay Viewer has its own saved source: 0 = PB, 1 = best sections.
// Unlike ordinary ghost playback, neither replay source uses furthest-run data.
bool s_replay_best_segments = false;
// Win32 mouse-hook thread only queues input. Consumed on the character tick.
std::atomic<int> s_replay_orbit_wheel_delta{0};
// UI mouse-hook callbacks are on the window thread, not the character tick.
// Publish orbit activity atomically instead of reading the live replay struct.
std::atomic_bool s_replay_orbit_active{false};
// Camera preference is separate from the transient viewer state. Rewind and
// replay restarts must not silently switch back to third-person.
// Existing saved values are preserved for backward compatibility:
// 0 = locked third person, 1 = first person, 2 = free cam, 3 = orbit third person.
// Menu cycle order: locked third person -> orbit third person -> first person -> free cam.
// The setting remains independent of the transient replay object.
int s_replay_camera_mode = 0;
// Last displayed replay pose, used only to emulate native footstep overlap.
Vec3 s_replay_tile_last_pose{};
std::uint32_t s_replay_tile_last_ms = 0;
bool s_replay_tile_pose_ready = false;
const std::vector<ghost_data::Sample>* s_replay_tile_track = nullptr;
std::uint32_t s_replay_tile_track_ms = 0;
// Independent per-button fastest movement tracks. Unlike the full PB track,
// each can come from a different (even unfinished) native run.
std::array<std::vector<ghost_data::Sample>, kRefunctButtonCount> s_fastest{};
// Before a PB ghost exists, PB.ghost stores the furthest *incomplete* run
// using the existing versioned furthest format. A valid completed PB is
// always authoritative and must never be replaced by a shorter run.
ghost_furthest_data::Run<kRefunctButtonCount> s_furthest{};
int s_furthest_checked_button = 0;
bool s_primary_pb_protected = false;
bool s_primary_ghost_invalid = false;
struct PendingFastestGhost {
    bool ready = false;
    std::uint32_t start_ms = 0;
    std::uint32_t end_ms = 0;
};
std::array<PendingFastestGhost, kRefunctButtonCount> s_pending_fastest{};
bool s_loaded_settings = false;
std::wstring s_loaded_profile;

// The HUD can render much faster than the 20-Hz recording stream. Reuse
// expensive world-to-screen projections briefly while still drawing each HUD
// frame. A changed camera/controller/viewport or run clock invalidates this.
struct GhostProjectionCache {
    void* controller = nullptr;
    int32_t viewport_width = 0;
    int32_t viewport_height = 0;
    std::uint64_t clock_ms = 0;
    std::uint32_t elapsed_ms = 0;
    bool populated = false;
    bool button_synced = false;
    bool fastest_sections = false;
    const std::vector<ghost_data::Sample>* track = nullptr;
    int button_count = -1;
    bool fastest_source = false;
    bool furthest_source = false;
    bool visible = false;
    float fx = 0.f, fy = 0.f, hx = 0.f, hy = 0.f;
};
GhostProjectionCache s_draw_cache{};
constexpr std::uint64_t kProjectionCacheDurationMs = 8; // at most ~120 updates/s


std::wstring GhostPath(const std::wstring& pb_path) {
    return ghost_data_paths::PB(g_storage_dir, pb_path);
}
std::wstring FurthestGhostPath(const std::wstring& pb_path) {
    return ghost_data_paths::Furthest(g_storage_dir, pb_path);
}
std::wstring FastestGhostPath(const std::wstring& pb_path, int button) {
    return ghost_data_paths::Gold(g_storage_dir, pb_path, button);
}
// Keep legacy files available if migration was blocked by permissions or a
// lock. New recordings always go into the structured directory layout.
std::wstring ReadGhostPath(const std::wstring& modern, const std::wstring& old_global,
                           const std::wstring& old_inline) {
    if (!modern.empty() && IsRegularFile(modern)) return modern;
    if (!old_global.empty() && IsRegularFile(old_global)) return old_global;
    return !old_inline.empty() && IsRegularFile(old_inline) ? old_inline : modern;
}
std::uint32_t Milliseconds(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0 || seconds > 86400.0) return 0;
    return static_cast<std::uint32_t>(std::llround(seconds * 1000.0));
}
void LoadGhostSettingOnce() {
    if (s_loaded_settings || g_settings_path.empty()) return;
    s_loaded_settings = true;
    s_show = GetPrivateProfileIntW(L"Ghost", L"ShowPBGhost", 1,
                                    g_settings_path.c_str()) != 0;
    s_pass_through = GetPrivateProfileIntW(L"Ghost", L"PassThroughCamera", 1,
                                           g_settings_path.c_str()) != 0;
    s_transparency_2d = GetPrivateProfileIntW(L"Ghost", L"Transparent2D", 1,
                                              g_settings_path.c_str()) != 0;
    s_transparency_3d = GetPrivateProfileIntW(L"Ghost", L"Transparent3D", 1,
                                              g_settings_path.c_str()) != 0;
    const int saved_colour = GetPrivateProfileIntW(L"Ghost", L"ColourRGB",
                                                   static_cast<int>(kDefaultGhostColour),
                                                   g_settings_path.c_str());
    s_ghost_colour_rgb = saved_colour >= 0 && saved_colour <= 0xFFFFFF
        ? static_cast<uint32_t>(saved_colour) : kDefaultGhostColour;
    s_button_sync = GetPrivateProfileIntW(L"Ghost", L"StartNextGhostOnButton", 0,
                                           g_settings_path.c_str()) != 0;
    s_use_fastest_sections = GetPrivateProfileIntW(L"Ghost", L"UseFastestSections", 0,
                                                   g_settings_path.c_str()) != 0;
    s_replay_best_segments = GetPrivateProfileIntW(L"Ghost", L"ReplayBestSegments", 0,
                                                    g_settings_path.c_str()) != 0;
    // Migrate the previous boolean preference without changing its existing key.
    const int previous = GetPrivateProfileIntW(L"Ghost", L"ReplayFirstPerson", 0,
                                               g_settings_path.c_str());
    const int saved = GetPrivateProfileIntW(L"Ghost", L"ReplayCameraMode", -1,
                                            g_settings_path.c_str());
    s_replay_camera_mode = saved >= 0 && saved <= 3 ? saved : (previous ? 1 : 0);
}

bool ReadGhostFile(const std::wstring& path, std::vector<std::uint8_t>& output,
                   std::size_t extra_bytes = 0) {
    WIN32_FILE_ATTRIBUTE_DATA attrs{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attrs) ||
        attrs.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY || attrs.nFileSizeHigh ||
        attrs.nFileSizeLow < ghost_data::kHeaderSize ||
        attrs.nFileSizeLow > ghost_data::kHeaderSize +
            ghost_data::kMaxSamples * ghost_data::kSampleSize + extra_bytes) return false;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::vector<std::uint8_t> bytes(attrs.nFileSizeLow);
    DWORD received = 0;
    const bool ok = ReadFile(h, bytes.data(), static_cast<DWORD>(bytes.size()),
                             &received, nullptr) != 0 && received == bytes.size();
    CloseHandle(h);
    if (!ok) return false;
    output = std::move(bytes);
    return true;
}

bool WriteGhostFile(const std::wstring& path, const std::vector<std::uint8_t>& bytes) {
    if (path.empty() || bytes.empty() || bytes.size() > MAXDWORD) return false;
    const std::wstring temporary = path + L".tmp";
    HANDLE h = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()),
                              &written, nullptr) != 0 && written == bytes.size();
    CloseHandle(h);
    if (ok && MoveFileExW(temporary.c_str(), path.c_str(),
                          MOVEFILE_REPLACE_EXISTING)) return true;
    DeleteFileW(temporary.c_str());
    return false;
}

// Both the old incomplete-run format and the completed PB format can live
// under the same path. The discriminator is the existing file magic, so older
// recordings remain readable without a new, incompatible file format.
bool IsCompletedGhostFormat(const std::vector<std::uint8_t>& bytes) {
    static constexpr std::uint8_t magic[] = {'R','F','U','G','H','O','S','T'};
    return bytes.size() >= ghost_data::kHeaderSize &&
        std::memcmp(bytes.data(), magic, sizeof(magic)) == 0;
}
bool ValidCompletedGhostBytes(const std::vector<std::uint8_t>& bytes) {
    if (!IsCompletedGhostFormat(bytes)) return false;
    std::vector<ghost_data::Sample> samples;
    return ghost_data::Deserialize(bytes, ghost_data::Get32(bytes.data() + 12), samples);
}
void ConsolidateLegacyFurthestGhost(const std::wstring& pb_path,
                                   std::uint32_t expected_pb_ms) {
    const auto primary = GhostPath(pb_path);
    if (primary.empty()) return;
    const std::wstring sources[] = {
        FurthestGhostPath(pb_path),
        ghost_data_paths::LegacyFurthest(g_storage_dir, pb_path),
        ghost_data_paths::LegacyInline(g_storage_dir, pb_path, L".furthest.ghost")
    };
    for (const auto& source : sources) {
        if (!IsRegularFile(source) || source == primary) continue;
        std::vector<std::uint8_t> old_bytes;
        ghost_furthest_data::Run<kRefunctButtonCount> old_run;
        if (!ReadGhostFile(source, old_bytes,
                ghost_furthest_data::kPrefixSize + kRefunctButtonCount * 4) ||
            !ghost_furthest_data::Deserialize(old_bytes, old_run)) {
            Log(L"Ghost: invalid legacy furthest file preserved for manual recovery");
            continue;
        }
        if (!IsRegularFile(primary)) {
            // A PB may still exist in an older folder after a locked-file
            // migration. It must take priority over any incomplete run.
            if (IsRegularFile(ghost_data_paths::LegacyPB(g_storage_dir, pb_path)) ||
                IsRegularFile(ghost_data_paths::LegacyInline(g_storage_dir, pb_path, L".ghost"))) {
                Log(L"Ghost: legacy PB path exists; preserving furthest separately until PB migration");
                continue;
            }
            if (MoveFileW(source.c_str(), primary.c_str())) {
                Log(L"Ghost: incomplete run migrated into PB.ghost");
                continue;
            }
            Log(L"WARNING: could not merge old furthest ghost; original preserved");
            continue;
        }
        std::vector<std::uint8_t> current_bytes;
        if (!ReadGhostFile(primary, current_bytes,
                ghost_furthest_data::kPrefixSize + kRefunctButtonCount * 4)) {
            Log(L"WARNING: main ghost unreadable; old furthest ghost preserved");
            continue;
        }
        if (IsCompletedGhostFormat(current_bytes)) {
            std::vector<ghost_data::Sample> matched_pb;
            if (!ValidCompletedGhostBytes(current_bytes) || !expected_pb_ms ||
                !ghost_data::Deserialize(current_bytes, expected_pb_ms, matched_pb)) {
                Log(L"WARNING: main PB ghost invalid/stale; old furthest ghost preserved");
                continue;
            }
            // The completed PB is authoritative. Delete only a *validated*
            // redundant copy, never the only usable recording.
        } else {
            ghost_furthest_data::Run<kRefunctButtonCount> current_run;
            if (!ghost_furthest_data::Deserialize(current_bytes, current_run)) {
                Log(L"WARNING: main ghost invalid; old furthest ghost preserved");
                continue;
            }
            const auto reached = old_run.reached;
            const bool better = reached > current_run.reached ||
                (reached == current_run.reached &&
                 old_run.button_ms[reached - 1] < current_run.button_ms[reached - 1]);
            if (better && !WriteGhostFile(primary, old_bytes)) {
                Log(L"WARNING: better legacy furthest ghost could not be merged");
                continue;
            }
        }
        if (!DeleteFileW(source.c_str()))
            Log(L"WARNING: redundant legacy furthest ghost could not be removed");
    }
}

// A new gold must not synchronously open/flush a file on the game tick.
// Keep a single FIFO writer: faster successive golds for the same button
// cannot race and leave the older file on disk.
struct FastestGhostWrite {
    std::wstring path;
    std::vector<std::uint8_t> bytes;
    std::uint32_t duration = 0;
    std::vector<ghost_data::Sample> track;
    ghost_furthest_data::Run<kRefunctButtonCount> furthest;
};
bool EncodeFastestGhostWrite(FastestGhostWrite& job) {
    if (!job.bytes.empty()) return true;
    if (job.furthest.reached)
        return ghost_furthest_data::Serialize(job.furthest, job.bytes);
    return ghost_data::Serialize(job.track, job.duration, job.bytes);
}
struct FastestGhostWriter {
    SRWLOCK lock = SRWLOCK_INIT;
    HANDLE wake = nullptr; // auto-reset
    HANDLE idle = nullptr; // manual-reset
    std::deque<FastestGhostWrite> queue;
    std::atomic_bool error{false};
};
FastestGhostWriter* s_fastest_writer = nullptr;
DWORD WINAPI FastestGhostWriterMain(LPVOID parameter) {
    auto* writer = static_cast<FastestGhostWriter*>(parameter);
    for (;;) {
        if (WaitForSingleObject(writer->wake, INFINITE) != WAIT_OBJECT_0) return 1;
        for (;;) {
            AcquireSRWLockExclusive(&writer->lock);
            if (writer->queue.empty()) {
                SetEvent(writer->idle);
                ReleaseSRWLockExclusive(&writer->lock);
                break;
            }
            FastestGhostWrite job = std::move(writer->queue.front());
            writer->queue.pop_front();
            ReleaseSRWLockExclusive(&writer->lock);
            // Encode and checksum the immutable snapshot off the game thread.
            // Keep all writes to the same path in the original FIFO order.
            bool encoded = false;
            try { encoded = EncodeFastestGhostWrite(job); } catch (...) {}
            if (!encoded) {
                writer->error.store(true, std::memory_order_release);
                Log(L"ERROR: background ghost encoding failed");
                continue;
            }
            // The gold writer is the only path that may create a new per-profile
            // folder after a snapshot is cloned. Never create it on a 20-Hz tick.
            const auto slash = job.path.find_last_of(L"\\/");
            if (slash != std::wstring::npos)
                EnsureDirectory(job.path.substr(0, slash));
            bool saved = false;
            for (int attempt = 0; attempt < 3 && !saved; ++attempt) {
                if (attempt) Sleep(100); // worker only
                saved = WriteGhostFile(job.path, job.bytes);
            }
            if (!saved) {
                writer->error.store(true, std::memory_order_release);
                Log(L"ERROR: failed to save fastest-segment ghost recording");
            }
        }
    }
}
void StartFastestGhostWriter() {
    if (s_fastest_writer) return;
    auto* writer = new (std::nothrow) FastestGhostWriter;
    if (!writer) return;
    writer->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    writer->idle = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    if (!writer->wake || !writer->idle) {
        if (writer->wake) CloseHandle(writer->wake);
        if (writer->idle) CloseHandle(writer->idle);
        delete writer;
        return;
    }
    HANDLE thread = CreateThread(nullptr, 0, FastestGhostWriterMain, writer, 0, nullptr);
    if (!thread) {
        CloseHandle(writer->wake);
        CloseHandle(writer->idle);
        delete writer;
        return;
    }
    CloseHandle(thread); // Worker persists for the ASI process lifetime.
    s_fastest_writer = writer;
}
bool QueueFastestGhostJob(FastestGhostWrite job) {
    if (job.path.empty() || (job.bytes.empty() && job.track.empty() && !job.furthest.reached)) return false;
    // If native threading is unavailable, correctness takes priority: save
    // immediately rather than silently losing a legitimate new record.
    if (!s_fastest_writer) return EncodeFastestGhostWrite(job) && WriteGhostFile(job.path, job.bytes);
    auto* writer = s_fastest_writer;
    AcquireSRWLockExclusive(&writer->lock);
    if (writer->queue.size() >= 128) {
        ReleaseSRWLockExclusive(&writer->lock);
        writer->error.store(true, std::memory_order_release);
        return false;
    }
    try {
        writer->queue.push_back(std::move(job));
    } catch (...) {
        ReleaseSRWLockExclusive(&writer->lock);
        writer->error.store(true, std::memory_order_release);
        return false;
    }
    ResetEvent(writer->idle);
    const bool signalled = SetEvent(writer->wake) != 0;
    if (!signalled) {
        writer->queue.pop_back();
        if (writer->queue.empty()) SetEvent(writer->idle);
        writer->error.store(true, std::memory_order_release);
    }
    ReleaseSRWLockExclusive(&writer->lock);
    return signalled;
}
// Reflected controller projection: no guessed offsets or custom camera matrix.
// This renders a 2D hologram at the ghost's 3D world position, not a new actor.
struct ProjectionApi {
    void* controller_class = nullptr;
    void* method = nullptr;
    int world = -1, screen = -1, result = -1;
};
ProjectionApi s_projection{};

int FieldOffset(void* function, const wchar_t* name, const wchar_t* expected_type,
                std::size_t size) {
    if (!IsReadable(function, sizeof(UStructHeader))) return -1;
    void* field = static_cast<const UStructHeader*>(function)->children;
    for (int n = 0; n < 256 && IsReadable(field, sizeof(UFieldHeader)); ++n) {
        auto* info = static_cast<const UFieldHeader*>(field);
        if (UnrealName(info->object.name) == name) {
            if (!IsReadable(info->object.class_ptr, sizeof(UObjectHeader)) ||
                UnrealName(static_cast<const UObjectHeader*>(info->object.class_ptr)->name) != expected_type)
                return -1;
            const int off = FindClassPropertyOffset(function, name);
            return off >= 0 && off <= 256 - static_cast<int>(size) ? off : -1;
        }
        field = info->next;
    }
    return -1;
}

bool ProjectPosition(PlayerController* controller, const Vec3& world,
                     int32_t width, int32_t height, float& x, float& y) {
    // DrawPBGhost validates the controller once before calling this twice.
    if (!g_process_event || !controller) return false;
    void* cls = reinterpret_cast<const UObjectHeader*>(controller)->class_ptr;
    if (s_projection.controller_class != cls) {
        s_projection = {};
        s_projection.controller_class = cls;
        s_projection.method = FindReflectedFunction(controller, L"ProjectWorldLocationToScreen");
        if (s_projection.method) {
            s_projection.world = FieldOffset(s_projection.method, L"WorldLocation", L"StructProperty", sizeof(Vec3));
            s_projection.screen = FieldOffset(s_projection.method, L"ScreenLocation", L"StructProperty", 2*sizeof(float));
            s_projection.result = FieldOffset(s_projection.method, L"ReturnValue", L"BoolProperty", 1);
        }
        if (!s_projection.method || s_projection.world < 0 ||
            s_projection.screen < 0 || s_projection.result < 0) {
            Log(L"PB ghost: native ProjectWorldLocationToScreen reflection unavailable; hologram disabled");
            s_projection.method = nullptr;
        }
    }
    // FieldOffset validated the persistent class UFunction at resolution time.
    if (!s_projection.method) return false;
    alignas(8) std::array<std::uint8_t, 256> params{};
    std::memcpy(params.data() + s_projection.world, &world, sizeof(world));
    // bPlayerViewportRelative defaults false (all bytes initially zero).
    g_process_event(controller, s_projection.method, params.data());
    if (!params[static_cast<size_t>(s_projection.result)]) return false;
    std::memcpy(&x, params.data() + s_projection.screen, sizeof(float));
    std::memcpy(&y, params.data() + s_projection.screen + sizeof(float), sizeof(float));
    return std::isfinite(x) && std::isfinite(y) &&
           x >= -200.f && y >= -200.f &&
           x <= static_cast<float>(width) + 200.f &&
           y <= static_cast<float>(height) + 200.f;
}

// Runtime-only ghost model. Six independent static-mesh actors create a real
// 3D figure that depth-tests against the scene, unlike the former HUD overlay.
// They have collision disabled at both actor and component levels. The engine
// manages their rendering. Normal ghosts never touch the camera; the optional
// replay viewer uses its own temporary CameraActor and restores the view.
// This works in Shipping builds where DrawDebugBox is compiled out.
struct alignas(16) GhostUETransform {
    float quat[4] = {0.f, 0.f, 0.f, 1.f};
    float translation[4] = {0.f, 0.f, 0.f, 0.f};
    float scale[4] = {1.f, 1.f, 1.f, 0.f};
};
static_assert(sizeof(GhostUETransform) == 48);

struct WorldGhostApi {
    void* gameplay = nullptr;                 // Default__GameplayStatics CDO
    void* actor_class = nullptr;              // Native StaticMeshActor UClass
    void* block_mesh = nullptr;               // SM_Block_1000x1000x20000 asset
    void* begin = nullptr, *finish = nullptr;
    int begin_world = -1, begin_class = -1, begin_transform = -1;
    int begin_collision = -1, begin_owner = -1, begin_result = -1;
    int finish_actor = -1, finish_transform = -1;
    int component_offset = -1;
    int32_t scan_cursor = 0;
    std::uint64_t retry_after = 0;
    bool reported = false;
};
WorldGhostApi s_world_api{};

struct WorldGhostActor {
    void* actor = nullptr;
    int32_t index = -1, serial = 0;
    Vec3 scale{};
};
struct WorldGhostInstances {
    MyCharacter* player = nullptr;
    std::array<WorldGhostActor, 6> parts{};
    Vec3 original_extent{}, original_center_offset{};
    bool bounds_ready = false;
    int count = 0;
};
// True only when the just-committed PB has a matching, newly written ghost.
// Cleared on a new run/profile and before each PB ghost save attempt.
bool s_pb_ghost_saved_for_latest_pb = false;
WorldGhostInstances s_world_parts{};
bool s_world_parts_ready = false;
bool s_world_parts_hidden = true;
// Native transform dispatch is unnecessary when a paused/button-synced
// ghost has not moved. Reset on actor destruction or world change.
bool s_world_pose_valid = false;
ghost_data::Sample s_world_pose{};

// Refunct's UE4.14 cooked assets include M_Glass_Inst (translucent) and
// M_Basic (BLEND_Opaque, vector parameter Color). The glass shader is still
// translucent at a scalar opacity of 1, so opacity OFF must use M_Basic.
// Only resolve these two exact known assets, never inspect UMaterial internals
// or modify shared material instances belonging to the game world.
struct GhostGlassState {
    void* material = nullptr;         // M_Glass_Inst: transparent only
    void* opaque_material = nullptr;  // M_Basic: genuine solid-colour shader
    void* string_library = nullptr;
    int32_t scan_cursor = 0;
    int applied_parts = 0;
    std::uint64_t retry_after = 0;
    bool warned_missing = false;
    bool warned_unavailable = false;
    bool failed = false;
    bool reported_applied = false;
    bool names_ready = false;
    bool scan_complete = false;
    // Material parameter names are global FNames, not cooked-package name-map
    // indices. A reflected name conversion can be absent/incompatible in UE4.14
    // Shipping; resolve existing registered FNames from validated UObject names.
    int32_t parameter_scan_cursor = 0;
    bool parameter_scan_complete = false;
    bool reported_parameter_names = false;
    FNameView opacity_name{}, color_name{}, color2_name{};
};
GhostGlassState s_ghost_glass{};

// Spectator playback follows the saved movement and issues the SAME native
// button contacts used by section practice at the recorded split timestamps.
// It must begin from button 0: Refunct cannot safely undo native progression.
// Replay resets the level at start and after a replay-driven native press.
// Existing recordings are read-only. No new PB/gold times may be saved.
struct GhostReplayViewer {
    struct FreeCamera {
        bool initialized = false;
        bool controller_sample_valid = false;
        Vec3 position{};
        Vec3 velocity{}; // Smoothed camera velocity; never modifies player physics.
        Rotator rotation{};
        Rotator target_rotation{}; // Smooth mouse and arrow look separately from input sampling.
        Rotator last_controller_rotation{};
        std::uint64_t last_input_tick = 0;
    } free_camera{};
    // Orbit follows the recorded ghost but uses the spectator's own viewing
    // direction. Camera height and distance are calculated every frame from
    // the ghost pose, without touching the real player or replay clock.
    struct OrbitCamera {
        bool initialized = false;
        bool controller_sample_valid = false;
        Rotator last_controller_rotation{};
        float yaw_offset = 0.f;
        float target_yaw_offset = 0.f;
        float pitch = -18.f;
        float target_pitch = -18.f;
        float distance = 325.f;
        float target_distance = 325.f;
        std::uint64_t last_input_tick = 0;
    } orbit_camera{};
    bool requested = false, active = false, paused = false, fastest = false;
    int speed_index = 1;                 // 0.5x, 1x, 2x
    double elapsed_ms = 0.0;
    std::uint32_t duration_ms = 0;
    std::uint64_t last_tick = 0;
    ghost_replay_progress::Timeline<kRefunctButtonCount> button_timeline{};
    int next_button = 0;
    // Split order is chronological; physical actor order can differ within a section.
    std::array<bool, kRefunctButtonCount> pressed_buttons{};
    std::uint64_t next_button_discovery_at = 0;
    bool button_cache_ready = false;
    bool awaiting_press = false;
    std::uint64_t press_sent_at = 0;
    bool changed_native_progress = false;
    bool marked_assisted = false;
    bool waiting_reset = false;
    bool owns_pending_restart = false;
    int resets_before_restart = -1;
    MyCharacter* player_before_restart = nullptr;
    std::uint64_t reset_requested_at = 0;
    double resume_at_ms = 0.0;
    MyCharacter* player = nullptr;
    void* controller = nullptr;
    void* previous_target = nullptr;
    void* camera_class = nullptr;
    int32_t class_scan_cursor = 0;
    WorldGhostActor camera{};
    void* move_fn = nullptr;
    int move_location = -1, move_rotation = -1, move_teleport = -1;
    void* view_fn = nullptr, *get_view_fn = nullptr;
    int view_target = -1, view_blend = -1, get_view_result = -1;
};
GhostReplayViewer s_replay{};
constexpr std::array<double, 3> kReplaySpeeds{{0.5, 1.0, 2.0}};

const GlobalObjectArray* GhostObjects() {
    if (!g_game_module) return nullptr;
    const auto* result = reinterpret_cast<const GlobalObjectArray*>(
        reinterpret_cast<uintptr_t>(g_game_module) + kGlobalObjectArrayRva);
    if (!IsReadable(result, sizeof(GlobalObjectArray)) || !result->objects ||
        result->num_elements <= 0 || result->num_elements > 1000000 ||
        result->max_elements < result->num_elements) return nullptr;
    return result;
}

bool GhostLive(const GlobalObjectArray* objects, const WorldGhostActor& part) {
    return objects && part.actor && part.index >= 0 &&
        part.index < objects->num_elements &&
        IsReadable(objects->objects + part.index, sizeof(GlobalObjectItem)) &&
        objects->objects[part.index].object == part.actor &&
        objects->objects[part.index].serial_number == part.serial &&
        !(static_cast<uint32_t>(objects->objects[part.index].flags) & 0x30000000u) &&
        IsReadable(part.actor, sizeof(UObjectHeader)) &&
        static_cast<const UObjectHeader*>(part.actor)->class_ptr == s_world_api.actor_class;
}

// An already-destroyed level invalidates its UObject indices. Never call
// ProcessEvent on such pointers, including during a map reload.
void DestroyWorldGhostActors() {
    const auto* objects = GhostObjects();
    for (auto& part : s_world_parts.parts) {
        if (!GhostLive(objects, part) || !g_process_event) continue;
        if (void* destroy = FindReflectedFunction(part.actor, L"K2_DestroyActor")) {
            alignas(16) std::array<uint8_t, 64> args{};
            g_process_event(part.actor, destroy, args.data());
        }
    }
    s_world_parts = {};
    s_world_pose_valid = false;
    s_world_parts_ready = false;
    s_world_parts_hidden = true;
    s_ghost_glass.applied_parts = 0;
    s_ghost_glass.failed = false;
    s_ghost_glass.reported_applied = false;
}

bool GhostClassName(void* object, const wchar_t* class_name) {
    if (!IsReadable(object, sizeof(UObjectHeader))) return false;
    void* cls = static_cast<const UObjectHeader*>(object)->class_ptr;
    for (int i = 0; i < 12 && IsReadable(cls, sizeof(UStructHeader)); ++i) {
        if (UnrealName(static_cast<const UObjectHeader*>(cls)->name) == class_name)
            return true;
        cls = static_cast<const UStructHeader*>(cls)->super_struct;
    }
    return false;
}

bool GhostParam(void* function, const wchar_t* name,
                const wchar_t* kind, size_t length, int& offset,
                size_t capacity = 512) {
    if (!IsReadable(function, sizeof(UStructHeader))) return false;
    void* field = static_cast<const UStructHeader*>(function)->children;
    for (int i = 0; i < 96 && IsReadable(field, sizeof(UFieldHeader)); ++i) {
        const auto* data = static_cast<const UFieldHeader*>(field);
        if (UnrealName(data->object.name) == name) {
            if (!IsReadable(data->object.class_ptr, sizeof(UObjectHeader)) ||
                UnrealName(static_cast<const UObjectHeader*>(data->object.class_ptr)->name) != kind)
                return false;
            const int result = FindClassPropertyOffset(function, name);
            if (result < 0 || static_cast<size_t>(result) + length > capacity)
                return false;
            offset = result;
            return true;
        }
        field = data->next;
    }
    return false;
}

// These signatures are UE4 Blueprint UFunctions; their offsets are resolved
// from the running executable, never hard-coded. Reject incompatible layouts.
bool ResolveWorldGhostSpawnFunctions() {
    auto& api = s_world_api;
    if (!api.gameplay || !api.actor_class || !api.block_mesh) return false;
    api.begin = FindReflectedFunction(api.gameplay, L"BeginDeferredActorSpawnFromClass");
    api.finish = FindReflectedFunction(api.gameplay, L"FinishSpawningActor");
    if (!api.begin || !api.finish) return false;
    if (!GhostParam(api.begin, L"WorldContextObject", L"ObjectProperty", sizeof(void*), api.begin_world) ||
        !GhostParam(api.begin, L"ActorClass", L"ClassProperty", sizeof(void*), api.begin_class) ||
        !GhostParam(api.begin, L"SpawnTransform", L"StructProperty", sizeof(GhostUETransform), api.begin_transform) ||
        !(GhostParam(api.begin, L"CollisionHandlingOverride", L"EnumProperty", 1, api.begin_collision) ||
          GhostParam(api.begin, L"CollisionHandlingOverride", L"ByteProperty", 1, api.begin_collision)) ||
        !GhostParam(api.begin, L"Owner", L"ObjectProperty", sizeof(void*), api.begin_owner) ||
        !GhostParam(api.begin, L"ReturnValue", L"ObjectProperty", sizeof(void*), api.begin_result) ||
        !GhostParam(api.finish, L"Actor", L"ObjectProperty", sizeof(void*), api.finish_actor) ||
        !GhostParam(api.finish, L"SpawnTransform", L"StructProperty", sizeof(GhostUETransform), api.finish_transform))
        return false;
    // A 40-byte transform would overlap the next parameter. Never write a
    // guessed 48-byte structure into an incompatible signature.
    if (api.begin_collision < api.begin_transform + static_cast<int>(sizeof(GhostUETransform)))
        return false;
    api.component_offset = FindClassPropertyOffset(api.actor_class, L"StaticMeshComponent");
    return api.component_offset >= 0 && api.component_offset <= 0x2000;
}

bool GhostResourceLive(const GlobalObjectArray* objects, const void* pointer) {
    if (!objects || !IsReadable(pointer, sizeof(UObjectHeader))) return false;
    const auto* header = static_cast<const UObjectHeader*>(pointer);
    if (header->index < 0 || header->index >= objects->num_elements) return false;
    const auto* slot = objects->objects + header->index;
    return IsReadable(slot, sizeof(GlobalObjectItem)) && slot->object == pointer &&
        !(static_cast<uint32_t>(slot->flags) & 0x30000000u);
}

bool ResolveWorldGhostAssets() {
    auto& api = s_world_api;
    const auto* objects = GhostObjects();
    if (!objects) return false;
    if (api.begin && api.finish &&
        GhostResourceLive(objects, api.gameplay) &&
        GhostResourceLive(objects, api.actor_class) &&
        GhostResourceLive(objects, api.block_mesh)) return true;
    if (api.begin || api.finish) {
        DestroyWorldGhostActors();
        api = {}; // Cached asset may have been GC'd by a level reload.
    }
    const auto now = static_cast<std::uint64_t>(GetTickCount64());
    if (now < api.retry_after) return false;
    constexpr int32_t kBatch = 384;
    const int32_t end = (std::min)(api.scan_cursor + kBatch, objects->num_elements);
    const std::uint64_t begin_ms = GetTickCount64();
    for (; api.scan_cursor < end; ++api.scan_cursor) {
        if (GetTickCount64() - begin_ms >= 1) break;
        const auto* item = objects->objects + api.scan_cursor;
        if (!IsReadable(item, sizeof(GlobalObjectItem)) || !item->object ||
            (static_cast<uint32_t>(item->flags) & 0x30000000u) ||
            !IsReadable(item->object, sizeof(UObjectHeader))) continue;
        const auto* header = item->object;
        const auto name = UnrealName(header->name);
        if (!api.gameplay && name == L"Default__GameplayStatics" &&
            GhostClassName(item->object, L"GameplayStatics"))
            api.gameplay = item->object;
        if (!api.actor_class && name == L"StaticMeshActor" &&
            GhostClassName(item->object, L"Class"))
            api.actor_class = item->object;
        if (!api.block_mesh && name == L"SM_Block_1000x1000x20000" &&
            GhostClassName(item->object, L"StaticMesh"))
            api.block_mesh = item->object;
        if (api.gameplay && api.actor_class && api.block_mesh) break;
    }
    if (api.gameplay && api.actor_class && api.block_mesh) {
        if (ResolveWorldGhostSpawnFunctions()) return true;
        if (!api.reported) {
            Log(L"Ghost 3D: native spawn functions or parameters unavailable; preserving old ghost recordings");
            api.reported = true;
        }
        api.begin = api.finish = nullptr;
        api.retry_after = now + 5000;
        return false;
    }
    if (api.scan_cursor >= objects->num_elements) {
        api.scan_cursor = 0;
        api.retry_after = now + 5000;
        if (!api.reported) {
            Log(L"Ghost 3D: waiting for GameplayStatics, StaticMeshActor, and world block mesh");
            api.reported = true;
        }
    }
    return false;
}

// Resolve and invoke only verified existing methods on the newly spawned
// actor/component. All setters use zero sweep; ghost cannot push the player.
// UE4's SceneComponent::SetMobility is a native C++ method and is not
// necessarily reflected as a UFunction in Refunct's Shipping build. We own
// these *newly spawned deferred* components, so set their verified Mobility
// UProperty before FinishSpawningActor registers/construction-finalizes them.
// Never use this on an existing level component: changing Mobility directly
// after registration bypasses the renderer's re-registration path.
bool GhostPrepareDeferredMobility(void* component) {
    if (!IsReadable(component, sizeof(UObjectHeader))) return false;
    void* klass = static_cast<const UObjectHeader*>(component)->class_ptr;
    for (int depth = 0; depth < 16 && IsReadable(klass, sizeof(UStructHeader)); ++depth) {
        const auto* cls = static_cast<const UStructHeader*>(klass);
        void* field = cls->children;
        for (int n = 0; n < 4096 && IsReadable(field, sizeof(UFieldHeader)); ++n) {
            const auto* property = static_cast<const UFieldHeader*>(field);
            if (UnrealName(property->object.name) == L"Mobility") {
                if (!IsReadable(property->object.class_ptr, sizeof(UObjectHeader)))
                    return false;
                const std::wstring type = UnrealName(
                    static_cast<const UObjectHeader*>(property->object.class_ptr)->name);
                if (type != L"ByteProperty" && type != L"EnumProperty") return false;
                const int offset = FindClassPropertyOffset(klass, L"Mobility");
                if (offset < static_cast<int>(sizeof(UObjectHeader)) || offset > 0x1000)
                    return false;
                auto* mobility = static_cast<uint8_t*>(component) + offset;
                if (!IsReadable(mobility, 1) || *mobility > 2) return false;
                *mobility = 2; // EComponentMobility::Movable, deferred actor only.
                return *mobility == 2;
            }
            field = property->next;
        }
        klass = cls->super_struct;
    }
    return false;
}

bool GhostDeferredMobilityIsMovable(void* component) {
    if (!IsReadable(component, sizeof(UObjectHeader))) return false;
    void* klass = static_cast<const UObjectHeader*>(component)->class_ptr;
    const int offset = FindClassPropertyOffset(klass, L"Mobility");
    return offset >= static_cast<int>(sizeof(UObjectHeader)) && offset <= 0x1000 &&
        IsReadable(static_cast<const uint8_t*>(component) + offset, 1) &&
        *(static_cast<const uint8_t*>(component) + offset) == 2;
}

bool GhostCallBool(void* object, const wchar_t* method,
                   const wchar_t* field, uint8_t value) {
    if (!g_process_event || !IsReadable(object, sizeof(UObjectHeader))) return false;
    void* fn = FindReflectedFunction(object, method);
    int offset = -1;
    if (!fn || !GhostParam(fn, field, L"BoolProperty", 1, offset)) return false;
    alignas(16) std::array<uint8_t, 512> args{};
    args[static_cast<size_t>(offset)] = value;
    g_process_event(object, fn, args.data());
    return true;
}

bool GhostCallByte(void* object, const wchar_t* method,
                   const wchar_t* field, uint8_t value) {
    if (!g_process_event || !IsReadable(object, sizeof(UObjectHeader))) return false;
    void* fn = FindReflectedFunction(object, method);
    int offset = -1;
    if (!fn || !(GhostParam(fn, field, L"EnumProperty", 1, offset) ||
                 GhostParam(fn, field, L"ByteProperty", 1, offset))) return false;
    alignas(16) std::array<uint8_t, 512> args{};
    args[static_cast<size_t>(offset)] = value;
    g_process_event(object, fn, args.data());
    return true;
}

bool GhostCallVector(void* object, const wchar_t* method,
                     const wchar_t* field, const Vec3& value) {
    if (!g_process_event || !IsReadable(object, sizeof(UObjectHeader))) return false;
    void* fn = FindReflectedFunction(object, method);
    int offset = -1;
    if (!fn || !GhostParam(fn, field, L"StructProperty", sizeof(Vec3), offset)) return false;
    alignas(16) std::array<uint8_t, 512> args{};
    std::memcpy(args.data() + offset, &value, sizeof(value));
    g_process_event(object, fn, args.data());
    return true;
}

// Resolve at most 256 registry entries per tick, using the same UObject
// liveness checks used for the working ghost mesh. Unlike the removed
// material-audit code, this never reads UMaterial parameter arrays or
// guesses a blend-mode/property layout in a Shipping process.
// Select only known compiled Refunct assets, never arbitrary UMaterials.
// Batch lookup and UObject liveness checks are shared by both modes.
void* FindLoadedGhostAppearanceMaterial() {
    auto& state = s_ghost_glass;
    const auto* objects = GhostObjects();
    if (!objects) return nullptr;
    if (state.material &&
        (!GhostResourceLive(objects, state.material) ||
         !GhostClassName(state.material, L"MaterialInstanceConstant") ||
         UnrealName(static_cast<const UObjectHeader*>(state.material)->name) != L"M_Glass_Inst")) {
        state.material = nullptr;
        state.scan_cursor = 0;
        state.scan_complete = false;
    }
    if (state.opaque_material &&
        (!GhostResourceLive(objects, state.opaque_material) ||
         !GhostClassName(state.opaque_material, L"Material") ||
         UnrealName(static_cast<const UObjectHeader*>(state.opaque_material)->name) != L"M_Basic")) {
        state.opaque_material = nullptr;
        state.scan_cursor = 0;
        state.scan_complete = false;
    }
    if (state.string_library && !GhostResourceLive(objects, state.string_library)) {
        state.string_library = nullptr;
        state.names_ready = false;
        state.scan_cursor = 0;
        state.scan_complete = false;
    }
    const auto selected = [&]() -> void* {
        return s_transparency_3d ? state.material : state.opaque_material;
    };
    // A valid source material can still replace the block's developer texture
    // if FName conversion is unavailable; the cyan MID is an optional upgrade.
    if (selected() && (state.string_library || state.scan_complete)) return selected();
    const auto now = static_cast<std::uint64_t>(GetTickCount64());
    if (now < state.retry_after) return nullptr;
    const int32_t end = (std::min)(objects->num_elements, state.scan_cursor + 256);
    const auto start = GetTickCount64();
    for (; state.scan_cursor < end; ++state.scan_cursor) {
        if (GetTickCount64() - start >= 1) break;
        const auto* slot = objects->objects + state.scan_cursor;
        if (!IsReadable(slot, sizeof(GlobalObjectItem)) || !slot->object ||
            (static_cast<uint32_t>(slot->flags) & 0x30000000u) ||
            !IsReadable(slot->object, sizeof(UObjectHeader))) continue;
        const auto* header = slot->object;
        const auto name = UnrealName(header->name);
        if (!state.material && name == L"M_Glass_Inst" &&
            GhostClassName(slot->object, L"MaterialInstanceConstant") &&
            GhostResourceLive(objects, slot->object)) {
            state.material = slot->object;
            Log(L"Ghost 3D: located Refunct's compiled translucent M_Glass_Inst");
        }
        if (!state.opaque_material && name == L"M_Basic" &&
            GhostClassName(slot->object, L"Material") &&
            GhostResourceLive(objects, slot->object)) {
            state.opaque_material = slot->object;
            Log(L"Ghost 3D: located Refunct's compiled opaque M_Basic material");
        }
        if (!state.string_library && name == L"Default__KismetStringLibrary" &&
            GhostClassName(slot->object, L"KismetStringLibrary") &&
            GhostResourceLive(objects, slot->object))
            state.string_library = slot->object;
        if (selected() && state.string_library) break;
    }
    if (state.scan_cursor >= objects->num_elements) {
        state.scan_complete = true;
        state.scan_cursor = 0;
        state.retry_after = now + 30000;
        if (!selected() && !state.warned_missing) {
            Log(s_transparency_3d
                ? L"Ghost 3D: M_Glass_Inst not loaded; retaining original ghost material"
                : L"Ghost 3D: M_Basic not loaded; retaining opaque original ghost material");
            state.warned_missing = true;
        }
    }
    return selected() && (state.string_library || state.scan_complete)
        ? selected() : nullptr;
}

bool GhostParameterNameEquals(const std::wstring& actual, const wchar_t* expected) {
    if (!expected || actual.size() != std::wcslen(expected)) return false;
    for (size_t i = 0; i < actual.size(); ++i)
        if (std::towlower(actual[i]) != std::towlower(expected[i])) return false;
    return true;
}

// UE4.14 provides a reflected string-to-FName conversion. Use it to create
// parameter names without relying on private FNamePool offsets or reading
// serialized material instance parameter arrays (which previously crashed).
bool GhostGlassFName(void* library, const wchar_t* literal, FNameView& result) {
    if (!g_process_event || !GhostClassName(library, L"KismetStringLibrary")) return false;
    void* fn = FindReflectedFunction(library, L"Conv_StringToName");
    int input = -1, output = -1;
    if (!fn || !GhostParam(fn, L"InString", L"StrProperty", sizeof(FStringView), input) ||
        !GhostParam(fn, L"ReturnValue", L"NameProperty", sizeof(FNameView), output) ||
        (input < output + static_cast<int>(sizeof(FNameView)) &&
         output < input + static_cast<int>(sizeof(FStringView))))
        return false;
    const size_t n = std::wcslen(literal);
    if (!n || n >= 31) return false;
    std::array<wchar_t,32> characters{};
    std::wmemcpy(characters.data(), literal, n);
    FStringView str{characters.data(), static_cast<int32_t>(n+1), static_cast<int32_t>(n+1)};
    alignas(16) std::array<uint8_t,512> args{};
    std::memcpy(args.data()+input, &str, sizeof(str));
    g_process_event(library, fn, args.data());
    FNameView candidate{};
    std::memcpy(&candidate, args.data()+output, sizeof(candidate));
    // FName is case-insensitive. Its canonical spelling is not necessarily
    // identical to the spelling in a cooked material's local name table.
    const std::wstring actual = UnrealName(candidate);
    if (!candidate.index || !GhostParameterNameEquals(actual, literal))
        return false;
    result = candidate; // Never poison a cached parameter with a failed conversion.
    return true;
}

// An exact-name FName already present in any reflected UObject is a valid
// global name for a material parameter of that spelling. Scanning the existing
// object registry is safer than guessing native material parameter-array
// offsets (which previously caused an access violation) or inventing FName IDs.
// Never dereference material internals, and spread the scan across ticks.
void GhostDiscoverRegisteredParameterNames() {
    auto& state = s_ghost_glass;
    if (state.parameter_scan_complete ||
        (state.color_name.index && state.opacity_name.index &&
         state.color2_name.index)) return;
    const auto* objects = GhostObjects();
    if (!objects) return;
    const int32_t end = (std::min)(objects->num_elements,
                                   state.parameter_scan_cursor + 384);
    const auto started = GetTickCount64();
    for (; state.parameter_scan_cursor < end; ++state.parameter_scan_cursor) {
        if (GetTickCount64() - started >= 1) break;
        const auto* slot = objects->objects + state.parameter_scan_cursor;
        if (!IsReadable(slot, sizeof(GlobalObjectItem)) || !slot->object ||
            (static_cast<uint32_t>(slot->flags) & 0x30000000u) ||
            !IsReadable(slot->object, sizeof(UObjectHeader))) continue;
        const FNameView name = slot->object->name;
        if (!name.index) continue;
        // Resolve only unclaimed names, so no extra allocations are needed
        // once all three names have been found.
        const std::wstring actual = UnrealName(name);
        if (!state.color_name.index && GhostParameterNameEquals(actual, L"Color"))
            state.color_name = name;
        else if (!state.opacity_name.index &&
                 GhostParameterNameEquals(actual, L"Opacity"))
            state.opacity_name = name;
        else if (!state.color2_name.index &&
                 GhostParameterNameEquals(actual, L"Color2"))
            state.color2_name = name;
        if (state.color_name.index && state.opacity_name.index &&
            state.color2_name.index) break;
    }
    state.parameter_scan_complete =
        state.parameter_scan_cursor >= objects->num_elements ||
        (state.color_name.index && state.opacity_name.index &&
         state.color2_name.index);
}

bool ResolveGhostGlassParameterNames() {
    auto& state = s_ghost_glass;
    if (state.names_ready) return true;
    // The standard reflected converter is preferred. Unlike the previous
    // implementation, a missing optional Color2 or Opacity name does not
    // prevent the actual Color parameter from being applied.
    if (state.string_library) {
        if (!state.color_name.index)
            GhostGlassFName(state.string_library, L"Color", state.color_name);
        if (s_transparency_3d) {
            if (!state.opacity_name.index)
                GhostGlassFName(state.string_library, L"Opacity", state.opacity_name);
            if (!state.color2_name.index)
                GhostGlassFName(state.string_library, L"Color2", state.color2_name);
        }
    }
    if (!state.color_name.index ||
        (s_transparency_3d && (!state.opacity_name.index ||
                              !state.color2_name.index)))
        GhostDiscoverRegisteredParameterNames();
    // Wait for the one-time bounded scan to complete before settling for a
    // partial set of names, so an absent optional Color2 cannot block Color.
    if (state.string_library && state.color_name.index &&
        (!s_transparency_3d ||
         (state.opacity_name.index && state.color2_name.index))) {
        state.names_ready = true;
    } else if (state.parameter_scan_complete) {
        state.names_ready = state.color_name.index != 0;
    }
    if (!state.reported_parameter_names && state.parameter_scan_complete) {
        Log((std::wstring(L"Ghost 3D: native parameter names: Color=") +
             (state.color_name.index ? L"yes" : L"no") +
             L", Opacity=" + (state.opacity_name.index ? L"yes" : L"no") +
             L", Color2=" + (state.color2_name.index ? L"yes" : L"no")).c_str());
        state.reported_parameter_names = true;
    }
    return state.names_ready;
}

bool GhostGlassGetMaterial(void* mesh, void*& result) {
    if (!g_process_event || !GhostClassName(mesh,L"StaticMeshComponent")) return false;
    void* fn = FindReflectedFunction(mesh, L"GetMaterial");
    int index=-1, returned=-1;
    if (!fn || !GhostParam(fn,L"ElementIndex",L"IntProperty",sizeof(int32_t),index) ||
        !GhostParam(fn,L"ReturnValue",L"ObjectProperty",sizeof(void*),returned)) return false;
    alignas(16) std::array<uint8_t,512> args{};
    g_process_event(mesh,fn,args.data()); // ElementIndex = 0
    std::memcpy(&result,args.data()+returned,sizeof(result));
    return true;
}

bool GhostGlassSetMaterial(void* mesh, void* material) {
    if (!g_process_event || !GhostClassName(mesh,L"StaticMeshComponent")) return false;
    void* fn = FindReflectedFunction(mesh,L"SetMaterial");
    int index=-1, value=-1;
    if (!fn || !GhostParam(fn,L"ElementIndex",L"IntProperty",sizeof(int32_t),index) ||
        !GhostParam(fn,L"Material",L"ObjectProperty",sizeof(void*),value)) return false;
    alignas(16) std::array<uint8_t,512> args{};
    std::memcpy(args.data()+value,&material,sizeof(material));
    g_process_event(mesh,fn,args.data());
    void* installed=nullptr;
    return GhostGlassGetMaterial(mesh,installed) && installed==material;
}

// UE4.14 uses several different reflected MID factory signatures. Refunct's
// tile customisation already needs the legacy one-argument factory: the two
// source-material factories are NOT sufficient for every Shipping build.
// Never mutate the shared M_Basic or M_Glass_Inst; all setters target only
// the newly created MID owned by this ghost mesh.
void* GhostGlassCreateMID(void* mesh, void* source) {
    if (!g_process_event || !GhostClassName(mesh, L"StaticMeshComponent") ||
        !GhostResourceLive(GhostObjects(), source)) return nullptr;
    void* previous = nullptr;
    if (!GhostGlassGetMaterial(mesh, previous)) return nullptr;
    constexpr struct {const wchar_t* method; const wchar_t* parent;} factories[]{
        {L"CreateDynamicMaterialInstance", L"SourceMaterial"},
        {L"CreateAndSetMaterialInstanceDynamicFromMaterial", L"Parent"},
        {L"CreateAndSetMaterialInstanceDynamic", nullptr}
    };
    for (const auto& factory : factories) {
        // Legacy UE4::CreateAndSetMaterialInstanceDynamic derives its parent
        // from the material already installed in this slot.
        if (!factory.parent && !GhostGlassSetMaterial(mesh, source)) continue;
        void* fn = FindReflectedFunction(mesh, factory.method);
        int index = -1, parent = -1, returned = -1;
        if (!fn || !GhostParam(fn, L"ElementIndex", L"IntProperty", sizeof(int32_t), index) ||
            !GhostParam(fn, L"ReturnValue", L"ObjectProperty", sizeof(void*), returned) ||
            (factory.parent && !GhostParam(fn, factory.parent, L"ObjectProperty",
                                           sizeof(void*), parent))) continue;
        alignas(16) std::array<uint8_t, 512> args{};
        if (factory.parent)
            std::memcpy(args.data() + parent, &source, sizeof(source));
        g_process_event(mesh, fn, args.data());
        void* mid = nullptr;
        void* installed = nullptr;
        std::memcpy(&mid, args.data() + returned, sizeof(mid));
        if (GhostResourceLive(GhostObjects(), mid) &&
            GhostClassName(mid, L"MaterialInstanceDynamic") &&
            GhostGlassGetMaterial(mesh, installed) && installed == mid) {
            static std::array<bool, 3> logged{};
            const auto method_index = static_cast<size_t>(&factory - factories);
            if (!logged[method_index]) {
                Log((std::wstring(L"Ghost 3D: private colour MID created using ") +
                     factory.method).c_str());
                logged[method_index] = true;
            }
            return mid;
        }
    }
    // Retain the known-working appearance rather than leaving an untracked
    // material on the component if one of the factories fails partway.
    void* installed = nullptr;
    if (previous && GhostGlassGetMaterial(mesh, installed) && installed != previous)
        GhostGlassSetMaterial(mesh, previous);
    return nullptr;
}

bool GhostGlassMIDScalar(void* mid, FNameView name, float value) {
    if (!g_process_event || !GhostClassName(mid,L"MaterialInstanceDynamic")) return false;
    void* fn=FindReflectedFunction(mid,L"SetScalarParameterValue");
    if (!fn) fn=FindReflectedFunction(mid,L"K2_SetScalarParameterValue");
    int parameter=-1, scalar=-1;
    if (!fn || !GhostParam(fn,L"ParameterName",L"NameProperty",sizeof(name),parameter) ||
        !GhostParam(fn,L"Value",L"FloatProperty",sizeof(value),scalar)) return false;
    alignas(16) std::array<uint8_t,512> args{};
    std::memcpy(args.data()+parameter,&name,sizeof(name));
    std::memcpy(args.data()+scalar,&value,sizeof(value));
    g_process_event(mid,fn,args.data());
    return true;
}

// Read back the CPU-side material override through a reflected UE4 UFunction.
// Do not inspect UMaterialInstanceDynamic's private parameter arrays (the
// previous material audit crashed while doing that). An unavailable getter
// simply means this check cannot be performed in the Shipping build.
bool GhostGlassReadMIDColor(void* mid, FNameView name, LinearColor& result) {
    if (!g_process_event || !GhostClassName(mid, L"MaterialInstanceDynamic"))
        return false;
    void* fn = FindReflectedFunction(mid, L"K2_GetVectorParameterValue");
    if (!fn) fn = FindReflectedFunction(mid, L"GetVectorParameterValue");
    int parameter = -1, returned = -1;
    if (!fn || !GhostParam(fn, L"ParameterName", L"NameProperty", sizeof(name), parameter) ||
        !GhostParam(fn, L"ReturnValue", L"StructProperty", sizeof(result), returned))
        return false;
    alignas(16) std::array<uint8_t, 512> args{};
    std::memcpy(args.data() + parameter, &name, sizeof(name));
    g_process_event(mid, fn, args.data());
    std::memcpy(&result, args.data() + returned, sizeof(result));
    return std::isfinite(result.r) && std::isfinite(result.g) &&
           std::isfinite(result.b) && std::isfinite(result.a);
}

bool GhostGlassMIDColor(void* mid, FNameView name) {
    if (!g_process_event || !GhostClassName(mid,L"MaterialInstanceDynamic")) return false;
    void* fn=FindReflectedFunction(mid,L"SetVectorParameterValue");
    if (!fn) fn=FindReflectedFunction(mid,L"K2_SetVectorParameterValue");
    int parameter=-1, color=-1;
    if (!fn || !GhostParam(fn,L"ParameterName",L"NameProperty",sizeof(name),parameter) ||
        !GhostParam(fn,L"Value",L"StructProperty",sizeof(LinearColor),color)) return false;
    const LinearColor tint = GhostColourLinear();
    alignas(16) std::array<uint8_t,512> args{};
    std::memcpy(args.data()+parameter,&name,sizeof(name));
    std::memcpy(args.data()+color,&tint,sizeof(tint));
    g_process_event(mid,fn,args.data());
    // A reflected setter can exist even when its FName is not an actual
    // shader parameter. Verify the update when UE4 offers a native getter.
    LinearColor actual{};
    if (GhostGlassReadMIDColor(mid, name, actual)) {
        constexpr float kTolerance = 0.002f;
        if (std::fabs(actual.r - tint.r) > kTolerance ||
            std::fabs(actual.g - tint.g) > kTolerance ||
            std::fabs(actual.b - tint.b) > kTolerance) {
            static bool logged = false;
            if (!logged) {
                Log((L"Ghost 3D: material accepted colour setter but read-back "
                     L"did not match the selected RGB for " + UnrealName(name)).c_str());
                logged = true;
            }
            return false;
        }
    }
    return true;
}

// The glass ghost retains the existing six per-part opacity values.
// Opacity OFF never calls this scalar setter: the compiled opaque M_Basic
// shader is used instead, because glass remains translucent at opacity 1.
float GhostPartOpacity(size_t index) {
    constexpr std::array<float,6> kTransparentOpacity{0.62f, 0.36f, 0.26f,
                                                       0.26f, 0.34f, 0.34f};
    return kTransparentOpacity[index];
}

void ApplyGhostGlassMaterial() {
    auto& state=s_ghost_glass;
    if (!g_process_event || state.failed ||
        s_world_parts.count!=static_cast<int>(s_world_parts.parts.size()) ||
        state.applied_parts==s_world_parts.count) return;
    void* source=FindLoadedGhostAppearanceMaterial();
    if (!source || !GhostResourceLive(GhostObjects(),source)) return;
    const auto& part=s_world_parts.parts[static_cast<size_t>(state.applied_parts)];
    if (!GhostLive(GhostObjects(),part) || s_world_api.component_offset<0) return;
    void* mesh=nullptr;
    const auto* address=static_cast<const uint8_t*>(part.actor)+s_world_api.component_offset;
    if (!IsReadable(address,sizeof(mesh))) return;
    std::memcpy(&mesh,address,sizeof(mesh));
    if (!GhostClassName(mesh,L"StaticMeshComponent")) return;

    // All changes are to the ghost's newly spawned mesh, not a level object.
    // The stock material alone cannot respond to the user's colour setting;
    // the material must first be cloned into a private dynamic instance.
    bool tinted = false;
    if (!ResolveGhostGlassParameterNames()) {
        // The registry scan is still in progress: do not permanently mark
        // the part as applied with the stock material yet. Otherwise finding
        // the actual parameter name later would never retry custom tinting.
        if (!state.parameter_scan_complete) return;
        if (!state.warned_unavailable) {
            Log(L"Ghost 3D: Color FName not registered; retaining stock material");
            state.warned_unavailable = true;
        }
    } else if (void* mid = GhostGlassCreateMID(mesh, source)) {
        // M_Basic is BLEND_Opaque. M_Glass_Inst requires its scalar opacity
        // and both of the base glass material's colour parameters.
        // Colour is the essential control. The glass' own alpha and Color2
        // are optional: if their names are absent, preserve native defaults
        // rather than falling back to untinted stock material.
        tinted = GhostGlassMIDColor(mid, state.color_name);
        if (s_transparency_3d && tinted) {
            if (state.opacity_name.index &&
                !GhostGlassMIDScalar(mid, state.opacity_name,
                                     GhostPartOpacity(static_cast<size_t>(state.applied_parts))) &&
                !state.warned_unavailable) {
                Log(L"Ghost 3D: opacity setter unavailable; retaining glass default alpha");
                state.warned_unavailable = true;
            }
            if (state.color2_name.index &&
                !GhostGlassMIDColor(mid, state.color2_name) &&
                !state.warned_unavailable) {
                Log(L"Ghost 3D: secondary glass colour setter unavailable");
                state.warned_unavailable = true;
            }
        }
        if (!tinted && !state.warned_unavailable) {
            Log(L"Ghost 3D: private MID installed but a native colour/opacity setter "
                L"is unavailable; check material parameter ABI");
            state.warned_unavailable = true;
        }
    } else if (!state.warned_unavailable) {
        Log(L"Ghost 3D: no compatible dynamic material factory; "
            L"static glass/basic material cannot follow custom RGB");
        state.warned_unavailable = true;
    }
    if (!tinted && !GhostGlassSetMaterial(mesh, source)) {
        state.failed = true;
        Log(L"Ghost 3D: could not assign Refunct's selected appearance material; "
            L"keeping the six-part default ghost");
        return;
    }
    ++state.applied_parts;
    if (state.applied_parts==s_world_parts.count && !state.reported_applied) {
        Log(tinted ? (s_transparency_3d
                         ? L"Ghost 3D: translucent colour M_Glass_Inst applied to all six parts"
                         : L"Ghost 3D: solid colour opaque M_Basic applied to all six parts")
                   : L"Ghost 3D: appearance material applied; cyan tint not verified");
        state.reported_applied=true;
    }
}

// Resolve the actor's world transform functions once. Rewalking reflection
// metadata and allocating Unreal FNames for six actors every tick would hitch.
struct GhostMotionApi {
    void* actor_class = nullptr;
    void* combined = nullptr, *location = nullptr, *rotation = nullptr;
    int combined_pos = -1, combined_rot = -1, combined_teleport = -1;
    int location_pos = -1, rotation_rot = -1;
};
GhostMotionApi s_ghost_motion{};
bool ResolveGhostMotion(void* actor) {
    void* cls = static_cast<const UObjectHeader*>(actor)->class_ptr;
    if (s_ghost_motion.actor_class == cls &&
        (s_ghost_motion.combined ||
         (s_ghost_motion.location && s_ghost_motion.rotation))) return true;
    s_ghost_motion = {};
    s_ghost_motion.actor_class = cls;
    void* fn = FindReflectedFunction(actor, L"K2_SetActorLocationAndRotation");
    if (fn && GhostParam(fn, L"NewLocation", L"StructProperty", sizeof(Vec3),
                         s_ghost_motion.combined_pos) &&
        GhostParam(fn, L"NewRotation", L"StructProperty", sizeof(Rotator),
                   s_ghost_motion.combined_rot)) {
        s_ghost_motion.combined = fn;
        GhostParam(fn, L"bTeleport", L"BoolProperty", 1, s_ghost_motion.combined_teleport);
        return true;
    }
    s_ghost_motion.combined = nullptr;
    s_ghost_motion.location = FindReflectedFunction(actor, L"K2_SetActorLocation");
    s_ghost_motion.rotation = FindReflectedFunction(actor, L"K2_SetActorRotation");
    if (!s_ghost_motion.location || !s_ghost_motion.rotation ||
        !GhostParam(s_ghost_motion.location, L"NewLocation", L"StructProperty",
                    sizeof(Vec3), s_ghost_motion.location_pos) ||
        !GhostParam(s_ghost_motion.rotation, L"NewRotation", L"StructProperty",
                    sizeof(Rotator), s_ghost_motion.rotation_rot)) return false;
    return true;
}
bool GhostCallLocation(void* actor, const Vec3& position, float yaw) {
    if (!g_process_event || !IsReadable(actor, sizeof(UObjectHeader)) ||
        static_cast<const UObjectHeader*>(actor)->class_ptr != s_ghost_motion.actor_class)
        return false;
    alignas(16) std::array<uint8_t, 512> args{};
    const Rotator rot{0.f, yaw, 0.f};
    if (s_ghost_motion.combined) {
        std::memcpy(args.data() + s_ghost_motion.combined_pos, &position, sizeof(position));
        std::memcpy(args.data() + s_ghost_motion.combined_rot, &rot, sizeof(rot));
        if (s_ghost_motion.combined_teleport >= 0)
            args[s_ghost_motion.combined_teleport] = 1;
        g_process_event(actor, s_ghost_motion.combined, args.data());
        return true;
    }
    if (!s_ghost_motion.location || !s_ghost_motion.rotation) return false;
    std::memcpy(args.data() + s_ghost_motion.location_pos, &position, sizeof(position));
    g_process_event(actor, s_ghost_motion.location, args.data());
    args.fill(0);
    std::memcpy(args.data() + s_ghost_motion.rotation_rot, &rot, sizeof(rot));
    g_process_event(actor, s_ghost_motion.rotation, args.data());
    return true;
}

struct GhostLimb {float x, y, z; Vec3 half_extent;};
constexpr std::array<GhostLimb, 6> kWorldGhostLimbs{{
    {0.f,   0.f,  70.f, {14.f,14.f,18.f}}, // Head top at capsule Z+88.
    {0.f,   0.f,  17.f, {18.f,12.f,34.f}}, // Torso
    {0.f, -26.f,  15.f, { 9.f, 8.f,34.f}}, // Left arm
    {0.f,  26.f,  15.f, { 9.f, 8.f,34.f}}, // Right arm
    {0.f, -10.f, -53.f, { 9.f, 9.f,35.f}}, // Left leg foot Z-88.
    {0.f,  10.f, -53.f, { 9.f, 9.f,35.f}}  // Right leg
}};

Vec3 WorldGhostLimbCenter(const ghost_data::Sample& sample, size_t index) {
    constexpr float kRadians = 0.01745329251994329577f;
    const float angle = sample.yaw * kRadians;
    const float co = std::cos(angle), si = std::sin(angle);
    const auto& limb = kWorldGhostLimbs[index];
    return {sample.x + co * limb.x - si * limb.y,
            sample.y + si * limb.x + co * limb.y,
            sample.z + limb.z};
}

bool WorldGhostActorBounds(void* actor, const Vec3& start, Vec3& extents, Vec3& center_offset) {
    void* function = FindReflectedFunction(actor, L"GetActorBounds");
    int origin = -1, extent = -1;
    if (!function || !GhostParam(function, L"Origin", L"StructProperty", sizeof(Vec3), origin) ||
        !GhostParam(function, L"BoxExtent", L"StructProperty", sizeof(Vec3), extent)) return false;
    alignas(16) std::array<uint8_t, 512> args{};
    g_process_event(actor, function, args.data());
    Vec3 middle{};
    std::memcpy(&middle, args.data() + origin, sizeof(middle));
    std::memcpy(&extents, args.data() + extent, sizeof(extents));
    center_offset = {middle.x - start.x, middle.y - start.y, middle.z - start.z};
    return std::isfinite(extents.x) && std::isfinite(extents.y) &&
        std::isfinite(extents.z) && extents.x >= 1.f && extents.y >= 1.f &&
        extents.z >= 1.f && extents.x < 100000.f && extents.y < 100000.f &&
        extents.z < 100000.f;
}

// Remember the last failed native stage without writing the same warning on
// every retry. Reporting the precise stage matters: successful reflection
// discovery does not prove that Shipping UE4 can construct an actor.
const wchar_t* s_last_ghost_spawn_failure = nullptr;
bool WorldGhostSpawnFailed(const wchar_t* stage) {
    if (s_last_ghost_spawn_failure != stage) {
        Log((std::wstring(L"Ghost 3D: initialization failed at ") + stage).c_str());
        s_last_ghost_spawn_failure = stage;
    }
    return false;
}

bool SpawnWorldGhostPart(MyCharacter* player, const ghost_data::Sample& pose, size_t index) {
    auto& api = s_world_api;
    auto& model = s_world_parts;
    if (!ResolveWorldGhostAssets() || index >= kWorldGhostLimbs.size())
        return WorldGhostSpawnFailed(L"native asset resolution");
    const Vec3 start{pose.x,pose.y,pose.z};
    GhostUETransform spawn{};
    spawn.translation[0] = start.x;
    spawn.translation[1] = start.y;
    spawn.translation[2] = start.z;
    alignas(16) std::array<uint8_t, 512> args{};
    std::memcpy(args.data() + api.begin_world, &player, sizeof(player));
    std::memcpy(args.data() + api.begin_class, &api.actor_class, sizeof(void*));
    std::memcpy(args.data() + api.begin_transform, &spawn, sizeof(spawn));
    args[api.begin_collision] = 1; // ESpawnActorCollisionHandlingMethod::AlwaysSpawn
    g_process_event(api.gameplay, api.begin, args.data());
    void* actor = nullptr;
    std::memcpy(&actor, args.data() + api.begin_result, sizeof(actor));
    if (!IsReadable(actor, sizeof(UObjectHeader)) ||
        static_cast<const UObjectHeader*>(actor)->class_ptr != api.actor_class)
        return WorldGhostSpawnFailed(L"BeginDeferredActorSpawnFromClass returned no StaticMeshActor");
    const auto* objects = GhostObjects();
    const auto* hdr = static_cast<const UObjectHeader*>(actor);
    if (!objects || hdr->index < 0 || hdr->index >= objects->num_elements ||
        objects->objects[hdr->index].object != actor)
        return WorldGhostSpawnFailed(L"spawned actor missing from live UObject registry");
    WorldGhostActor part{actor,hdr->index,objects->objects[hdr->index].serial_number,{}};
    // Store immediately, including the deferred actor, so any later failure
    // cleans up only the actor that the ghost feature created.
    model.parts[index] = part;

    // StaticMeshActor defaults to Static mobility. Configure the verified
    // UProperty while this actor is still deferred; the reflected SetMobility
    // function does not exist in this Shipping build (confirmed by game log).
    void* component = nullptr;
    const auto* component_ptr = static_cast<const uint8_t*>(actor) + api.component_offset;
    if (!IsReadable(component_ptr, sizeof(component)))
        return WorldGhostSpawnFailed(L"deferred StaticMeshComponent pointer unreadable");
    std::memcpy(&component, component_ptr, sizeof(component));
    if (!GhostClassName(component, L"StaticMeshComponent"))
        return WorldGhostSpawnFailed(L"deferred StaticMeshComponent missing");
    if (!GhostPrepareDeferredMobility(component))
        return WorldGhostSpawnFailed(L"deferred Mobility UProperty unavailable");

    // Give the unregistered/deferred mesh its asset *before* finishing spawn.
    // On already-registered static components UE4 may reject SetStaticMesh.
    void* set_mesh = FindReflectedFunction(component, L"SetStaticMesh");
    int mesh_field = -1;
    if (!set_mesh || !GhostParam(set_mesh, L"NewMesh", L"ObjectProperty", sizeof(void*), mesh_field))
        return WorldGhostSpawnFailed(L"deferred SetStaticMesh native signature");
    args.fill(0);
    std::memcpy(args.data() + mesh_field, &api.block_mesh, sizeof(void*));
    g_process_event(component, set_mesh, args.data());
    const int mesh_offset = FindClassPropertyOffset(
        static_cast<const UObjectHeader*>(component)->class_ptr, L"StaticMesh");
    if (mesh_offset < 0 || mesh_offset > 0x2000 ||
        !IsReadable(static_cast<const uint8_t*>(component) + mesh_offset, sizeof(void*)) ||
        *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(component) + mesh_offset) != api.block_mesh)
        return WorldGhostSpawnFailed(L"deferred SetStaticMesh did not apply the mesh");

    // Finish construction once mesh and Mobility are set. Re-read the component
    // pointer because FinishSpawningActor may reconstruct native components.
    args.fill(0);
    std::memcpy(args.data() + api.finish_actor, &actor, sizeof(actor));
    std::memcpy(args.data() + api.finish_transform, &spawn, sizeof(spawn));
    g_process_event(api.gameplay, api.finish, args.data());
    if (!GhostLive(GhostObjects(), model.parts[index]))
        return WorldGhostSpawnFailed(L"FinishSpawningActor invalidated the actor");
    if (!IsReadable(component_ptr, sizeof(component)))
        return WorldGhostSpawnFailed(L"finished StaticMeshComponent pointer unreadable");
    std::memcpy(&component, component_ptr, sizeof(component));
    if (!GhostClassName(component, L"StaticMeshComponent"))
        return WorldGhostSpawnFailed(L"finished StaticMeshComponent missing");
    if (!GhostDeferredMobilityIsMovable(component))
        return WorldGhostSpawnFailed(L"FinishSpawningActor reset Movable mobility");
    if (!GhostCallBool(actor, L"SetActorEnableCollision", L"bNewActorEnableCollision", 0))
        return WorldGhostSpawnFailed(L"SetActorEnableCollision(false)");
    if (!GhostCallByte(component, L"SetCollisionEnabled", L"NewType", 0))
        return WorldGhostSpawnFailed(L"SetCollisionEnabled(NoCollision)");
    // The construction pass must not replace the configured mesh.
    if (!IsReadable(static_cast<const uint8_t*>(component) + mesh_offset, sizeof(void*)) ||
        *reinterpret_cast<void* const*>(static_cast<const uint8_t*>(component) + mesh_offset) != api.block_mesh)
        return WorldGhostSpawnFailed(L"FinishSpawningActor reset StaticMesh");
    if (!model.bounds_ready) {
        if (!WorldGhostActorBounds(actor, start, model.original_extent,
                                   model.original_center_offset))
            return WorldGhostSpawnFailed(L"GetActorBounds (mesh extent unavailable)");
        model.bounds_ready = true;
    }
    const auto& e = model.original_extent;
    const auto& target = kWorldGhostLimbs[index].half_extent;
    const Vec3 scale{target.x/e.x, target.y/e.y, target.z/e.z};
    if (!std::isfinite(scale.x) || !std::isfinite(scale.y) || !std::isfinite(scale.z) ||
        scale.x <= 0.f || scale.y <= 0.f || scale.z <= 0.f)
        return WorldGhostSpawnFailed(L"non-finite visual mesh scale");
    if (!GhostCallVector(actor, L"SetActorScale3D", L"NewScale3D", scale))
        return WorldGhostSpawnFailed(L"SetActorScale3D");
    // This verification is deliberately after SetStaticMesh and scale because
    // those operations can re-register primitive components.
    void* get_collision = FindReflectedFunction(component, L"GetCollisionEnabled");
    int collision_result = -1;
    if (!get_collision || !(GhostParam(get_collision, L"ReturnValue", L"EnumProperty",
                                      1, collision_result) ||
                             GhostParam(get_collision, L"ReturnValue", L"ByteProperty",
                                      1, collision_result)))
        return WorldGhostSpawnFailed(L"GetCollisionEnabled native signature");
    args.fill(0);
    g_process_event(component, get_collision, args.data());
    if (args[collision_result] != 0)
        return WorldGhostSpawnFailed(L"component collision remained enabled");
    if (!ResolveGhostMotion(actor))
        return WorldGhostSpawnFailed(L"K2 actor transform functions");
    if (!GhostCallBool(actor, L"SetActorHiddenInGame", L"bNewHidden", 1))
        return WorldGhostSpawnFailed(L"SetActorHiddenInGame");
    model.parts[index].scale = scale;
    s_last_ghost_spawn_failure = nullptr;
    return true;
}

void PlaceWorldGhost(const ghost_data::Sample& pose) {
    for (size_t i = 0; i < s_world_parts.parts.size(); ++i) {
        const auto& part = s_world_parts.parts[i];
        const Vec3 center = WorldGhostLimbCenter(pose, i);
        // Compensate for the source mesh's pivot. This keeps the recorded
        // capsule feet on the ground regardless of mesh origin convention.
        constexpr float kRadians = 0.01745329251994329577f;
        const float angle = pose.yaw * kRadians;
        const float co = std::cos(angle), si = std::sin(angle);
        const Vec3 shift{
            s_world_parts.original_center_offset.x * part.scale.x,
            s_world_parts.original_center_offset.y * part.scale.y,
            s_world_parts.original_center_offset.z * part.scale.z};
        const Vec3 position{center.x - (co * shift.x - si * shift.y),
                            center.y - (si * shift.x + co * shift.y),
                            center.z - shift.z};
        if (!GhostCallLocation(part.actor, position, pose.yaw)) {
            WorldGhostSpawnFailed(L"updating world-space limb transform");
            DestroyWorldGhostActors();
            return;
        }
    }
}

// The split recorder has already processed native button contacts before
// DrawPBGhost runs. Use its captured contact time, not the HUD frame timestamp,
// so a frame hitch cannot introduce an artificial delay into the ghost.
// Select a single section using the player's most recent native button press.
// The source preference is independent from whether the ghost waits at buttons.
bool ButtonSynchronizedPlaybackMs(std::uint32_t& result,
                                  const std::vector<ghost_data::Sample>*& track,
                                  bool& fastest_source, bool& furthest_source,
                                  bool use_fastest_sections = true) {
    track = nullptr;
    fastest_source = false;
    furthest_source = false;
    if (!g_button_state_initialized || !g_run_started_at_zero ||
        g_practice_active || g_preloaded_restart_active || g_progress_restart_active ||
        g_last_button_count < 0 || g_last_button_count >= kRefunctButtonCount)
        return false;
    const int button = g_last_button_count; // 0 = start -> button 1
    const double pressed_at = button == 0 ? 0.0 :
        (!g_button_splits.empty() && g_button_splits.back().button_number == button
            ? g_button_splits.back().run_seconds : -1.0);
    if (!std::isfinite(pressed_at) || pressed_at < 0.0 ||
        !std::isfinite(g_elapsed_seconds) || g_elapsed_seconds < pressed_at)
        return false;

    const auto& fastest = s_fastest[static_cast<std::size_t>(button)];
    if (use_fastest_sections && !fastest.empty()) {
        const std::uint32_t duration_ms =
            Milliseconds(g_pb.best_segment_seconds[static_cast<std::size_t>(button)]);
        if (duration_ms) {
            result = std::min(Milliseconds(g_elapsed_seconds - pressed_at), duration_ms);
            track = &fastest;
            fastest_source = true;
            return true;
        }
    }

    // In full-run mode prefer the PB section. In fastest-sections mode the
    // completed-PB movement is the last fallback when no gold/furthest exists.
    const auto use_pb = [&]() -> bool {
        if (s_playback.empty()) return false;
        const double start = button == 0 ? 0.0 :
            g_pb.button_seconds[static_cast<std::size_t>(button - 1)];
        const double end = g_pb.button_seconds[static_cast<std::size_t>(button)];
        if (!ghost_data::ButtonSegmentPlaybackTime(
                g_elapsed_seconds, pressed_at, start, end,
                Milliseconds(g_pb.total_seconds), result)) return false;
        track = &s_playback;
        return true;
    };
    if (!use_fastest_sections && use_pb()) return true;

    if (s_furthest.reached > static_cast<std::uint32_t>(button) &&
        !s_furthest.samples.empty()) {
        const double start = button == 0 ? 0.0 :
            s_furthest.button_ms[static_cast<std::size_t>(button - 1)] / 1000.0;
        const double end = s_furthest.button_ms[static_cast<std::size_t>(button)] / 1000.0;
        if (ghost_data::ButtonSegmentPlaybackTime(
                g_elapsed_seconds, pressed_at, start, end,
                s_furthest.button_ms[s_furthest.reached - 1], result)) {
            track = &s_furthest.samples;
            furthest_source = true;
            return true;
        }
    }
    return use_fastest_sections && use_pb();
}

// Without button synchronisation, chain recorded fastest sections back to
// back. A section without a gold track uses the actual furthest-run or PB
// positions, never invented positions from a split time alone. An unknown
// section ends the available route, holding at its last valid point.
bool ContinuousFastestSectionsPlaybackMs(
        std::uint32_t& result, const std::vector<ghost_data::Sample>*& track,
        bool& fastest_source, bool& furthest_source,
        double elapsed_seconds = g_elapsed_seconds, bool require_clean_run = true,
        bool allow_furthest = true) {
    track = nullptr;
    fastest_source = false;
    furthest_source = false;
    if ((require_clean_run && (!g_run_started_at_zero || !g_button_state_initialized ||
         g_practice_active || g_preloaded_restart_active || g_progress_restart_active)) ||
        !std::isfinite(elapsed_seconds) || elapsed_seconds < 0.0) return false;

    std::uint32_t remaining = Milliseconds(elapsed_seconds);
    std::uint32_t last_time = 0;
    const std::vector<ghost_data::Sample>* last_track = nullptr;
    bool last_fastest = false, last_furthest = false;
    for (int button = 0; button < kRefunctButtonCount; ++button) {
        const auto index = static_cast<std::size_t>(button);
        const std::vector<ghost_data::Sample>* selected = nullptr;
        std::uint32_t start_ms = 0, duration_ms = 0;
        bool gold = false, furthest = false;

        const auto gold_duration = Milliseconds(g_pb.best_segment_seconds[index]);
        if (!s_fastest[index].empty() && gold_duration) {
            selected = &s_fastest[index];
            duration_ms = gold_duration;
            gold = true;
        } else if (allow_furthest && !s_furthest.samples.empty() &&
                   s_furthest.reached > static_cast<std::uint32_t>(button)) {
            const auto end_ms = s_furthest.button_ms[index];
            start_ms = button == 0 ? 0 : s_furthest.button_ms[index - 1];
            if (end_ms > start_ms) {
                selected = &s_furthest.samples;
                duration_ms = end_ms - start_ms;
                furthest = true;
            }
        } else if (!s_playback.empty()) {
            const auto end_ms = Milliseconds(g_pb.button_seconds[index]);
            start_ms = button == 0 ? 0 :
                Milliseconds(g_pb.button_seconds[index - 1]);
            if (end_ms > start_ms) {
                selected = &s_playback;
                duration_ms = end_ms - start_ms;
            }
        }
        if (!selected || !duration_ms) break;
        last_track = selected;
        last_time = start_ms + duration_ms;
        last_fastest = gold;
        last_furthest = furthest;
        if (remaining <= duration_ms) {
            result = start_ms + remaining;
            track = selected;
            fastest_source = gold;
            furthest_source = furthest;
            return true;
        }
        remaining -= duration_ms;
    }
    if (!last_track) return false;
    // Stay at the end of the furthest available segment when the route ends.
    track = last_track;
    result = last_time;
    fastest_source = last_fastest;
    furthest_source = last_furthest;
    return true;
}

// The HUD and the world-space renderer must use the same exact playback
// clock/source, including button-sync, gold sections and unfinished runs.
struct GhostPlayback {
    const std::vector<ghost_data::Sample>* track = nullptr;
    std::uint32_t elapsed_ms = 0;
    bool fastest = false, furthest = false;
};
bool SelectGhostPlayback(GhostPlayback& playback) {
    LoadGhostSettingOnce();
    if (s_replay.requested || s_replay.active) {
        if (s_loaded_profile != g_pb_path) return false;
        playback.elapsed_ms = static_cast<std::uint32_t>(s_replay.elapsed_ms);
        playback.track = &s_playback;
        if (s_replay.fastest) {
            if (!ContinuousFastestSectionsPlaybackMs(playback.elapsed_ms,
                    playback.track, playback.fastest, playback.furthest,
                    s_replay.elapsed_ms / 1000.0, false, false)) return false;
        } else if (s_playback.empty()) {
            // Explicit PB source: never substitute an unfinished run.
            return false;
        }
        return playback.track && !playback.track->empty();
    }
    if (!s_show || !g_engine_timer_valid || g_timer_complete ||
        CustomMapActive() || CustomMapApplying() ||
        s_loaded_profile != g_pb_path) return false;
    playback.elapsed_ms = Milliseconds(g_elapsed_seconds);
    playback.track = &s_playback;
    if (s_button_sync) {
        if (!ButtonSynchronizedPlaybackMs(playback.elapsed_ms, playback.track,
                    playback.fastest, playback.furthest,
                    s_use_fastest_sections)) return false;
    } else if (s_use_fastest_sections) {
        if (!ContinuousFastestSectionsPlaybackMs(playback.elapsed_ms,
                    playback.track, playback.fastest, playback.furthest)) return false;
    } else if (s_playback.empty() || Milliseconds(g_pb.total_seconds) == 0) {
        if (s_furthest.samples.empty() || s_furthest.reached == 0) return false;
        playback.track = &s_furthest.samples;
        playback.furthest = true;
        playback.elapsed_ms = (std::min)(playback.elapsed_ms,
            s_furthest.button_ms[s_furthest.reached - 1]);
    }
    return playback.track && !playback.track->empty();
}

// Replay uses exactly the same segment priorities as replay visual playback:
// best segment -> PB fallback only. Ordinary ghost playback can still use a
// furthest run, but replay never imports it as if it were a PB or a gold.
bool BuildGhostReplayButtonTimeline(bool fastest,
        ghost_replay_progress::Timeline<kRefunctButtonCount>& timeline) {
    timeline = {};
    if (fastest) {
        std::uint64_t total = 0;
        for (int button = 0; button < kRefunctButtonCount; ++button) {
            const auto i = static_cast<std::size_t>(button);
            std::uint32_t duration = 0;
            const auto gold_duration = Milliseconds(g_pb.best_segment_seconds[i]);
            if (!s_fastest[i].empty() && gold_duration) {
                duration = gold_duration;
            } else if (!s_playback.empty()) {
                const auto start = button ? Milliseconds(g_pb.button_seconds[i - 1]) : 0;
                const auto end = Milliseconds(g_pb.button_seconds[i]);
                if (end > start) duration = end - start;
            }
            if (!duration || total + duration > 86400000u) break;
            total += duration;
            timeline.duration_ms = static_cast<std::uint32_t>(total);
            if (!timeline.Add(timeline.duration_ms)) return false;
        }
    } else if (!s_playback.empty()) {
        timeline.duration_ms = (std::max)(s_playback.back().time_ms, Milliseconds(g_pb.total_seconds));
        for (int i = 0; i < kRefunctButtonCount; ++i) {
            const auto at = Milliseconds(g_pb.button_seconds[static_cast<std::size_t>(i)]);
            if (!at || at > timeline.duration_ms) break;
            if (!timeline.Add(at)) return false;
        }
    }
    // PB means a complete, valid PB ghost. No furthest-run fallback.
    if (!fastest && (s_playback.empty() ||
        !Milliseconds(g_pb.total_seconds) ||
        timeline.count != static_cast<std::size_t>(kRefunctButtonCount))) return false;
    // Missing timestamps cannot be reconstructed from 20Hz position samples.
    return timeline.count > 0 && timeline.duration_ms > 0;
}

// CameraActor is a native engine class, not a custom cooked dependency.
// Scan a bounded batch only while a replay start was requested.
bool FindReplayCameraClass() {
    const auto* objects = GhostObjects();
    if (!objects) return false;
    if (GhostResourceLive(objects, s_replay.camera_class) &&
        GhostClassName(s_replay.camera_class, L"Class")) return true;
    s_replay.camera_class = nullptr;
    const int32_t end = (std::min)(s_replay.class_scan_cursor + 320, objects->num_elements);
    const std::uint64_t started = GetTickCount64();
    for (; s_replay.class_scan_cursor < end; ++s_replay.class_scan_cursor) {
        if (GetTickCount64() - started >= 1) break;
        const auto* item = objects->objects + s_replay.class_scan_cursor;
        if (!item->object || (static_cast<uint32_t>(item->flags) & 0x30000000u) ||
            !IsReadable(item->object, sizeof(UObjectHeader))) continue;
        if (UnrealName(item->object->name) == L"CameraActor" &&
            GhostClassName(item->object, L"Class")) {
            s_replay.camera_class = item->object;
            return true;
        }
    }
    return false;
}

bool ReplayCameraLive() {
    const auto* objects = GhostObjects();
    const auto& part = s_replay.camera;
    return objects && part.actor && part.index >= 0 &&
        part.index < objects->num_elements &&
        IsReadable(objects->objects + part.index, sizeof(GlobalObjectItem)) &&
        objects->objects[part.index].object == part.actor &&
        objects->objects[part.index].serial_number == part.serial &&
        !(static_cast<uint32_t>(objects->objects[part.index].flags) & 0x30000000u) &&
        IsReadable(part.actor, sizeof(UObjectHeader)) &&
        static_cast<const UObjectHeader*>(part.actor)->class_ptr == s_replay.camera_class;
}

// Read the game's EXISTING mouse-look rotation instead of hooking Unreal's
// input processing or calling unsafe material/actor APIs. In UE4 the local
// PlayerController normally continues updating ControlRotation while viewing
// a CameraActor. Arrow keys are a fallback when the build does not do so.
float ReplayAngleDelta(float latest, float previous) {
    if (!std::isfinite(latest) || !std::isfinite(previous)) return 0.f;
    float delta = std::remainder(latest - previous, 360.f);
    return std::fabs(delta) <= 75.f ? delta : 0.f; // skip teleports/resets
}

void UpdateReplayFreeCamera(const Vec3& follow_location, const Rotator& follow_rotation) {
    auto& cam = s_replay.free_camera;
    const std::uint64_t now = GetTickCount64();
    if (!cam.initialized) {
        cam.position = follow_location;
        cam.rotation = {0.f, follow_rotation.yaw, 0.f};
        cam.target_rotation = cam.rotation;
        cam.last_input_tick = now;
        cam.initialized = true;
    }
    // Time-based damping keeps the same feel at 30, 60 and 240 FPS.
    // A long hitch must not fling the camera across the level.
    const float dt = static_cast<float>((std::min)(
        now >= cam.last_input_tick ? now - cam.last_input_tick : std::uint64_t{0},
        std::uint64_t{100})) / 1000.f;
    cam.last_input_tick = now;
    if (!s_replay.player || !s_replay.player->controller ||
        !IsReadable(s_replay.player->controller, sizeof(PlayerController))) {
        cam.velocity = {};
        cam.controller_sample_valid = false;
        return;
    }
    const Rotator current = s_replay.player->controller->control_rotation;
    const bool valid_rotation = std::isfinite(current.pitch) &&
        std::isfinite(current.yaw) && std::fabs(current.pitch) < 3600.f &&
        std::fabs(current.yaw) < 360000.f;
    const bool menu_open = g_menu_open.load(std::memory_order_relaxed);
    // F5 is just an overlay. Normal menu pages allow camera movement;
    // text entry, search, hotkey capture and an unfocused window do not.
    const bool can_move = HasFocusedGameWindow() &&
        !g_rebinding.load(std::memory_order_relaxed) &&
        !g_name_editing.load(std::memory_order_relaxed) &&
        !g_coordinate_editing.load(std::memory_order_relaxed) &&
        !(menu_open && IsMenuSearchTypingKey('W'));
    if (valid_rotation) {
        if (cam.controller_sample_valid && can_move) {
            cam.target_rotation.yaw += ReplayAngleDelta(current.yaw, cam.last_controller_rotation.yaw);
            cam.target_rotation.pitch += ReplayAngleDelta(current.pitch, cam.last_controller_rotation.pitch);
        }
        cam.last_controller_rotation = current;
    }
    cam.controller_sample_valid = valid_rotation;
    if (!can_move) {
        // Stop on focus loss/editing; don't drift while the player is typing.
        cam.velocity = {};
        return;
    }
    if (dt <= 0.f) return;

    const auto down = [](int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; };
    // Arrow keys remain the F5 menu's navigation controls while it is open.
    if (!menu_open) {
        constexpr float kKeyboardLookDegreesPerSecond = 105.f;
        cam.target_rotation.yaw += (float(down(VK_RIGHT)) - float(down(VK_LEFT))) *
            kKeyboardLookDegreesPerSecond * dt;
        cam.target_rotation.pitch += (float(down(VK_UP)) - float(down(VK_DOWN))) *
            kKeyboardLookDegreesPerSecond * dt;
    }
    cam.target_rotation.pitch = std::clamp(cam.target_rotation.pitch, -85.f, 85.f);
    cam.target_rotation.yaw = std::remainder(cam.target_rotation.yaw, 360.f);
    const float rotation_alpha = 1.f - std::exp(-dt / 0.065f);
    // Take the shortest yaw path across -180 / +180.
    cam.rotation.yaw += std::remainder(cam.target_rotation.yaw - cam.rotation.yaw, 360.f) * rotation_alpha;
    cam.rotation.yaw = std::remainder(cam.rotation.yaw, 360.f);
    cam.rotation.pitch += (cam.target_rotation.pitch - cam.rotation.pitch) * rotation_alpha;
    cam.rotation.roll = 0.f;

    // Match Movement -> Fly input: W/A/S/D move horizontally by yaw,
    // Space goes up, Shift goes down, and Fly speed is the maximum speed.
    // Acceleration/deceleration make camera movement less abrupt.
    const float yaw = cam.rotation.yaw * 0.01745329251994329577f;
    const float cy = std::cos(yaw), sy = std::sin(yaw);
    const float forward = float(down('W')) - float(down('S'));
    const float strafe = float(down('D')) - float(down('A'));
    const float horizontal_length = std::hypot(forward, strafe);
    const float fly_speed = std::isfinite(g_fly_speed) && g_fly_speed > 0.f
        ? g_fly_speed : 1200.f;
    Vec3 desired_velocity{};
    if (horizontal_length > 0.0001f) {
        desired_velocity.x = (forward * cy - strafe * sy) * fly_speed / horizontal_length;
        desired_velocity.y = (forward * sy + strafe * cy) * fly_speed / horizontal_length;
    }
    desired_velocity.z = (float(down(VK_SPACE)) - float(down(VK_SHIFT))) * fly_speed;
    constexpr float kAccelerateSeconds = 0.12f;
    constexpr float kDecelerateSeconds = 0.18f;
    const auto approach = [dt](float previous, float desired) {
        const float tau = std::fabs(desired) > 0.0001f
            ? kAccelerateSeconds : kDecelerateSeconds;
        return previous + (desired - previous) * (1.f - std::exp(-dt / tau));
    };
    cam.velocity.x = approach(cam.velocity.x, desired_velocity.x);
    cam.velocity.y = approach(cam.velocity.y, desired_velocity.y);
    cam.velocity.z = approach(cam.velocity.z, desired_velocity.z);
    // Remove tiny floating-point tails so the camera truly comes to rest.
    if (horizontal_length < 0.0001f && std::hypot(cam.velocity.x, cam.velocity.y) < 0.1f) {
        cam.velocity.x = 0.f;
        cam.velocity.y = 0.f;
    }
    if (std::fabs(desired_velocity.z) < 0.0001f && std::fabs(cam.velocity.z) < 0.1f)
        cam.velocity.z = 0.f;
    cam.position.x += cam.velocity.x * dt;
    cam.position.y += cam.velocity.y * dt;
    cam.position.z += cam.velocity.z * dt;

}

// Mouse-orbit camera: stays centered on the ghost, even while the ghost
// changes direction or the replay is paused. Up/Down zoom only when F5 is
// closed; the menu must retain its own navigation keys while visible.
void UpdateReplayOrbitCamera(const ghost_data::Sample& pose,
                             Vec3& location, Rotator& rotation) {
    auto& cam = s_replay.orbit_camera;
    const std::uint64_t now = GetTickCount64();
    if (!cam.initialized) {
        cam.initialized = true;
        cam.last_input_tick = now;
    }
    const float dt = static_cast<float>((std::min)(
        now >= cam.last_input_tick ? now - cam.last_input_tick : std::uint64_t{0},
        std::uint64_t{100})) / 1000.f;
    cam.last_input_tick = now;

    const bool menu_open = g_menu_open.load(std::memory_order_relaxed);
    const bool can_control = HasFocusedGameWindow() &&
        !g_rebinding.load(std::memory_order_relaxed) &&
        !g_name_editing.load(std::memory_order_relaxed) &&
        !g_coordinate_editing.load(std::memory_order_relaxed) &&
        !(menu_open && IsMenuSearchTypingKey('W'));
    bool valid_controller = false;
    if (s_replay.player && s_replay.player->controller &&
        IsReadable(s_replay.player->controller, sizeof(PlayerController))) {
        const Rotator current = s_replay.player->controller->control_rotation;
        valid_controller = std::isfinite(current.pitch) && std::isfinite(current.yaw) &&
            std::fabs(current.pitch) < 3600.f && std::fabs(current.yaw) < 360000.f;
        if (valid_controller) {
            if (can_control && cam.controller_sample_valid) {
                cam.target_yaw_offset += ReplayAngleDelta(
                    current.yaw, cam.last_controller_rotation.yaw);
                cam.target_pitch += ReplayAngleDelta(
                    current.pitch, cam.last_controller_rotation.pitch);
            }
            cam.last_controller_rotation = current;
        }
    }
    cam.controller_sample_valid = valid_controller;

    // Mouse wheel is observed by a same-process Win32 hook and queued. Its
    // signed delta preserves both directions and high-resolution mouse wheels.
    // Positive wheel motion (up) zooms in; negative (down) zooms out.
    const int wheel_delta = s_replay_orbit_wheel_delta.exchange(0, std::memory_order_relaxed);
    if (can_control && wheel_delta) {
        constexpr float kUnitsPerNotch = 70.f;
        cam.target_distance = std::clamp(cam.target_distance -
            static_cast<float>(wheel_delta) * (kUnitsPerNotch / 120.f),
            110.f, 1350.f);
    }
    cam.target_pitch = std::clamp(cam.target_pitch, -80.f, 80.f);
    cam.target_yaw_offset = std::remainder(cam.target_yaw_offset, 360.f);
    if (dt > 0.f) {
        const float look_alpha = 1.f - std::exp(-dt / 0.065f);
        const float zoom_alpha = 1.f - std::exp(-dt / 0.10f);
        cam.yaw_offset += std::remainder(
            cam.target_yaw_offset - cam.yaw_offset, 360.f) * look_alpha;
        cam.pitch += (cam.target_pitch - cam.pitch) * look_alpha;
        cam.distance += (cam.target_distance - cam.distance) * zoom_alpha;
    }
    rotation = {cam.pitch, std::remainder(pose.yaw + cam.yaw_offset, 360.f), 0.f};
    constexpr float kRadians = 0.01745329251994329577f;
    const float yaw = rotation.yaw * kRadians;
    const float pitch = rotation.pitch * kRadians;
    const float horizontal = std::cos(pitch) * cam.distance;
    constexpr float kOrbitEyeHeight = 64.f;
    location = {pose.x - std::cos(yaw) * horizontal,
                pose.y - std::sin(yaw) * horizontal,
                pose.z + kOrbitEyeHeight - std::sin(pitch) * cam.distance};
}

bool ReplayMoveCamera(const ghost_data::Sample& pose) {
    if (!ReplayCameraLive() || !s_replay.move_fn) return false;
    constexpr float kRadians = 0.01745329251994329577f;
    const float angle = pose.yaw * kRadians;
    // Keep the original follow framing for level look and manual camera modes.
    constexpr float kFirstPersonEyeOffset = 64.f;
    Vec3 location = s_replay_camera_mode == 1
        ? Vec3{pose.x, pose.y, pose.z + kFirstPersonEyeOffset}
        : Vec3{pose.x - std::cos(angle) * 285.f,
               pose.y - std::sin(angle) * 285.f,
               pose.z + 135.f};
    Rotator rotation{s_replay_camera_mode == 1 ? pose.pitch : -18.f, pose.yaw, 0.f};
    if (s_replay_camera_mode == 0) {
        // Orbit the same focal point rather than just tilting in place: the
        // ghost stays framed when its recorded vertical look changes.
        constexpr float kBasePitch = -18.f;
        constexpr float kBaseBack = 285.f;
        constexpr float kBaseHeight = 135.f;
        static const float distance = kBaseBack / std::cos(kBasePitch * kRadians);
        static const float focus_height = kBaseHeight + distance * std::sin(kBasePitch * kRadians);
        rotation.pitch = std::clamp(kBasePitch + pose.pitch, -85.f, 85.f);
        const float pitch = rotation.pitch * kRadians;
        const float horizontal = distance * std::cos(pitch);
        location = {pose.x - std::cos(angle) * horizontal,
                    pose.y - std::sin(angle) * horizontal,
                    pose.z + focus_height - distance * std::sin(pitch)};
    } else if (s_replay_camera_mode == 2) {
        UpdateReplayFreeCamera(location, rotation);
        location = s_replay.free_camera.position;
        rotation = s_replay.free_camera.rotation;
    } else if (s_replay_camera_mode == 3) {
        UpdateReplayOrbitCamera(pose, location, rotation);
    }
    alignas(16) std::array<uint8_t, 512> params{};
    std::memcpy(params.data() + s_replay.move_location, &location, sizeof(location));
    std::memcpy(params.data() + s_replay.move_rotation, &rotation, sizeof(rotation));
    if (s_replay.move_teleport >= 0) params[s_replay.move_teleport] = 1;
    g_process_event(s_replay.camera.actor, s_replay.move_fn, params.data());
    return ReplayCameraLive();
}

void* ReplayCurrentViewTarget() {
    if (!GhostResourceLive(GhostObjects(), s_replay.controller) ||
        !s_replay.get_view_fn || s_replay.get_view_result < 0) return nullptr;
    alignas(16) std::array<uint8_t, 512> params{};
    g_process_event(s_replay.controller, s_replay.get_view_fn, params.data());
    void* result = nullptr;
    std::memcpy(&result, params.data() + s_replay.get_view_result, sizeof(result));
    return GhostResourceLive(GhostObjects(), result) ? result : nullptr;
}

bool ReplaySetViewTarget(void* target) {
    if (!GhostResourceLive(GhostObjects(), s_replay.controller) ||
        !GhostResourceLive(GhostObjects(), target) || !s_replay.view_fn) return false;
    alignas(16) std::array<uint8_t, 512> params{};
    std::memcpy(params.data() + s_replay.view_target, &target, sizeof(target));
    const float instant = 0.f;
    std::memcpy(params.data() + s_replay.view_blend, &instant, sizeof(instant));
    g_process_event(s_replay.controller, s_replay.view_fn, params.data());
    return true;
}

void StopGhostReplayInternal() {
    const bool replay_changed_progress = s_replay.changed_native_progress ||
        GhostReplayTilesChanged() || GhostReplayLiftsChanged() || GhostReplayCubesChanged();
    ResetGhostReplayLifts();
    ResetGhostReplayTiles();
    ResetGhostReplayCubes();
    s_replay_tile_pose_ready = false;
    s_replay_tile_track = nullptr;
    // Restore the view BEFORE destroying our camera, including on a restart.
    if (s_replay.active && ReplayCurrentViewTarget() == s_replay.camera.actor) {
        // The game may have switched cameras for its own cutscene or restart.
        // Only restore our saved view if we still own the current view target.
        void* restore = GhostResourceLive(GhostObjects(), s_replay.previous_target)
            ? s_replay.previous_target : static_cast<void*>(s_replay.player);
        if (GhostResourceLive(GhostObjects(), restore)) ReplaySetViewTarget(restore);
    }
    if (ReplayCameraLive() && g_process_event) {
        if (void* fn = FindReflectedFunction(s_replay.camera.actor, L"K2_DestroyActor")) {
            alignas(16) std::array<uint8_t, 64> args{};
            g_process_event(s_replay.camera.actor, fn, args.data());
        }
    }
    if (s_replay.waiting_reset && s_replay.owns_pending_restart &&
        g_restart_requested && !replay_changed_progress) {
        // A user can cancel while the R restart is still queued. Do not
        // restart their level for a replay they no longer want to watch.
        g_restart_requested = false;
    }
    if (replay_changed_progress) {
        // Stop is processed by the menu before the next CharacterTick; keep
        // the intervening frame from finalising a synthetic finish/PB.
        g_run_pb_eligible = false;
        g_run_finish_processed = true;
    }
    // Avoid leaving a partly played native run behind. A completed replay
    // remains visible until Stop, then a regular restart clears progress.
    const LevelState* native = PracticeLevelState();
    if (replay_changed_progress && native &&
        !g_restart_requested && !g_full_level_reload_requested &&
        !g_preloaded_restart_requested && !g_practice_restart_requested) {
        g_restart_requested = true;
        Log(L"Ghost replay: restoring world after native buttons, lifts, tiles or cubes");
    }
    s_replay = {};
    s_replay_orbit_active.store(false, std::memory_order_release);
    s_replay_orbit_wheel_delta.store(0, std::memory_order_relaxed);
    s_draw_cache = {};
    // Normal ghost playback resumes with the live player's run clock.
    if (s_world_parts.count) DestroyWorldGhostActors();
}

// Spawn the camera using the SAME guarded deferred actor factory already used
// for the six-part ghost; no guessed engine offsets or global view changes.
bool CreateReplayCamera(MyCharacter* player) {
    if (!ResolveWorldGhostAssets() || !FindReplayCameraClass() ||
        !g_process_event || !GhostResourceLive(GhostObjects(), player->controller)) return false;
    void* get_view = FindReflectedFunction(player->controller, L"GetViewTarget");
    void* set_view = FindReflectedFunction(player->controller, L"SetViewTargetWithBlend");
    int old_target_offset = -1;
    if (!get_view || !set_view ||
        !GhostParam(get_view, L"ReturnValue", L"ObjectProperty", sizeof(void*), old_target_offset) ||
        !GhostParam(set_view, L"NewViewTarget", L"ObjectProperty", sizeof(void*), s_replay.view_target) ||
        !GhostParam(set_view, L"BlendTime", L"FloatProperty", sizeof(float), s_replay.view_blend))
        return false;
    alignas(16) std::array<uint8_t, 512> args{};
    g_process_event(player->controller, get_view, args.data());
    void* previous = nullptr;
    std::memcpy(&previous, args.data() + old_target_offset, sizeof(previous));
    if (!GhostResourceLive(GhostObjects(), previous)) return false;

    const auto& api = s_world_api;
    GhostUETransform spawn{};
    std::memcpy(args.data() + api.begin_world, &player, sizeof(player));
    std::memcpy(args.data() + api.begin_class, &s_replay.camera_class, sizeof(void*));
    std::memcpy(args.data() + api.begin_transform, &spawn, sizeof(spawn));
    args[api.begin_collision] = 1; // AlwaysSpawn; camera has no collision.
    g_process_event(api.gameplay, api.begin, args.data());
    void* actor = nullptr;
    std::memcpy(&actor, args.data() + api.begin_result, sizeof(actor));
    if (!GhostResourceLive(GhostObjects(), actor) ||
        static_cast<const UObjectHeader*>(actor)->class_ptr != s_replay.camera_class)
        return false;
    const auto* hdr = static_cast<const UObjectHeader*>(actor);
    const auto* objects = GhostObjects();
    s_replay.camera = {actor, hdr->index, objects->objects[hdr->index].serial_number, {}};
    args.fill(0);
    std::memcpy(args.data() + api.finish_actor, &actor, sizeof(actor));
    std::memcpy(args.data() + api.finish_transform, &spawn, sizeof(spawn));
    g_process_event(api.gameplay, api.finish, args.data());
    if (!ReplayCameraLive()) return false;
    s_replay.move_fn = FindReflectedFunction(actor, L"K2_SetActorLocationAndRotation");
    if (!s_replay.move_fn ||
        !GhostParam(s_replay.move_fn, L"NewLocation", L"StructProperty", sizeof(Vec3), s_replay.move_location) ||
        !GhostParam(s_replay.move_fn, L"NewRotation", L"StructProperty", sizeof(Rotator), s_replay.move_rotation))
        return false;
    GhostParam(s_replay.move_fn, L"bTeleport", L"BoolProperty", 1, s_replay.move_teleport);
    GhostPlayback playback{};
    ghost_data::Sample first{};
    if (!SelectGhostPlayback(playback) ||
        !ghost_data::PositionAt(*playback.track, playback.elapsed_ms, first) ||
        !ReplayMoveCamera(first)) return false;
    s_replay.controller = player->controller;
    s_replay.previous_target = previous;
    s_replay.view_fn = set_view;
    s_replay.get_view_fn = get_view;
    s_replay.get_view_result = old_target_offset;
    if (!ReplaySetViewTarget(actor)) return false;
    s_replay.player = player;
    s_replay.active = true;
    s_replay_orbit_active.store(s_replay_camera_mode == 3,
                                std::memory_order_release);
    // A successful reflected call is not proof that the native controller
    // accepted the target. Reject failed handoffs and restore when possible.
    if (ReplayCurrentViewTarget() != actor) return false;
    s_replay.last_tick = GetTickCount64();
    Log(L"Ghost replay: spectator camera active; playback is independent of the run clock");
    return true;
}

void Rectangle(void* hud, float x, float y, float width, float height,
               float opacity) {
    if (g_draw_rect && width > 0 && height > 0 &&
        std::isfinite(x) && std::isfinite(y)) {
        LinearColor tint = GhostColourLinear();
        tint.a = s_transparency_2d ? opacity : 1.0f;
        g_draw_rect(hud, tint, x, y, width, height);
    }
}
} // namespace

bool IsGhostReplayRequested() { return s_replay.requested || s_replay.active; }
bool IsGhostReplayPaused() { return s_replay.paused; }
std::wstring GhostReplaySourceLabel() {
    LoadGhostSettingOnce();
    const bool fastest = IsGhostReplayRequested()
        ? s_replay.fastest : s_replay_best_segments;
    return fastest ? L"Best segments" : L"PB";
}
void ToggleGhostReplaySource() {
    LoadGhostSettingOnce();
    // Changing source while native buttons are pressed needs a fresh world.
    // Stop the old playback safely; the user can start the chosen source next.
    if (IsGhostReplayRequested()) StopGhostReplayInternal();
    s_replay_best_segments = !s_replay_best_segments;
    if (!g_settings_path.empty())
        QueueSettingsValue(L"Ghost", L"ReplayBestSegments",
                           s_replay_best_segments ? L"1" : L"0");
    Log((L"Ghost replay source: " + GhostReplaySourceLabel()).c_str());
}
std::wstring GhostReplayStatus() {
    const std::uint32_t elapsed = static_cast<std::uint32_t>(s_replay.elapsed_ms / 1000.0);
    const std::uint32_t total = s_replay.duration_ms / 1000;
    const auto fmt = [](std::uint32_t seconds) {
        return std::to_wstring(seconds / 60) + L":" +
            (seconds % 60 < 10 ? L"0" : L"") + std::to_wstring(seconds % 60);
    };
    if (s_replay.requested && s_replay.waiting_reset) return L"Restarting level...";
    if (s_replay.requested && !s_replay.active) return L"Preparing buttons, tiles and lifts...";
    if (!s_replay.active) return L"Stopped";
    return fmt(elapsed) + L" / " + fmt(total) +
        (s_replay.paused ? L" (paused)" : L"");
}
std::wstring GhostReplaySpeedLabel() {
    return s_replay.speed_index == 0 ? L"0.5x" :
           s_replay.speed_index == 2 ? L"2x" : L"1x";
}
bool IsGhostReplayFirstPerson() {
    LoadGhostSettingOnce();
    return s_replay_camera_mode == 1;
}
bool IsGhostReplayFirstPersonActive(const MyCharacter* player) {
    // Called on the game thread, after replay creation/cleanup for this tick.
    return s_replay.active && s_replay_camera_mode == 1 && player &&
           s_replay.player == player && s_replay.controller == player->controller;
}
bool IsGhostReplayFreeCameraActive() {
    return s_replay.active && s_replay_camera_mode == 2;
}
bool IsGhostReplayOrbitCameraActive() {
    return s_replay_orbit_active.load(std::memory_order_acquire);
}
void QueueGhostReplayOrbitWheel(short delta) {
    if (!delta) return;
    constexpr int kMaxQueuedDelta = 120 * 30;
    int current = s_replay_orbit_wheel_delta.load(std::memory_order_relaxed);
    for (;;) {
        const int next = std::clamp(current + static_cast<int>(delta),
                                    -kMaxQueuedDelta, kMaxQueuedDelta);
        if (s_replay_orbit_wheel_delta.compare_exchange_weak(
                current, next, std::memory_order_relaxed)) break;
    }
}
std::wstring GhostReplayCameraLabel() {
    LoadGhostSettingOnce();
    switch (s_replay_camera_mode) {
        case 1: return L"First person";
        case 2: return L"Free cam";
        case 3: return L"Third person";
        default: return L"Locked third person";
    }
}
void ToggleGhostReplayCameraMode() {
    LoadGhostSettingOnce();
    // Keep old stored mode IDs intact; change only the user-facing order.
    switch (s_replay_camera_mode) {
        case 0: s_replay_camera_mode = 3; break; // Locked -> orbit
        case 3: s_replay_camera_mode = 1; break; // Orbit -> first person
        case 1: s_replay_camera_mode = 2; break; // First person -> free cam
        default: s_replay_camera_mode = 0; break; // Free cam -> locked
    }
    s_replay.free_camera = {}; // Re-enter free cam at the current ghost pose.
    s_replay.orbit_camera = {}; // Orbit resets to follow, without snapping other modes.
    s_replay_orbit_active.store(s_replay.active && s_replay_camera_mode == 3,
                                std::memory_order_release);
    s_replay_orbit_wheel_delta.store(0, std::memory_order_relaxed);
    if (!g_settings_path.empty()) {
        QueueSettingsValue(L"Ghost", L"ReplayCameraMode",
                           s_replay_camera_mode == 0 ? L"0" :
                           s_replay_camera_mode == 1 ? L"1" :
                           s_replay_camera_mode == 2 ? L"2" : L"3");
    }
    Log((L"Ghost replay camera: " + GhostReplayCameraLabel()).c_str());
}
bool StartGhostReplayViewer() {
    if (s_replay.requested || s_replay.active) {
        StopGhostReplayInternal();
        return true;
    }
    if (s_loaded_profile != g_pb_path || CustomMapActive() || CustomMapApplying() ||
        g_practice_active || g_preloaded_restart_active || g_progress_restart_active ||
        g_restart_requested || g_practice_restart_requested ||
        g_full_level_reload_requested) return false;
    const LevelState* native = PracticeLevelState();
    if (!native || native->buttons < 0 || native->buttons > kRefunctButtonCount)
        return false;
    LoadGhostSettingOnce();
    const bool fastest = s_replay_best_segments;
    ghost_replay_progress::Timeline<kRefunctButtonCount> timeline{};
    if (!BuildGhostReplayButtonTimeline(fastest, timeline)) {
        Log(L"Ghost replay: saved ghost lacks matching button split timestamps");
        Notify(L"Ghost replay needs recorded button split times for this profile");
        return false;
    }
    s_replay = {};
    s_replay.requested = true;
    s_replay.fastest = fastest;
    s_replay.duration_ms = timeline.duration_ms;
    s_replay.button_timeline = timeline;
    s_replay.last_tick = GetTickCount64();
    s_replay.reset_requested_at = GetTickCount64();
    s_draw_cache = {};
    // Always start from a vanilla map. Even with native button count zero,
    // manual height overrides or disabled button gates can remain installed.
    // The standard R path resets those without modifying UObject internals.
    s_replay.waiting_reset = true;
    s_replay.owns_pending_restart = true;
    s_replay.resets_before_restart = native->resets;
    s_replay.player_before_restart = g_player;
    g_restart_requested = true;
    Log(L"Ghost replay: restarting level for real button-by-button playback");
    Notify(L"Ghost replay: restarting level before playback");
    return true;
}
void StopGhostReplayViewer() { if (IsGhostReplayRequested()) StopGhostReplayInternal(); }
void ToggleGhostReplayPause() {
    if (s_replay.active) {
        s_replay.paused = !s_replay.paused;
        s_replay.last_tick = GetTickCount64();
    }
}
// Rewind is a full native restart, not a fabricated decrement of LevelState.
void RewindGhostReplayTo(double target_ms) {
    if (!IsGhostReplayRequested()) return;
    const auto old = s_replay;
    StopGhostReplayInternal();
    s_replay = {};
    s_replay.requested = true;
    s_replay.fastest = old.fastest;
    s_replay.duration_ms = old.duration_ms;
    s_replay.button_timeline = old.button_timeline;
    s_replay.speed_index = old.speed_index;
    s_replay.resume_at_ms = std::clamp(target_ms, 0.0,
                                       static_cast<double>(old.duration_ms));
    s_replay.waiting_reset = true;
    s_replay.owns_pending_restart = true;
    if (const LevelState* native = PracticeLevelState())
        s_replay.resets_before_restart = native->resets;
    s_replay.player_before_restart = g_player;
    s_replay.reset_requested_at = GetTickCount64();
    g_restart_requested = true;
    Log(L"Ghost replay: restarting the native level to rewind button progression");
}
void SeekGhostReplaySeconds(int seconds) {
    if (!IsGhostReplayRequested()) return;
    if (s_replay.waiting_reset) {
        s_replay.resume_at_ms = std::clamp(s_replay.resume_at_ms + seconds * 1000.0,
            0.0, static_cast<double>(s_replay.duration_ms));
        s_replay.elapsed_ms = s_replay.resume_at_ms;
        return;
    }
    const double target = std::clamp(s_replay.elapsed_ms + seconds * 1000.0,
                                     0.0, static_cast<double>(s_replay.duration_ms));
    if (target + 1.0 < s_replay.elapsed_ms &&
        (s_replay.next_button > 0 || s_replay.awaiting_press ||
         s_replay.changed_native_progress || GhostReplayTilesChanged() ||
         GhostReplayLiftsChanged() || GhostReplayCubesChanged())) {
        RewindGhostReplayTo(target);
        return;
    }
    s_replay.elapsed_ms = target;
    s_replay_tile_pose_ready = false;
    s_replay.last_tick = GetTickCount64();
}
void RestartGhostReplay() {
    if (!IsGhostReplayRequested()) return;
    if (s_replay.waiting_reset) {
        s_replay.resume_at_ms = 0;
        s_replay.elapsed_ms = 0;
        s_replay.paused = false;
        return;
    }
    if (s_replay.next_button > 0 || s_replay.awaiting_press ||
        s_replay.changed_native_progress || GhostReplayTilesChanged() ||
        GhostReplayLiftsChanged() || GhostReplayCubesChanged()) {
        RewindGhostReplayTo(0.0);
        return;
    }
    s_replay.elapsed_ms = 0;
    s_replay_tile_pose_ready = false;
    s_replay.paused = false;
    s_replay.last_tick = GetTickCount64();
}
void CycleGhostReplaySpeed(int direction) {
    s_replay.speed_index = std::clamp(s_replay.speed_index + direction, 0, 2);
}
// Read the contact from the exact source segment and split deadline, not the
// current viewer clock (which may have overshot or been moved by seeking).
// In a stitched gold replay, the finishing pose belongs to the segment that
// just ended, not to the start of the following segment.
const std::vector<ghost_data::Sample>* ReplayButtonContactTrack(int split_index,
                                                                 std::uint32_t& at_ms) {
    if (split_index < 0 || split_index >= kRefunctButtonCount ||
        static_cast<std::size_t>(split_index) >= s_replay.button_timeline.count)
        return nullptr;
    const auto i = static_cast<std::size_t>(split_index);
    const auto* track = &s_playback;
    at_ms = s_replay.button_timeline.at_ms[i];
    if (s_replay.fastest) {
        const auto gold_duration = Milliseconds(g_pb.best_segment_seconds[i]);
        if (!s_fastest[i].empty() && gold_duration) {
            track = &s_fastest[i];
            at_ms = gold_duration;
        } else {
            at_ms = Milliseconds(g_pb.button_seconds[i]);
        }
    }
    return track;
}
bool ReplayButtonContactPose(int split_index, ghost_data::Sample& pose) {
    std::uint32_t at_ms = 0;
    const auto* track = ReplayButtonContactTrack(split_index, at_ms);
    if (!track) return false;
    // Legacy tracks can end slightly before a split; use the same bounded
    // tolerance as segment extraction. Never extrapolate a missing contact.
    if (!at_ms || track->empty() ||
        track->front().time_ms > at_ms + 75u ||
        track->back().time_ms + 75u < at_ms) return false;
    return ghost_data::PositionAt(*track, at_ms, pose) && ghost_data::Valid(pose);
}

int ResolveReplayButtonActorIndex(int split_index) {
    const auto section = SectionContainingButton(split_index + 1);
    if (!section.section || !g_practice_button_actors_ready) return -1;
    const int first = section.previous_end_button;
    const int end = section.end_button;
    int first_remaining = -1;
    int remaining = 0;
    for (int candidate = first; candidate < end; ++candidate) {
        if (s_replay.pressed_buttons[candidate]) continue;
        if (first_remaining < 0) first_remaining = candidate;
        ++remaining;
    }
    // New captures carry the actual actor index, not an inferred position.
    std::uint32_t at_ms = 0;
    if (const auto* track = ReplayButtonContactTrack(split_index, at_ms)) {
        const auto tag = ghost_data::ContactTagAt(*track, at_ms, split_index + 1);
        if (tag) {
            const int physical = static_cast<int>(tag & 255u) - 1;
            if (physical >= first && physical < end && !s_replay.pressed_buttons[physical]) {
                Log((L"Ghost replay: recorded native identity for split " +
                     std::to_wstring(split_index + 1) + L" = " +
                     std::to_wstring(physical + 1)).c_str());
                return physical;
            }
            // Different gold segments may contain the same actual button;
            // the native map cannot press it twice. Keep playback functional.
            Log(L"Ghost replay: incompatible recorded button identity in stitched route; using remaining actor");
        }
    }
    // The last unpressed actor is forced by native section progression.
    // It does not need a position lookup, even for independently saved golds.
    if (remaining <= 1) return first_remaining;

    ghost_data::Sample contact{};
    if (!ReplayButtonContactPose(split_index, contact)) {
        Log(L"Ghost replay: missing contact pose; using first remaining button in section");
        return first_remaining;
    }
    // Legacy recordings: resolve live world positions by actor, not by a
    // presumed match between array order and the static coordinate table.
    // Only at a multi-button contact, never every frame.
    std::array<Vec3, kRefunctButtonCount> centers{};
    bool live_centers = true;
    for (int candidate = first; candidate < end; ++candidate) {
        if (s_replay.pressed_buttons[candidate]) continue;
        if (!ResolveReplayButtonCenter(candidate + 1, centers[candidate]))
            live_centers = false;
    }
    if (live_centers) {
        for (int a = first; a < end; ++a) for (int b = a + 1; b < end; ++b) {
            if (s_replay.pressed_buttons[a] || s_replay.pressed_buttons[b]) continue;
            if (std::hypot(centers[a].x - centers[b].x,
                           centers[a].y - centers[b].y) < 50.f) live_centers = false;
        }
    }
    if (!live_centers)
        Log(L"Ghost replay: live button locations unavailable; legacy coordinate fallback");
    int best = first_remaining;
    double best_distance = std::numeric_limits<double>::infinity();
    double second_distance = best_distance;
    for (int candidate = first; candidate < end; ++candidate) {
        if (s_replay.pressed_buttons[candidate]) continue;
        const Vec3 center = live_centers ? centers[candidate] : PracticeSpawnForButton(candidate + 1);
        const double distance = std::hypot(static_cast<double>(contact.x) - center.x,
                                           static_cast<double>(contact.y) - center.y);
        if (!std::isfinite(distance)) continue;
        Log((L"Ghost replay legacy match: split=" + std::to_wstring(split_index + 1) +
             L" actor=" + std::to_wstring(candidate + 1) +
             L" source=" + (live_centers ? L"live" : L"static") +
             L" contact XY=" + std::to_wstring(contact.x) + L"," + std::to_wstring(contact.y) +
             L" actor XY=" + std::to_wstring(center.x) + L"," + std::to_wstring(center.y) +
             L" distance=" + std::to_wstring(distance)).c_str());
        if (distance < best_distance) {
            second_distance = best_distance;
            best_distance = distance;
            best = candidate;
        } else if (distance < second_distance) {
            second_distance = distance;
        }
    }
    // Old recordings lack explicit actor IDs and may have imprecise poses.
    // A weak match is diagnostic, not a reason to tear down the viewer.
    // Ties use the first remaining actor, preserving deterministic playback.
    if (best_distance > 300.0 || second_distance - best_distance < 50.0) {
        Log((L"Ghost replay: approximate physical button match for split " +
             std::to_wstring(split_index + 1) + L"; using remaining actor " +
             std::to_wstring(best + 1)).c_str());
    }
    return best;
}

// Advance exactly one native contact at a time and wait for Refunct's own
// LevelState.buttons confirmation. The sequence is also unit-tested against
// an in-memory native progress mock; no position-only fake button presses.
bool AdvanceReplayNativeButtons(MyCharacter* player,
                                const LevelState* progress,
                                std::uint64_t now) {
    // Never let replay-injected buttons affect PB or gold saving, even when
    // the user's Save With Cheats option allows ordinary assisted runs.
    g_recording_tainted = true;
    g_run_pb_eligible = false;
    // Mark once at activation; native button contacts do not use synthetic
    // input shortcuts or modify the PB/ghost files.
    if (!s_replay.marked_assisted) {
        MarkCheatAction(CheatAction::Buttons);
        s_replay.marked_assisted = true;
    }

    if (s_replay.awaiting_press) {
        if (progress->buttons == s_replay.next_button + 1) {
            ++s_replay.next_button;
            s_replay.awaiting_press = false;
            s_replay.changed_native_progress = true;
        } else if (progress->buttons != s_replay.next_button ||
                   now - s_replay.press_sent_at > 2500) {
            Log(L"Ghost replay: button did not confirm its native progress; stopping");
            return false;
        } else {
            // Hold the recorded clock at the contact until Refunct confirms
            // the section has been unlocked; avoid skipping the next button.
            s_replay.last_tick = now;
            return true;
        }
    }
    if (progress->buttons != s_replay.next_button) {
        Log(L"Ghost replay: unexpected native button progress; stopping");
        return false;
    }
    const auto passed = now >= s_replay.last_tick ?
        (std::min)(now - s_replay.last_tick, std::uint64_t{250}) : 0;
    s_replay.last_tick = now;
    if (!s_replay.paused) {
        s_replay.elapsed_ms = (std::min)(static_cast<double>(s_replay.duration_ms),
            s_replay.elapsed_ms + static_cast<double>(passed) * kReplaySpeeds[s_replay.speed_index]);
        if (s_replay.elapsed_ms >= s_replay.duration_ms) s_replay.paused = true;
    }
    if (s_replay.button_timeline.Due(static_cast<std::size_t>(s_replay.next_button),
                                      s_replay.elapsed_ms)) {
        const int button = s_replay.next_button;
        const int actor_index = ResolveReplayButtonActorIndex(button);
        if (actor_index < 0) {
            Log((L"Ghost replay: no remaining native button for split " +
                 std::to_wstring(button + 1) + L"; stopping").c_str());
            Notify(L"Replay stopped: native button cache or progression unavailable");
            return false;
        }
        if (!ActivatePracticeButton(actor_index, player)) {
            Log((L"Ghost replay: native button " + std::to_wstring(actor_index + 1) +
                 L" could not activate; stopping").c_str());
            return false;
        }
        s_replay.pressed_buttons[actor_index] = true;
        s_replay.changed_native_progress = true; // May already have changed synchronously.
        s_replay.awaiting_press = true;
        s_replay.press_sent_at = now;
        Log((L"Ghost replay: split " + std::to_wstring(button + 1) +
             L" activated physical button " + std::to_wstring(actor_index + 1)).c_str());
    }
    return true;
}

void UpdateGhostReplayViewer(MyCharacter* player) {
    if (!IsGhostReplayRequested()) return;
    if (!IsReadable(player, sizeof(MyCharacter)) ||
        !IsReadable(player->controller, sizeof(PlayerController)) ||
        CustomMapActive() || CustomMapApplying() ||
        g_practice_active || g_preloaded_restart_active || g_progress_restart_active ||
        s_loaded_profile != g_pb_path ||
        g_full_level_reload_requested || g_preloaded_restart_requested ||
        g_practice_restart_requested ||
        (g_restart_requested && !s_replay.waiting_reset) ||
        (s_replay.active && (s_replay.player != player ||
                              s_replay.controller != player->controller ||
                              !ReplayCameraLive()))) {
        StopGhostReplayInternal();
        Log(L"Ghost replay: stopped after world/player/profile change");
        return;
    }
    const LevelState* progress = PracticeLevelState();
    if (!progress) return;
    const auto now = GetTickCount64();
    if (s_replay.waiting_reset) {
        // RestartRun may return before UE recreates the level/character.
        // Never apply old button-actor pointers to a new world.
        if (!g_restart_requested && progress->buttons == 0 &&
            (progress->resets != s_replay.resets_before_restart ||
             player != s_replay.player_before_restart)) {
            s_replay.waiting_reset = false;
            s_replay.owns_pending_restart = false;
            s_replay.last_tick = now;
            s_replay.next_button = 0;
            s_replay.pressed_buttons.fill(false);
            s_replay.awaiting_press = false;
            s_replay.button_cache_ready = false;
            s_replay.class_scan_cursor = 0;
            // The timeline can be sought forward after starting: each native
            // button will be issued in order, one per confirmation cycle.
            s_replay.elapsed_ms = s_replay.resume_at_ms;
            Log(L"Ghost replay: fresh button-zero world ready for playback");
        } else if (now - s_replay.reset_requested_at > 12000) {
            Log(L"Ghost replay: reset did not reach native button zero; stopped");
            StopGhostReplayInternal();
        }
        return;
    }
    if (!s_replay.active) {
        if (progress->buttons != 0) {
            Log(L"Ghost replay: native progress was not zero when camera began");
            StopGhostReplayInternal();
            return;
        }
        // Discovery traverses the native object registry. Retry at most once
        // per 1.5s while the freshly restarted level finishes loading.
        if (!s_replay.button_cache_ready) {
            if (now < s_replay.next_button_discovery_at) return;
            if (!DiscoverPracticeButtons() || !g_practice_button_actors_ready ||
                !g_add_based_character || !g_remove_based_character) {
                s_replay.next_button_discovery_at = now + 1500;
                if (now - s_replay.reset_requested_at > 12000) {
                    Log(L"Ghost replay: native button actors never became available");
                    StopGhostReplayInternal();
                }
                return;
            }
            s_replay.button_cache_ready = true;
        }
        if (!ResolveWorldGhostAssets()) return;
        if (!FindReplayCameraClass()) {
            const auto* objects = GhostObjects();
            if (objects && s_replay.class_scan_cursor >= objects->num_elements) {
                Log(L"Ghost replay: native CameraActor class unavailable");
                StopGhostReplayInternal();
            }
            return;
        }
        const auto tiles = PrepareGhostReplayTiles(player);
        if (tiles == GhostReplayTileStatus::Pending) return;
        const auto lifts = PrepareGhostReplayLifts(player);
        if (lifts == GhostReplayLiftStatus::Pending) return;
        const auto cubes = PrepareGhostReplayCubes(player);
        if (cubes == GhostReplayCubeStatus::Pending) return;
        // Geometry failure should not terminate an otherwise usable replay.
        if (tiles == GhostReplayTileStatus::Unavailable)
            Log(L"Ghost replay: tile activation unavailable; button replay continues");
        if (!CreateReplayCamera(player)) {
            Log(L"Ghost replay: spectator camera handoff failed; stopped");
            StopGhostReplayInternal();
            return;
        }
        s_replay.last_tick = now;
        Log((L"Ghost replay: ready to press " +
             std::to_wstring(s_replay.button_timeline.count) +
             L" native buttons at their recorded times").c_str());
    }
    if (!AdvanceReplayNativeButtons(player, progress, now)) {
        StopGhostReplayInternal();
        return;
    }
    GhostPlayback playback{};
    ghost_data::Sample pose{};
    if (!SelectGhostPlayback(playback) ||
        !ghost_data::PositionAt(*playback.track, playback.elapsed_ms, pose) ||
        !ReplayMoveCamera(pose)) {
        Log(L"Ghost replay: camera position unavailable; restoring player view");
        StopGhostReplayInternal();
        return;
    }
    // The visual ghost has no collision. Reproduce native contacts only when
    // its recorded capsule feet sweep across a currently reachable tile.
    const Vec3 feet_pose{pose.x,pose.y,pose.z};
    const bool continuous = s_replay_tile_pose_ready &&
        s_replay_tile_track == playback.track &&
        playback.elapsed_ms >= s_replay_tile_track_ms &&
        playback.elapsed_ms - s_replay_tile_track_ms <= 300 &&
        s_replay_tile_last_ms <= static_cast<std::uint32_t>(s_replay.elapsed_ms) &&
        static_cast<std::uint32_t>(s_replay.elapsed_ms) - s_replay_tile_last_ms <= 300;
    TouchGhostReplayTiles(player,
        continuous ? s_replay_tile_last_pose : feet_pose, feet_pose, continuous);
    TouchGhostReplayCubes(player,
        continuous ? s_replay_tile_last_pose : feet_pose, feet_pose, continuous);
    UpdateGhostReplayLifts(player,
        continuous ? s_replay_tile_last_pose : feet_pose, feet_pose, continuous);
    s_replay_tile_last_pose = feet_pose;
    s_replay_tile_last_ms = static_cast<std::uint32_t>(s_replay.elapsed_ms);
    s_replay_tile_track = playback.track;
    s_replay_tile_track_ms = playback.elapsed_ms;
    s_replay_tile_pose_ready = true;
}

bool DrainFastestSegmentGhostWrites() {
    if (!s_fastest_writer) return true;
    constexpr DWORD kTimeoutMs = 10000;
    const DWORD status = WaitForSingleObject(s_fastest_writer->idle, kTimeoutMs);
    if (status == WAIT_OBJECT_0) return true;
    Log(L"ERROR: fastest-segment ghost writer did not drain; profile operation blocked");
    return false;
}
void CopyFastestSegmentGhosts(const std::wstring& destination_pb_path) {
    if (!g_save_fastest_ghosts || destination_pb_path.empty() ||
        !DrainFastestSegmentGhostWrites()) return;
    const auto target_folder = ghost_data_paths::ProfileSectionDir(g_storage_dir, destination_pb_path);
    if (target_folder.empty() || !EnsureDirectory(target_folder)) {
        Log(L"WARNING: unable to create destination gold ghost folder for PB snapshot");
        return;
    }
    for (int button = 1; button <= kRefunctButtonCount; ++button) {
        if (s_fastest[static_cast<size_t>(button - 1)].empty()) continue;
        const auto from = ReadGhostPath(FastestGhostPath(g_pb_path, button),
            ghost_data_paths::LegacyGold(g_storage_dir, g_pb_path, button),
            ghost_data_paths::LegacyInline(g_storage_dir, g_pb_path,
                L".gold" + (button < 10 ? std::wstring(L"0") : std::wstring{}) +
                std::to_wstring(button) + L".ghost"));
        const auto to = FastestGhostPath(destination_pb_path, button);
        if (GetFileAttributesW(from.c_str()) != INVALID_FILE_ATTRIBUTES &&
            !CopyFileW(from.c_str(), to.c_str(), TRUE))
            Log(L"WARNING: failed to copy fastest-segment ghost to PB snapshot");
    }
}
void QueueFastestSegmentGhost(int button, double started_at,
                              double ended_at, bool native_run) {
    if (button >= 1 && button <= kRefunctButtonCount) {
        s_fastest[static_cast<size_t>(button - 1)].clear();
        s_pending_fastest[static_cast<size_t>(button - 1)] = {};
        s_draw_cache = {};
    }
    if (!g_save_fastest_ghosts || !RecordsAllowedForSegments() ||
        !native_run || button < 1 || button > kRefunctButtonCount ||
        g_pb_path.empty() || !std::isfinite(started_at) ||
        !std::isfinite(ended_at) || started_at < 0.0 ||
        ended_at <= started_at || ended_at > 86400.0 ||
        g_practice_active || g_preloaded_restart_active || g_progress_restart_active)
        return;
    const std::uint32_t start_ms = Milliseconds(started_at);
    const std::uint32_t end_ms = Milliseconds(ended_at);
    if (end_ms <= start_ms + 49u) return;
    s_pending_fastest[static_cast<size_t>(button - 1)] = {true, start_ms, end_ms};
}

bool IsPBGhostEnabled() { LoadGhostSettingOnce(); return s_show; }
void SetPBGhostEnabled(bool enabled) {
    LoadGhostSettingOnce();
    if (s_show == enabled) return;
    s_show = enabled;
    if (!enabled) DestroyWorldGhostActors();
    if (!g_settings_path.empty())
        QueueSettingsValue(L"Ghost", L"ShowPBGhost", enabled ? L"1" : L"0");
}
bool IsPBGhostPassThroughEnabled() { LoadGhostSettingOnce(); return s_pass_through; }
void SetPBGhostPassThroughEnabled(bool enabled) {
    LoadGhostSettingOnce();
    if (s_pass_through == enabled) return;
    s_pass_through = enabled;
    if (!enabled) DestroyWorldGhostActors();
    s_draw_cache = {};
    if (!g_settings_path.empty())
        QueueSettingsValue(L"Ghost", L"PassThroughCamera", enabled ? L"1" : L"0");
}

uint32_t GetPBGhostColourRgb() {
    LoadGhostSettingOnce();
    return s_ghost_colour_rgb;
}
void SetPBGhostColourRgb(uint32_t rgb) {
    LoadGhostSettingOnce();
    if (rgb > 0xFFFFFFu || s_ghost_colour_rgb == rgb) return;
    s_ghost_colour_rgb = rgb;
    s_ghost_glass.warned_unavailable = false;
    // Existing MIDs are private to the ghost. Recreate only our visual actors,
    // using the proven native material initialization path; no replay changes.
    if (s_world_parts.count) DestroyWorldGhostActors();
    if (!g_settings_path.empty()) {
        const std::wstring value = std::to_wstring(rgb);
        QueueSettingsValue(L"Ghost", L"ColourRGB", value.c_str());
    }
}
bool IsPBGhost2DTransparencyEnabled() { LoadGhostSettingOnce(); return s_transparency_2d; }
void SetPBGhost2DTransparencyEnabled(bool enabled) {
    LoadGhostSettingOnce();
    if (s_transparency_2d == enabled) return;
    s_transparency_2d = enabled;
    if (!g_settings_path.empty())
        QueueSettingsValue(L"Ghost", L"Transparent2D", enabled ? L"1" : L"0");
}
bool IsPBGhost3DTransparencyEnabled() { LoadGhostSettingOnce(); return s_transparency_3d; }
void SetPBGhost3DTransparencyEnabled(bool enabled) {
    LoadGhostSettingOnce();
    if (s_transparency_3d == enabled) return;
    s_transparency_3d = enabled;
    s_ghost_glass.names_ready = false; // Opaque M_Basic only needs the Color name.
    s_ghost_glass.warned_unavailable = false;
    s_ghost_glass.warned_missing = false;
    s_ghost_glass.retry_after = 0;
    s_ghost_glass.scan_cursor = 0;
    s_ghost_glass.scan_complete = false;
    // MIDs are created once for each ghost limb. Respawn only our six ghost
    // actors so that opacity is reapplied without mutating shared materials.
    DestroyWorldGhostActors();
    if (!g_settings_path.empty())
        QueueSettingsValue(L"Ghost", L"Transparent3D", enabled ? L"1" : L"0");
}

bool HasPBGhostForActiveProfile() {
    if (!s_playback.empty() || !s_furthest.samples.empty()) return true;
    for (const auto& track : s_fastest) if (!track.empty()) return true;
    return false;
}
std::wstring PBGhostRecordingStatus() {
    const auto reached = s_furthest.reached;
    if (reached) return L"FURTHEST BUTTON " + std::to_wstring(reached);
    if (HasPBGhostForActiveProfile()) return L"AVAILABLE";
    return L"NO SAVED GHOST";
}
bool IsPBGhostButtonSyncEnabled() { LoadGhostSettingOnce(); return s_button_sync; }
void SetPBGhostButtonSyncEnabled(bool enabled) {
    LoadGhostSettingOnce();
    if (s_button_sync == enabled) return;
    s_button_sync = enabled;
    s_draw_cache = {}; // Synchronisation mode changes the entire playback clock.
    if (!g_settings_path.empty())
        QueueSettingsValue(L"Ghost", L"StartNextGhostOnButton",
                           enabled ? L"1" : L"0");
}
bool IsPBGhostFastestSectionsEnabled() {
    LoadGhostSettingOnce();
    return s_use_fastest_sections;
}
void SetPBGhostFastestSectionsEnabled(bool enabled) {
    LoadGhostSettingOnce();
    if (s_use_fastest_sections == enabled) return;
    s_use_fastest_sections = enabled;
    s_draw_cache = {}; // Different recording sources and clocks.
    if (!g_settings_path.empty())
        QueueSettingsValue(L"Ghost", L"UseFastestSections",
                           enabled ? L"1" : L"0");
}
void ResetPBGhostCapture() {
    s_pb_ghost_saved_for_latest_pb = false;
    DestroyWorldGhostActors();
    // Preserve reserved sample storage across restarts and profile switches.
    // Releasing and reallocating a growing vector mid-game causes frame spikes.
    s_capture.started = false;
    s_capture.complete = false;
    s_capture.usable = false;
    s_capture.overflow_reported = false;
    s_capture.start_clock = 0.0;
    s_capture.last_elapsed = 0.0;
    s_capture.samples.clear();
    s_pending_fastest.fill({});
    s_furthest_checked_button = 0;
}

void LoadPBGhostForActiveProfile() {
    StopGhostReplayViewer();
    LoadGhostSettingOnce();
    if (!DrainFastestSegmentGhostWrites()) {
        Log(L"WARNING: deferred ghost load until pending fastest recordings are saved");
        return;
    }
    StartFastestGhostWriter(); // start outside gameplay where practical
    ResetPBGhostCapture();
    // Preallocate before gameplay where possible; a bounded 2-hour track
    // occupies 2.75 MiB and then needs no sample-buffer reallocations.
    if (s_capture.samples.capacity() < ghost_data::kMaxSamples)
        s_capture.samples.reserve(ghost_data::kMaxSamples);
    s_playback.clear();
    for (auto& track : s_fastest) track.clear();
    s_furthest = {};
    s_primary_pb_protected = false;
    s_primary_ghost_invalid = false;
    s_draw_cache = {};
    s_loaded_profile = g_pb_path;
    if (!g_pb_path.empty() && !EnsureGhostSectionDirectory(g_pb_path))
        Log(L"WARNING: cannot create gold ghost directory for selected profile");
    // PB.ghost is read below as either a completed PB or an incomplete
    // furthest-run track. Legacy furthest files are consolidated first.
    // Fastest segments are independent of a completed PB. Read the matching
    // track for each recorded gold, including those from unfinished runs.
    if (!g_pb_path.empty()) {
        for (int button = 1; button <= kRefunctButtonCount; ++button) {
            const auto index = static_cast<size_t>(button - 1);
            const auto expected_ms = Milliseconds(g_pb.best_segment_seconds[index]);
            if (!expected_ms) continue;
            const auto path = ReadGhostPath(FastestGhostPath(g_pb_path, button),
            ghost_data_paths::LegacyGold(g_storage_dir, g_pb_path, button),
            ghost_data_paths::LegacyInline(g_storage_dir, g_pb_path,
                L".gold" + (button < 10 ? std::wstring(L"0") : std::wstring{}) +
                std::to_wstring(button) + L".ghost"));
            if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
            std::vector<std::uint8_t> bytes;
            if (!ReadGhostFile(path, bytes) ||
                !ghost_data::Deserialize(bytes, expected_ms, s_fastest[index]))
                Log((L"Fastest ghost: rejected stale/corrupt track for button " +
                     std::to_wstring(button)).c_str());
        }
    }
    if (g_pb_path.empty()) return;
    const auto primary = GhostPath(g_pb_path);
    // Older installations saved an additional furthest.ghost. Consolidate
    // only after validating the old file, and never overwrite a PB or other
    // existing primary file on a failed/inconclusive read.
    ConsolidateLegacyFurthestGhost(g_pb_path, Milliseconds(g_pb.total_seconds));
    const auto path = ReadGhostPath(primary,
        ghost_data_paths::LegacyPB(g_storage_dir, g_pb_path),
        ghost_data_paths::LegacyInline(g_storage_dir, g_pb_path, L".ghost"));
    if (!IsRegularFile(path)) {
        // Filesystem permissions can prevent a legacy move. Reading the old
        // partial recording is still safe; never overwrite its source.
        const auto legacy = ReadGhostPath(FurthestGhostPath(g_pb_path),
            ghost_data_paths::LegacyFurthest(g_storage_dir, g_pb_path),
            ghost_data_paths::LegacyInline(g_storage_dir, g_pb_path, L".furthest.ghost"));
        if (IsRegularFile(legacy)) {
            std::vector<std::uint8_t> old_bytes;
            if (ReadGhostFile(legacy, old_bytes,
                    ghost_furthest_data::kPrefixSize + kRefunctButtonCount * 4) &&
                ghost_furthest_data::Deserialize(old_bytes, s_furthest))
                Log(L"Ghost: loaded legacy incomplete recording (migration blocked)");
        }
        return;
    }
    std::vector<std::uint8_t> bytes;
    if (!ReadGhostFile(path, bytes,
            ghost_furthest_data::kPrefixSize + kRefunctButtonCount * 4)) {
        s_primary_ghost_invalid = true;
        Log(L"Ghost: could not read main recording; preserving it without overwrite");
        return;
    }
    if (IsCompletedGhostFormat(bytes)) {
        const auto pb_ms = Milliseconds(g_pb.total_seconds);
        s_primary_pb_protected = true; // Even stale PBs must not be clobbered.
        if (!pb_ms || !ghost_data::Deserialize(bytes, pb_ms, s_playback)) {
            s_primary_ghost_invalid = true;
            Log(L"PB ghost: stale/corrupt completed recording preserved");
            return;
        }
        Log((L"PB ghost: loaded " + std::to_wstring(s_playback.size()) +
             L" position samples for " + g_active_profile).c_str());
    } else if (ghost_furthest_data::Deserialize(bytes, s_furthest)) {
        Log((L"Ghost: loaded incomplete recording through button " +
             std::to_wstring(s_furthest.reached)).c_str());
    } else {
        s_primary_ghost_invalid = true;
        Log(L"Ghost: rejected invalid main recording; left file unchanged");
    }
}

void RenamePBGhostProfile(const std::wstring& old_pb_path,
                          const std::wstring& new_pb_path) {
    if (!DrainFastestSegmentGhostWrites()) return;
    // The caller moves the WHOLE Profiles/<name> directory atomically first,
    // including PB, furthest and every gold ghost. Only recover legacy files
    // left behind by a previous failed migration here.
    if (!EnsureGhostSectionDirectory(new_pb_path)) return;
    const auto move_if_present = [](const std::wstring& from, const std::wstring& to,
                                    const wchar_t* warning) {
        if (from.empty() || to.empty() || !IsRegularFile(from)) return;
        if (IsRegularFile(to) || !MoveFileW(from.c_str(), to.c_str())) Log(warning);
    };
    move_if_present(ghost_data_paths::LegacyPB(g_storage_dir, old_pb_path),
                    GhostPath(new_pb_path), L"WARNING: old PB ghost rename conflict");
    move_if_present(ghost_data_paths::LegacyFurthest(g_storage_dir, old_pb_path),
                    FurthestGhostPath(new_pb_path), L"WARNING: old furthest ghost rename conflict");
    move_if_present(ghost_data_paths::LegacyInline(g_storage_dir, old_pb_path, L".ghost"),
                    GhostPath(new_pb_path), L"WARNING: inline PB ghost rename conflict");
    move_if_present(ghost_data_paths::LegacyInline(g_storage_dir, old_pb_path, L".furthest.ghost"),
                    FurthestGhostPath(new_pb_path), L"WARNING: inline furthest ghost rename conflict");
    for (int button = 1; button <= kRefunctButtonCount; ++button) {
        const auto to = FastestGhostPath(new_pb_path, button);
        move_if_present(ghost_data_paths::LegacyGold(g_storage_dir, old_pb_path, button),
                        to, L"WARNING: legacy gold ghost rename conflict");
        const std::wstring suffix = L".gold" +
            (button < 10 ? std::wstring(L"0") : std::wstring{}) +
            std::to_wstring(button) + L".ghost";
        move_if_present(ghost_data_paths::LegacyInline(g_storage_dir, old_pb_path, suffix),
                        to, L"WARNING: inline gold ghost rename conflict");
    }
    s_loaded_profile = new_pb_path;
}

void CommitPendingFastestGhosts() {
    if (!s_capture.usable || s_capture.samples.size() < 2) return;
    for (int button = 1; button <= kRefunctButtonCount; ++button) {
        auto& pending = s_pending_fastest[static_cast<size_t>(button - 1)];
        if (!pending.ready) continue;
        pending.ready = false;
        // The player may have changed saving preferences or cheated since
        // this segment was queued. Never bypass current eligibility.
        if (!g_save_fastest_ghosts || !RecordsAllowedForSegments()) continue;
        const auto index = static_cast<size_t>(button - 1);
        const auto duration = pending.end_ms - pending.start_ms;
        const auto stored = Milliseconds(g_pb.best_segment_seconds[index]);
        // A later faster record or profile change must never be overwritten
        // with a stale movement track.
        if (stored == 0 || (stored > duration ? stored - duration : duration - stored) > 2u ||
            s_loaded_profile != g_pb_path) continue;
        std::vector<ghost_data::Sample> track;
        if (!ghost_data::ExtractSegment(s_capture.samples,
                                        pending.start_ms, pending.end_ms, track)) continue;
        // Preserve the observed finishing contact even if split rounding puts
        // the native event a few milliseconds from the extracted endpoint.
        if (const auto tag = ghost_data::ContactTagAt(s_capture.samples,
                                                       pending.end_ms, button))
            track.back().contact = tag;
        FastestGhostWrite job{};
        job.path = FastestGhostPath(g_pb_path, button);
        job.duration = duration;
        job.track = track; // Worker owns its own stable copy; playback can change.
        if (!QueueFastestGhostJob(std::move(job))) {
            Log(L"ERROR: fastest-segment ghost could not be queued for saving");
            continue;
        }
        s_fastest[index] = std::move(track);
        s_draw_cache = {};
    }
}

// Save at the native button contact, not at the end of a completed run.
// Never use a timing-only gold to invent movement; all positions come from
// this run's actual player capture. The farthest button wins, with faster
// arrival time breaking ties.
void CommitFurthestGhostAtButton() {
    const int button = g_last_button_count;
    if (button <= s_furthest_checked_button || button < 1 ||
        button > kRefunctButtonCount || g_pb_path.empty() ||
        !s_capture.started || !s_capture.usable ||
        s_capture.samples.size() < 2 || !g_run_started_at_zero) return;
    // Never replace a completed PB ghost or an unreadable existing file.
    if (s_primary_pb_protected || s_primary_ghost_invalid) {
        s_furthest_checked_button = button;
        return;
    }
    // Furthest-run ghosts follow the full-run ghost toggle and PB cheat
    // permission, rather than bypassing Save with cheats on button contact.
    if (!g_save_pb_ghosts || !RecordsAllowedForPB()) {
        s_furthest_checked_button = button;
        return;
    }
    // An untimed button jump cannot become a legitimate replay; stop trying
    // it every frame so an invalid edge never causes a sampling hitch.
    if (g_button_splits.size() < static_cast<size_t>(button)) {
        s_furthest_checked_button = button;
        return;
    }
    ghost_furthest_data::Run<kRefunctButtonCount> candidate;
    candidate.reached = static_cast<std::uint32_t>(button);
    std::uint32_t previous = 0;
    for (int i = 0; i < button; ++i) {
        const auto& split = g_button_splits[static_cast<size_t>(i)];
        if (split.button_number != i + 1) {
            s_furthest_checked_button = button;
            return;
        }
        const auto elapsed = Milliseconds(split.run_seconds);
        if (elapsed <= previous) {
            s_furthest_checked_button = button;
            return;
        }
        candidate.button_ms[static_cast<size_t>(i)] = elapsed;
        previous = elapsed;
    }
    const bool improved = candidate.reached > s_furthest.reached ||
        (candidate.reached == s_furthest.reached &&
         (!s_furthest.button_ms[candidate.reached - 1] ||
          previous < s_furthest.button_ms[candidate.reached - 1]));
    if (!improved) {
        s_furthest_checked_button = button;
        return;
    }
    if (!ghost_furthest_data::ExtractObservedPrefix(
            s_capture.samples, previous, candidate.samples)) {
        // The native contact timestamp can be slightly ahead of the latest
        // player sample. Retry briefly instead of losing this entire run.
        if (s_capture.samples.back().time_ms + 75u < previous &&
            previous - s_capture.samples.back().time_ms <= 2000u) return;
        s_furthest_checked_button = button;
        Log(L"WARNING: furthest ghost contact lacked a valid movement prefix");
        return;
    }
    FastestGhostWrite job{};
    job.path = GhostPath(g_pb_path);
    job.furthest = candidate;
    if (!QueueFastestGhostJob(std::move(job))) {
        s_furthest_checked_button = button;
        Log(L"ERROR: furthest-run ghost could not be saved");
        return;
    }
    s_furthest_checked_button = button;
    s_furthest = std::move(candidate);
    s_draw_cache = {};
    Log((L"Furthest ghost: recorded through button " +
         std::to_wstring(button) + L" for " + g_active_profile).c_str());
}

// Called only on a confirmed native progress edge. Never serialize a UObject
// address: the physical actor slot comes from the owning level's Buttons array.
void RecordPBGhostButtonContact(const MyCharacter* player, const void* actor,
                               int split, double run_start, double elapsed) {
    if (!s_capture.started || !s_capture.usable || s_capture.complete ||
        !actor || !player || !g_practice_button_actors_ready ||
        IsGhostReplayRequested() || CustomMapActive() || CustomMapApplying() ||
        std::abs(s_capture.start_clock - run_start) > 0.001 ||
        !std::isfinite(elapsed) || elapsed < 0 || elapsed > 86400.0) return;
    const auto section = SectionContainingButton(split);
    if (!section.section) return;
    int physical = 0;
    for (int i = section.previous_end_button; i < section.end_button; ++i) {
        if (g_practice_button_actors[i] == actor) { physical = i + 1; break; }
    }
    const auto tag = ghost_data::ButtonContactTag(split, physical);
    if (!tag || !IsReadable(player->root_component, sizeof(SceneComponent)) ||
        !IsReadable(player->controller, sizeof(PlayerController))) return;
    const auto ms = Milliseconds(elapsed);
    const auto pos = player->root_component->location;
    const auto rot = player->controller->control_rotation;
    const ghost_data::Sample contact{ms, pos.x, pos.y, pos.z,
        rot.yaw, std::remainder(rot.pitch, 360.f), tag};
    if (!ghost_data::Valid(contact)) return;
    auto& samples = s_capture.samples;
    if (!samples.empty() && samples.back().time_ms >= ms) {
        // The normal recorder and contact hook can share a millisecond.
        // Do not invent timestamps, overwrite another event or reorder poses.
        if (samples.back().time_ms == ms && !samples.back().contact)
            samples.back() = contact;
        else return;
    } else {
        if (samples.size() >= ghost_data::kMaxSamples) return;
        samples.push_back(contact);
    }
    // Only once per contact, useful for verifying physical identity in-game.
    Log((L"Ghost capture: split " + std::to_wstring(split) +
         L" physical button " + std::to_wstring(physical)).c_str());
}

void UpdatePBGhostRecording(const MyCharacter* player) {
    if (g_button_state_initialized && !g_run_started_at_zero) {
        ResetPBGhostCapture();
        return;
    }
    if (!g_engine_timer_valid || !g_game_data_valid || !g_button_state_initialized ||
        !player || !std::isfinite(g_last_run_start) ||
        !std::isfinite(g_elapsed_seconds)) return;
    // Track from native button zero. A late attachment must not silently create
    // a 'full run' replay from halfway through the course.
    const bool new_run = !s_capture.started ||
        std::abs(s_capture.start_clock - g_last_run_start) > 0.001 ||
        g_elapsed_seconds + 0.25 < s_capture.last_elapsed;
    if (new_run) {
        ResetPBGhostCapture();
        s_capture.started = true;
        s_capture.start_clock = g_last_run_start;
        s_capture.usable = g_elapsed_seconds <= 0.5 && g_last_button_count == 0 &&
            !g_practice_active &&
            !g_preloaded_restart_active && !g_progress_restart_active &&
            !CustomMapActive() && !CustomMapApplying();
        if (s_capture.usable &&
            s_capture.samples.capacity() < ghost_data::kMaxSamples) {
            // One bounded (~3.85 MiB) allocation at first run, retained for
            // future runs. No vector growth, copies, or heap calls at 20 Hz.
            s_capture.samples.reserve(ghost_data::kMaxSamples);
        }
    }
    s_capture.last_elapsed = g_elapsed_seconds;
    if (g_practice_active || g_preloaded_restart_active || g_progress_restart_active ||
        CustomMapActive() || CustomMapApplying()) s_capture.usable = false;
    if (!s_capture.usable || s_capture.complete) return;
    bool pending_contact = g_last_button_count > s_furthest_checked_button &&
        g_last_button_count > 0;
    for (const auto& gold : s_pending_fastest) {
        if (gold.ready) { pending_contact = true; break; }
    }
    const std::uint32_t now_ms = Milliseconds(g_elapsed_seconds);
    if (!now_ms && g_elapsed_seconds != 0.0) return;
    if (!s_capture.samples.empty()) {
        if (now_ms <= s_capture.samples.back().time_ms) {
            CommitPendingFastestGhosts();
            CommitFurthestGhostAtButton();
            if (g_timer_complete) s_capture.complete = true;
            return;
        }
        if (!g_timer_complete && !pending_contact &&
            now_ms - s_capture.samples.back().time_ms < ghost_data::kSamplingIntervalMs)
            return;
    }
    if (s_capture.samples.size() >= ghost_data::kMaxSamples) {
        s_capture.usable = false;
        if (!s_capture.overflow_reported) {
            Log(L"PB ghost: run too long to capture with bounded sample storage");
            s_capture.overflow_reported = true;
        }
        return;
    }
    // The majority of character ticks do not require a ghost sample. Only
    // validate UE4 pointers on 20-Hz sample frames or the final finish frame.
    if (!IsReadable(player->root_component, sizeof(SceneComponent)) ||
        !IsReadable(player->controller, sizeof(PlayerController))) return;
    const Vec3 position = player->root_component->location;
    const Rotator rotation = player->controller->control_rotation;
    const ghost_data::Sample sample{now_ms, position.x, position.y, position.z,
        rotation.yaw, std::remainder(rotation.pitch, 360.f)};
    if (!ghost_data::Valid(sample)) { s_capture.usable = false; return; }
    s_capture.samples.push_back(sample);
    CommitPendingFastestGhosts();
    CommitFurthestGhostAtButton();
    if (g_timer_complete) s_capture.complete = true;
}

bool PBGhostSavedForLatestPB() { return s_pb_ghost_saved_for_latest_pb; }
void CommitPBGhostOnNewPB(double run_seconds) {
    s_pb_ghost_saved_for_latest_pb = false;
    if (!g_save_pb_ghosts || !RecordsAllowedForPB()) {
        s_playback.clear(); // Prevent replaying the previous PB as the new one.
        s_draw_cache = {};
        return;
    }
    const std::uint32_t pb_ms = Milliseconds(run_seconds);
    // PB file has already been saved by records.cpp. Ghost failure cannot
    // roll back or modify any legitimate PB timing data.
    if (!s_capture.started || !s_capture.usable || !s_capture.complete ||
        !ghost_data::ValidTrack(s_capture.samples, pb_ms) ||
        g_practice_active || g_preloaded_restart_active || g_progress_restart_active ||
        CustomMapActive() || CustomMapApplying() || g_pb_path.empty()) {
        Log(L"PB ghost: new PB saved, but no complete vanilla ghost capture available");
        s_playback.clear();
        return;
    }
    // A pending partial-run write uses this same file. Drain it before
    // committing the completed PB, otherwise the background worker could
    // overwrite the PB with an older partial recording.
    if (!DrainFastestSegmentGhostWrites()) {
        s_playback.clear();
        Log(L"PB ghost: pending partial recording could not drain");
        return;
    }
    std::vector<std::uint8_t> bytes;
    if (!ghost_data::Serialize(s_capture.samples, pb_ms, bytes) ||
        !WriteGhostFile(GhostPath(g_pb_path), bytes)) {
        Log(L"PB ghost: could not persist new PB ghost track");
        s_playback.clear();
        return;
    }
    s_playback = s_capture.samples;
    s_furthest = {}; // Completed PB replaces the incomplete-run recording.
    s_primary_pb_protected = true;
    s_primary_ghost_invalid = false;
    s_pb_ghost_saved_for_latest_pb = true;
    s_draw_cache = {};
    Log((L"PB ghost: saved " + std::to_wstring(s_playback.size()) +
         L" samples for " + g_active_profile).c_str());
}

// Spawn one ghost limb per character tick to avoid a multi-actor hitch. Once
// all six parts exist, UE4 updates the renderable actor transforms normally.
void DrawWorldPBGhost(MyCharacter* player) {
    if ((!s_replay.requested && !s_replay.active &&
         (!IsPBGhostPassThroughEnabled() || !IsPBGhostEnabled())) ||
        !IsReadable(player, sizeof(MyCharacter)) ||
        !IsReadable(player->root_component, sizeof(SceneComponent))) {
        if (s_world_parts.count) DestroyWorldGhostActors();
        return;
    }
    GhostPlayback playback{};
    if (!SelectGhostPlayback(playback)) {
        if (s_world_parts.count) DestroyWorldGhostActors();
        return;
    }
    ghost_data::Sample sample{};
    if (!ghost_data::PositionAt(*playback.track, playback.elapsed_ms, sample) ||
        !std::isfinite(sample.yaw)) return;
    if (s_world_parts.player && s_world_parts.player != player)
        DestroyWorldGhostActors();
    if (!ResolveWorldGhostAssets()) return;
    if (!s_world_parts.player) s_world_parts.player = player;
    const auto* objects = GhostObjects();
    for (int i = 0; i < s_world_parts.count; ++i) {
        if (!GhostLive(objects, s_world_parts.parts[static_cast<size_t>(i)])) {
            DestroyWorldGhostActors();
            return;
        }
    }
    if (s_world_parts.count < static_cast<int>(s_world_parts.parts.size())) {
        const int index = s_world_parts.count;
        if (!SpawnWorldGhostPart(player, sample, static_cast<size_t>(index))) {
            DestroyWorldGhostActors();
            s_world_api.retry_after = GetTickCount64() + 3000;
            return;
        }
        ++s_world_parts.count;
    }
    if (s_world_parts.count == static_cast<int>(s_world_parts.parts.size())) {
        ApplyGhostGlassMaterial(); // Per tick, use glass or a genuinely opaque shader.
        // Keep six UE actors at native render rate, but do not issue six
        // ProcessEvent transform writes for a frozen/unchanged replay pose.
        // No interpolation or sampling cadence is changed.
        if (!s_world_pose_valid || s_world_pose.x != sample.x ||
            s_world_pose.y != sample.y || s_world_pose.z != sample.z ||
            s_world_pose.yaw != sample.yaw) {
            PlaceWorldGhost(sample);
            if (!s_world_parts.player) return; // Placement failed and cleared actors.
            s_world_pose = sample;
            s_world_pose_valid = true;
        }
        // A camera at the recorded capsule's eye level would otherwise sit
        // inside the ghost's head, producing obstructive near-clipped geometry.
        // Change visibility only on spawn or camera-mode transitions. Keep
        // all six actors for instant third-person switching; no recording,
        // material or gameplay object is affected.
        const bool hide_for_first_person = s_replay.active && s_replay_camera_mode == 1;
        if (!s_world_parts_ready || s_world_parts_hidden != hide_for_first_person) {
            for (const auto& part : s_world_parts.parts) {
                if (!GhostCallBool(part.actor, L"SetActorHiddenInGame", L"bNewHidden",
                                   hide_for_first_person ? 1 : 0)) {
                    WorldGhostSpawnFailed(L"updating replay ghost visibility");
                    DestroyWorldGhostActors();
                    s_world_api.retry_after = GetTickCount64() + 3000;
                    return;
                }
            }
            s_world_parts_hidden = hide_for_first_person;
            const bool first_ready = !s_world_parts_ready;
            s_world_parts_ready = true;
            if (first_ready) Log(L"Ghost 3D: six world-space StaticMeshActors initialized");
        }
    }
}

void DrawPBGhost(void* hud, int32_t viewport_width, int32_t viewport_height) {
    // 3D mode never projects geometry into the HUD. Do not draw a fallback
    // billboard when the native 3D renderer is unavailable.
    if (s_replay.requested || s_replay.active || IsPBGhostPassThroughEnabled() || !g_draw_rect || !g_player ||
        !IsReadable(g_player, sizeof(MyCharacter)) ||
        !IsReadable(g_player->controller, sizeof(PlayerController))) return;
    GhostPlayback playback{};
    if (!SelectGhostPlayback(playback)) { s_draw_cache = {}; return; }
    auto* controller = g_player->controller;
    const bool button_synced = IsPBGhostButtonSyncEnabled();
    const bool fastest_sections = IsPBGhostFastestSectionsEnabled();
    const std::uint32_t playback_ms = playback.elapsed_ms;
    const auto* track = playback.track;
    const bool fastest_source = playback.fastest;
    const bool furthest_source = playback.furthest;
    const int button_count = button_synced ? g_last_button_count : -1;
    const std::uint64_t now_ms = static_cast<std::uint64_t>(GetTickCount64());
    const bool use_cache = s_draw_cache.populated &&
        s_draw_cache.controller == controller &&
        s_draw_cache.button_synced == button_synced &&
        s_draw_cache.fastest_sections == fastest_sections &&
        s_draw_cache.track == track &&
        s_draw_cache.button_count == button_count &&
        s_draw_cache.fastest_source == fastest_source &&
        s_draw_cache.furthest_source == furthest_source &&
        s_draw_cache.viewport_width == viewport_width &&
        s_draw_cache.viewport_height == viewport_height &&
        now_ms >= s_draw_cache.clock_ms &&
        now_ms - s_draw_cache.clock_ms < kProjectionCacheDurationMs &&
        playback_ms >= s_draw_cache.elapsed_ms &&
        playback_ms - s_draw_cache.elapsed_ms <= 25;
    if (!use_cache) {
        s_draw_cache = {};
        s_draw_cache.controller = controller;
        s_draw_cache.viewport_width = viewport_width;
        s_draw_cache.viewport_height = viewport_height;
        s_draw_cache.clock_ms = now_ms;
        s_draw_cache.elapsed_ms = playback_ms;
        s_draw_cache.button_synced = button_synced;
        s_draw_cache.fastest_sections = fastest_sections;
        s_draw_cache.track = track;
        s_draw_cache.button_count = button_count;
        s_draw_cache.fastest_source = fastest_source;
        s_draw_cache.furthest_source = furthest_source;
        s_draw_cache.populated = true;
        ghost_data::Sample point{};
        if (ghost_data::PositionAt(*track, playback_ms, point)) {
            // Ghost tracks store the player capsule's centre, not the feet.
            // Refunct's normal standing capsule half-height is 88 UE units
            // (also used by the checkpoint positioning code). Anchor the
            // hologram to both ends of that capsule, without changing the
            // recorded transforms or replay format.
            constexpr float kStandingCapsuleHalfHeight = 88.f;
            const Vec3 foot{point.x, point.y, point.z - kStandingCapsuleHalfHeight};
            const Vec3 head{point.x, point.y, point.z + kStandingCapsuleHalfHeight};
            s_draw_cache.visible =
                ProjectPosition(controller, foot, viewport_width, viewport_height,
                                s_draw_cache.fx, s_draw_cache.fy) &&
                ProjectPosition(controller, head, viewport_width, viewport_height,
                                s_draw_cache.hx, s_draw_cache.hy);
        }
    }
    if (!s_draw_cache.visible) return;
    const float fx = s_draw_cache.fx, fy = s_draw_cache.fy;
    const float hx = s_draw_cache.hx, hy = s_draw_cache.hy;
    const float height = std::clamp(std::abs(fy - hy), 9.f, 240.f);
    if (fy <= hy || std::abs(fx - hx) > height) return;
    const float cx = (hx + fx) * 0.5f;
    const float unit = height / 12.f;
    // Lightweight non-colliding hologram: head, torso, arms, legs. This is a
    // screen-space rendering of the recorded 3D pose, not a spawned UE4 pawn.
    Rectangle(hud, cx - unit, hy, 2.f*unit, 2.f*unit, 0.62f);
    Rectangle(hud, cx - 1.35f*unit, hy + 2.3f*unit, 2.7f*unit, 5.f*unit, 0.36f);
    Rectangle(hud, cx - 2.3f*unit, hy + 2.7f*unit, unit, 4.2f*unit, 0.26f);
    Rectangle(hud, cx + 1.3f*unit, hy + 2.7f*unit, unit, 4.2f*unit, 0.26f);
    Rectangle(hud, cx - 1.15f*unit, hy + 7.4f*unit, unit, 4.6f*unit, 0.34f);
    Rectangle(hud, cx + 0.15f*unit, hy + 7.4f*unit, unit, 4.6f*unit, 0.34f);
}
} // namespace refunct

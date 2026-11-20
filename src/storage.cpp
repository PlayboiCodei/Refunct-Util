#include "refunct/crosshair.hpp"
#include "refunct/internal_api.hpp"
#include "refunct/performance.hpp"
#include "refunct/ghost_data_paths.hpp"
#include "refunct/runtime_logs.hpp"
#include <new>
#include <cwchar>

namespace refunct {
// Store all user configuration beneath Refunct's Steam installation, not AppData.
// Profile records: RefunctUtility\\Profiles\\Any%\\Splits\\records.ini
bool IsRegularFile(const std::wstring& path) {
    const DWORD flags = GetFileAttributesW(path.c_str());
    return flags != INVALID_FILE_ATTRIBUTES && !(flags & FILE_ATTRIBUTE_DIRECTORY);
}

bool EnsureDirectory(const std::wstring& path) {
    if (CreateDirectoryW(path.c_str(), nullptr)) return true;
    const DWORD flags = GetFileAttributesW(path.c_str());
    return flags != INVALID_FILE_ATTRIBUTES && (flags & FILE_ATTRIBUTE_DIRECTORY);
}

// All new writes use Profiles/<name>; legacy files are read only when
// startup migration could not move them (e.g. antivirus or access denied).
std::wstring ProfilePath(const std::wstring& name) {
    if (g_storage_dir.empty() || !ValidProfileName(name)) return L"";
    const auto current = ghost_data_paths::Records(g_storage_dir, name);
    if (IsRegularFile(current)) return current;
    const auto old = ghost_data_paths::LegacySplit(g_storage_dir, name);
    return IsRegularFile(old) ? old : current;
}

bool EnsureProfileDirectories(const std::wstring& name) {
    if (!ValidProfileName(name) || g_storage_dir.empty()) return false;
    return EnsureDirectory(ghost_data_paths::ProfilesRoot(g_storage_dir)) &&
        EnsureDirectory(ghost_data_paths::ProfileDir(g_storage_dir, name)) &&
        EnsureDirectory(ghost_data_paths::SplitsDir(g_storage_dir, name)) &&
        EnsureDirectory(ghost_data_paths::GhostDir(g_storage_dir, name)) &&
        EnsureDirectory(ghost_data_paths::ProfileSectionDir(
            g_storage_dir, ghost_data_paths::Records(g_storage_dir, name)));
}

bool EnsureGhostSectionDirectory(const std::wstring& ini) {
    return EnsureProfileDirectories(ghost_data_paths::ProfileName(ini));
}

namespace {
struct MigrationCount { unsigned moved = 0, failed = 0, conflicts = 0; };
// On a conflict the existing new-format file wins; the old file remains
// unchanged for manual inspection. Win32 MoveFileW never overwrites the target.
void MoveLegacyFile(const std::wstring& from, const std::wstring& to,
                    MigrationCount& count) {
    if (from.empty() || to.empty() || !IsRegularFile(from) || from == to) return;
    if (IsRegularFile(to)) { ++count.conflicts; return; }
    if (MoveFileW(from.c_str(), to.c_str())) ++count.moved;
    else ++count.failed;
}
std::vector<std::wstring> EnumerateFiles(const std::wstring& folder,
                                         const wchar_t* pattern) {
    std::vector<std::wstring> results;
    if (folder.empty()) return results;
    WIN32_FIND_DATAW item{};
    HANDLE handle = FindFirstFileW((folder + L"\\" + pattern).c_str(), &item);
    if (handle == INVALID_HANDLE_VALUE) return results;
    do {
        if (!(item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            results.emplace_back(item.cFileName);
    } while (FindNextFileW(handle, &item));
    FindClose(handle);
    return results;
}
std::vector<std::wstring> EnumerateFolders(const std::wstring& folder) {
    std::vector<std::wstring> results;
    if (folder.empty()) return results;
    WIN32_FIND_DATAW item{};
    HANDLE handle = FindFirstFileW((folder + L"\\*").c_str(), &item);
    if (handle == INVALID_HANDLE_VALUE) return results;
    do {
        if ((item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            wcscmp(item.cFileName, L".") && wcscmp(item.cFileName, L".."))
            results.emplace_back(item.cFileName);
    } while (FindNextFileW(handle, &item));
    FindClose(handle);
    return results;
}
int ParseGoldNumber(const std::wstring& filename) {
    if (filename.size() != 12 || filename.compare(0, 4, L"gold") ||
        _wcsicmp(filename.c_str() + 6, L".ghost") != 0 ||
        filename[4] < L'0' || filename[4] > L'9' ||
        filename[5] < L'0' || filename[5] > L'9') return 0;
    const int button = (filename[4] - L'0') * 10 + filename[5] - L'0';
    return button >= 1 && button <= kRefunctButtonCount &&
           (button >= 10 || filename[4] == L'0') ? button : 0;
}
void MigrateOneLegacyGhost(const std::wstring& name, const std::wstring& suffix,
                           const std::wstring& source, MigrationCount& count) {
    if (!ValidProfileName(name)) return;
    const auto ini = ghost_data_paths::Records(g_storage_dir, name);
    // If two different INIs claim the same profile name, never silently
    // combine their ghost recordings. Leave old files for manual resolution.
    if (IsRegularFile(ini) &&
        IsRegularFile(ghost_data_paths::LegacySplit(g_storage_dir, name))) {
        ++count.conflicts;
        return;
    }
    std::wstring target;
    if (_wcsicmp(suffix.c_str(), L"ghost") == 0)
        target = ghost_data_paths::PB(g_storage_dir, ini);
    else if (_wcsicmp(suffix.c_str(), L"furthest.ghost") == 0)
        target = ghost_data_paths::Furthest(g_storage_dir, ini);
    else if (suffix.size() == 12 && _wcsnicmp(suffix.c_str(), L"gold", 4) == 0) {
        const int number = ParseGoldNumber(suffix);
        if (number) target = ghost_data_paths::Gold(g_storage_dir, ini, number);
    }
    if (target.empty()) return;
    if (!EnsureProfileDirectories(name)) { ++count.failed; return; }
    MoveLegacyFile(source, target, count);
}
} // namespace

// Startup-only, repeatable and no-overwrite migration. Every old filename is
// recognized precisely; unknown files are retained where they were.
void MigrateLegacyGhostFiles() {
    if (g_storage_dir.empty()) return;
    MigrationCount count{};
    // First migrate old split records so profile discovery sees their new home.
    for (const auto& file : EnumerateFiles(g_splits_dir, L"*.ini")) {
        if (file.size() <= 4 || _wcsicmp(file.c_str() + file.size() - 4, L".ini")) continue;
        const auto name = file.substr(0, file.size() - 4);
        if (!ValidProfileName(name)) continue;
        if (!EnsureProfileDirectories(name)) { ++count.failed; continue; }
        const auto old = ghost_data_paths::LegacySplit(g_storage_dir, name);
        const auto now = ghost_data_paths::Records(g_storage_dir, name);
        // A conflicting old INI belongs to a different record. Never attach
        // its backup to the new record when a destination already exists.
        const bool conflict = IsRegularFile(old) && IsRegularFile(now);
        MoveLegacyFile(old, now, count);
        if (!conflict && IsRegularFile(now))
            MoveLegacyFile(old + L".bak", now + L".bak", count);
    }
    // Recover a backup left behind by an interrupted earlier migration. Never
    // mix it with a still-existing legacy INI that conflicts with the new one.
    for (const auto& file : EnumerateFiles(g_splits_dir, L"*.ini.bak")) {
        if (file.size() <= 8 || _wcsicmp(file.c_str() + file.size() - 8, L".ini.bak")) continue;
        const auto name = file.substr(0, file.size() - 8);
        if (!ValidProfileName(name)) continue;
        const auto old = ghost_data_paths::LegacySplit(g_storage_dir, name);
        const auto now = ghost_data_paths::Records(g_storage_dir, name);
        if (IsRegularFile(old) || !IsRegularFile(now)) continue;
        if (!EnsureProfileDirectories(name)) { ++count.failed; continue; }
        MoveLegacyFile(old + L".bak", now + L".bak", count);
    }
    // Previous releases stored full ghosts in a shared Ghosts folder.
    const auto old_ghosts = ghost_data_paths::LegacyGhostRoot(g_storage_dir);
    for (const auto& file : EnumerateFiles(old_ghosts, L"*.ghost")) {
        if (file.size() <= 6 || _wcsicmp(file.c_str() + file.size() - 6, L".ghost")) continue;
        std::wstring name = file.substr(0, file.size() - 6);
        std::wstring suffix = L"ghost";
        if (name.size() > 9 && _wcsicmp(name.c_str() + name.size() - 9, L".furthest") == 0) {
            name.resize(name.size() - 9);
            suffix = L"furthest.ghost";
        }
        MigrateOneLegacyGhost(name, suffix, old_ghosts + L"\\" + file, count);
    }
    // Previous gold ghosts lived in Ghosts/Sections/<profile>.
    for (const auto& name : EnumerateFolders(ghost_data_paths::SectionRoot(g_storage_dir))) {
        if (!ValidProfileName(name)) continue;
        const auto old_dir = ghost_data_paths::SectionRoot(g_storage_dir) + L"\\" + name;
        for (const auto& file : EnumerateFiles(old_dir, L"gold*.ghost"))
            MigrateOneLegacyGhost(name, file, old_dir + L"\\" + file, count);
        RemoveDirectoryW(old_dir.c_str()); // Succeeds only when empty.
    }
    // Oldest layout: ghost files stored alongside <profile>.ini in Splits.
    for (const auto& file : EnumerateFiles(g_splits_dir, L"*.ghost")) {
        const auto delimiter = file.find(L".ini.");
        if (delimiter == std::wstring::npos) continue;
        const auto name = file.substr(0, delimiter);
        const auto suffix = file.substr(delimiter + 5);
        MigrateOneLegacyGhost(name, suffix, g_splits_dir + L"\\" + file, count);
    }
    // Remove only directories that are completely empty. Any unexpected or
    // failed-to-migrate file retains its directory without being touched.
    RemoveDirectoryW(ghost_data_paths::SectionRoot(g_storage_dir).c_str());
    RemoveDirectoryW(old_ghosts.c_str());
    if (!g_splits_dir.empty()) RemoveDirectoryW(g_splits_dir.c_str());
    if (count.moved || count.failed || count.conflicts)
        Log((L"Profile storage migration: moved=" + std::to_wstring(count.moved) +
            L", failed=" + std::to_wstring(count.failed) +
            L", conflicts=" + std::to_wstring(count.conflicts) +
            L"; original files preserved on failure/conflict").c_str());
}

std::wstring LegacyDataFolder() {
    wchar_t local[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (!n || n >= MAX_PATH) return L"";
    return std::wstring(local) + L"\\RefunctUtility";
}

bool InitializeStorage() {
    StartupTrace("STORAGE 01: entered InitializeStorage");
    bool standard_layout = false;
    const auto storage = runtime_logs::StorageDirectory(runtime_logs::ModulePath(nullptr), &standard_layout);
    if (storage.empty()) return false;
    if (!standard_layout)
        Log(L"INFO: nonstandard Refunct directory layout; storing data by game EXE");
    StartupTrace("STORAGE 02: resolved Refunct root");
    g_storage_dir = storage;
    g_splits_dir = g_storage_dir + L"\\Splits";
    if (!EnsureDirectory(g_storage_dir) || !EnsureDirectory(g_splits_dir) ||
        !EnsureDirectory(ghost_data_paths::GhostRoot(g_storage_dir)) ||
        !EnsureDirectory(ghost_data_paths::SectionRoot(g_storage_dir))) {
        Log(L"ERROR: unable to create RefunctUtility folder inside game installation");
        return false;
    }
    // Retry directory creation after early tracing, without making an
    // unavailable log directory fatal to settings/profile initialization.
    if (!runtime_logs::EnsureDirectory(runtime_logs::Directory(g_storage_dir)))
        StartupTrace("STORAGE: unable to create runtime Logs directory");
    const std::wstring maps_dir = g_storage_dir + L"\\Maps";
    if (!EnsureDirectory(maps_dir)) {
        Log((L"Custom maps: cannot create Maps folder: " + maps_dir).c_str());
        // Other features must remain available if the map directory is blocked.
    } else {
        Log((L"Custom maps folder ready: " + maps_dir).c_str());
    }
    // Run before ScanProfiles/LoadPersonalBest and before I/O workers start.
    // Migration is bounded by the number of old ghost files (max 37/profile).
    MigrateLegacyGhostFiles();
    StartupTrace("STORAGE 03: directories ready");
    g_settings_path = g_storage_dir + L"\\settings.ini";
    g_checkpoint_path = g_storage_dir + L"\\save-state.ini";
    // One-time non-destructive import. The old AppData files are not removed.
    const std::wstring legacy = LegacyDataFolder();
    if (!legacy.empty() && !IsRegularFile(g_settings_path))
        CopyFileW((legacy + L"\\settings.ini").c_str(), g_settings_path.c_str(), TRUE);
    if (!legacy.empty() && !IsRegularFile(ProfilePath(L"Any%")))
        CopyFileW((legacy + L"\\personal_best.ini").c_str(), ProfilePath(L"Any%").c_str(), TRUE);
    StartupTrace("STORAGE 04: before storage-path Log()");
    Log((L"Refunct Utility storage: " + g_storage_dir).c_str());
    StartupTrace("STORAGE 05: finished InitializeStorage");
    return true;
}

// Retain the existing INI key names/format. Only immutable copies are sent
// to this writer: it never reads game globals or Unreal objects.
namespace {
struct IniEntry {
    std::wstring section;
    std::wstring key;
    std::wstring value;
};
struct SettingsWriter {
    SRWLOCK lock = SRWLOCK_INIT;
    HANDLE wake = nullptr;
    HANDLE idle = nullptr;
    std::vector<IniEntry> pending;
    std::wstring path;
    bool work_pending = false;
    std::atomic_bool failed{false};
};
INIT_ONCE s_settings_writer_once = INIT_ONCE_STATIC_INIT;
SettingsWriter* s_settings_writer = nullptr;

DWORD WINAPI SettingsWriterMain(LPVOID context) {
    auto* worker = static_cast<SettingsWriter*>(context);
    for (;;) {
        if (WaitForSingleObject(worker->wake, INFINITE) != WAIT_OBJECT_0) return 1;
        for (;;) {
            std::vector<IniEntry> entries;
            std::wstring path;
            AcquireSRWLockExclusive(&worker->lock);
            if (!worker->work_pending) {
                SetEvent(worker->idle);
                ReleaseSRWLockExclusive(&worker->lock);
                break;
            }
            entries.swap(worker->pending);
            path = worker->path;
            worker->work_pending = false;
            ReleaseSRWLockExclusive(&worker->lock);
            bool ok = true;
            for (const auto& entry : entries) {
                bool saved = false;
                for (int attempt = 0; attempt < 3 && !saved; ++attempt) {
                    if (attempt) Sleep(25); // worker only
                    saved = WritePrivateProfileStringW(entry.section.c_str(),
                        entry.key.c_str(), entry.value.c_str(), path.c_str()) != 0;
                }
                if (!saved) ok = false;
            }
            if (!ok) {
                worker->failed.store(true, std::memory_order_release);
                Log(L"ERROR: asynchronous settings save failed (check folder permissions)");
            }
        }
    }
}

BOOL CALLBACK InitializeSettingsWriter(PINIT_ONCE, PVOID, PVOID*) {
    auto* worker = new (std::nothrow) SettingsWriter;
    if (!worker) return TRUE;
    worker->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    worker->idle = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    if (worker->wake && worker->idle) {
        HANDLE thread = CreateThread(nullptr, 0, SettingsWriterMain, worker, 0, nullptr);
        if (thread) {
            CloseHandle(thread);
            s_settings_writer = worker;
            return TRUE;
        }
    }
    if (worker->wake) CloseHandle(worker->wake);
    if (worker->idle) CloseHandle(worker->idle);
    delete worker;
    return TRUE;
}

bool EnqueueSettings(std::vector<IniEntry>&& entries) {
    if (g_settings_path.empty() || entries.empty()) return false;
    InitOnceExecuteOnce(&s_settings_writer_once, InitializeSettingsWriter, nullptr, nullptr);
    auto* worker = s_settings_writer;
    if (!worker) return false;
    AcquireSRWLockExclusive(&worker->lock);
    // Signal while holding the queue lock. If the OS event fails, caller
    // retains its untouched snapshot for the synchronous error fallback.
    if (!SetEvent(worker->wake)) {
        ReleaseSRWLockExclusive(&worker->lock);
        return false;
    }
    // Merge pending changes to the same key, while preserving the ordering
    // of unrelated sections and newer updates from Fly/Ghost toggles.
    if (!worker->work_pending || worker->path != g_settings_path) {
        worker->pending.clear();
        worker->path = g_settings_path;
    }
    for (auto& entry : entries) {
        auto found = std::find_if(worker->pending.begin(), worker->pending.end(),
            [&](const IniEntry& pending) {
                return pending.section == entry.section && pending.key == entry.key;
            });
        if (found != worker->pending.end()) {
            found->value = std::move(entry.value);
        } else {
            worker->pending.push_back(std::move(entry));
        }
    }
    worker->work_pending = true;
    ResetEvent(worker->idle);
    ReleaseSRWLockExclusive(&worker->lock);
    return true;
}
} // namespace

// Queue a single independent Ghost or movement setting, rather than touching
// settings.ini from the game tick. Never wait for the writer from hotkeys.
bool QueueSettingsValue(const wchar_t* section, const wchar_t* key,
                        const wchar_t* value) {
    if (!section || !*section || !key || !*key || !value ||
        g_settings_path.empty()) return false;
    std::vector<IniEntry> entries;
    entries.push_back({section, key, value});
    if (EnqueueSettings(std::move(entries))) return true;
    // Failure to start the worker must not silently discard a setting.
    return WritePrivateProfileStringW(section, key, value, g_settings_path.c_str()) != 0;
}

bool DrainSettingsWriter(DWORD timeout_ms) {
    if (!s_settings_writer) return true;
    return WaitForSingleObject(s_settings_writer->idle, timeout_ms) == WAIT_OBJECT_0;
}

void SaveMovementModeSettings() {
    if (g_settings_path.empty()) return;
    const wchar_t* flying = (g_flying || g_restore_flying_pending) ? L"1" : L"0";
    const wchar_t* noclip = (g_noclip || g_restore_noclip_pending) ? L"1" : L"0";
    std::vector<IniEntry> modes;
    modes.reserve(2);
    modes.push_back({L"Movement", L"Flying", flying});
    modes.push_back({L"Movement", L"Noclip", noclip});
    if (!EnqueueSettings(std::move(modes))) {
        // A rare worker-start failure must not discard a user's mode changes.
        WritePrivateProfileStringW(L"Movement", L"Flying", flying, g_settings_path.c_str());
        WritePrivateProfileStringW(L"Movement", L"Noclip", noclip, g_settings_path.c_str());
    }
}

void SaveSettings() {
    if (g_settings_path.empty()) return;
    std::vector<IniEntry> entries;
    entries.reserve(180);
    auto write = [&](const wchar_t* section, const wchar_t* key, const std::wstring& value) {
        entries.push_back({section, key, value});
    };
    if (HotkeyProfilesReady())
        write(L"HotkeyProfiles", L"ActiveProfile", ActiveHotkeyProfile());
    if (HudProfilesReady())
        write(L"HUDProfiles", L"ActiveProfile", ActiveHudProfile());
    write(L"Settings", L"UtilLogEnabled",
          g_util_log_enabled.load(std::memory_order_relaxed) ? L"1" : L"0");
    write(L"Settings", L"StartupLogEnabled",
          g_startup_log_enabled.load(std::memory_order_relaxed) ? L"1" : L"0");
    write(L"Display", L"Coordinates", g_show_coordinates ? L"1" : L"0");
    write(L"Display", L"Timer", g_show_timer ? L"1" : L"0");
    write(L"Display", L"ShowFPS", g_show_fps ? L"1" : L"0");
    write(L"Display", L"ShowGameTime", g_show_game_time_hud ? L"1" : L"0");
    write(L"Display", L"ShowLocalTime", g_show_local_time_hud ? L"1" : L"0");
    write(L"Display", L"ShowSpeed", g_show_speed ? L"1" : L"0");
    write(L"Display", L"ShowCompletionPercent", g_show_completion_percent ? L"1" : L"0");
    write(L"Display", L"ShowButtonNumber", g_show_button_number ? L"1" : L"0");
    write(L"Display", L"ShowKeyDisplay", g_show_key_display ? L"1" : L"0");
    write(L"Display", L"ShowPracticeStatus", g_show_practice_status_hud ? L"1" : L"0");
    write(L"Display", L"ShowRecordStatus", g_show_record_status_hud ? L"1" : L"0");
    write(L"Display", L"ShowEnabledModules", g_show_enabled_modules ? L"1" : L"0");
    write(L"Display", L"ShowActiveCheats", g_show_active_cheats ? L"1" : L"0");
    write(L"Display", L"ShowActiveCustomisations", g_show_active_customisations ? L"1" : L"0");
    write(L"Display", L"ShowMenuDescriptions", g_show_menu_descriptions ? L"1" : L"0");
    for (size_t i = 0; i < kHudElementCount; ++i)
        write(L"HudPositions", kHudPositionKeys[i],
              std::to_wstring(static_cast<int>(g_hud_corners[i])));
    // Separate keys keep old HUD placements and INI files backward compatible.
    for (size_t i = 0; i < kHudElementCount; ++i) {
        const std::wstring key = kHudPositionKeys[i];
        write(L"HudAppearance", (key + L"ColourMode").c_str(),
              std::to_wstring(g_hud_colour_modes[i]));
        write(L"HudAppearance", (key + L"CustomRGB").c_str(),
              std::to_wstring(g_hud_custom_colours[i] & 0xFFFFFFu));
        write(L"HudAppearance", (key + L"SizePercent").c_str(),
              std::to_wstring(g_hud_size_percent[i]));
        write(L"HudAppearance", (key + L"ShowHeader").c_str(),
              g_hud_show_headers[i] ? L"1" : L"0");
    }
    write(L"HudPositions", L"F5MenuPosition",
          std::to_wstring(static_cast<int>(g_menu_position)));
    write(L"Display", L"CubeProgress", g_cube_display_mode != ProgressDisplayMode::Off ? L"1" : L"0");
    write(L"Display", L"TileProgress", g_tile_display_mode != ProgressDisplayMode::Off ? L"1" : L"0");
    write(L"Display", L"CubeDisplayMode", std::to_wstring(static_cast<int>(g_cube_display_mode)));
    write(L"Display", L"TileDisplayMode", std::to_wstring(static_cast<int>(g_tile_display_mode)));
    write(L"Display", L"ButtonSplits", g_show_splits ? L"1" : L"0");
    write(L"Display", L"ShowPBRun", g_show_pb_run_hud ? L"1" : L"0");
    write(L"Display", L"ShowSumOfBest", g_show_sum_of_best_hud ? L"1" : L"0");
    write(L"Display", L"ShowLastButtonSplit", g_show_last_button_split_hud ? L"1" : L"0");
    write(L"Display", L"ShowCompareTime", g_show_compare_hud ? L"1" : L"0");
    write(L"Display", L"CompareTimeView", std::to_wstring(static_cast<int>(g_compare_hud_view)));
    write(L"Display", L"ShowBestSegmentTime", g_show_best_segment_time_hud ? L"1" : L"0");
    write(L"Display", L"ShowPBSplitTime", g_show_pb_split_time_hud ? L"1" : L"0");
    write(L"Display", L"BestSegmentTimeView", std::to_wstring(static_cast<int>(g_best_segment_time_view)));
    write(L"Display", L"PBSplitTimeView", std::to_wstring(static_cast<int>(g_pb_split_time_view)));
    // Legacy visibility key remains for upgrades/downgrades.
    write(L"Display", L"PBGainLossDisplay", std::to_wstring(static_cast<int>(g_pb_gain_loss_display)));
    write(L"Display", L"SectionVsPBDisplay", std::to_wstring(static_cast<int>(g_section_vs_pb_display)));
    write(L"Display", L"SectionVsFileDisplay", std::to_wstring(static_cast<int>(g_section_vs_file_display)));
    write(L"Display", L"SplitComparePopup", g_show_compare_popup ? L"1" : L"0");
    write(L"Display", L"SplitComparePopupTime", g_compare_popup_show_time ? L"1" : L"0");
    write(L"Display", L"SplitComparePopupHeader", g_compare_popup_show_header ? L"1" : L"0");
    write(L"Display", L"CompareMode", std::to_wstring(static_cast<int>(g_compare_mode)));
    // Write both new 3-way modes and legacy settings for compatibility.
    write(L"Display", L"SplitSegmentDisplayMode", std::to_wstring(static_cast<int>(g_segment_row_mode)));
    write(L"Display", L"SplitPBDisplayMode", std::to_wstring(static_cast<int>(g_pb_row_mode)));
    write(L"Display", L"ComparePB", g_pb_row_mode == SplitRowMode::Off ? L"0" : L"1");
    write(L"Display", L"SplitButtonNumber", g_show_split_button ? L"1" : L"0");
    write(L"Display", L"SplitTotalTime", g_show_split_total_time ? L"1" : L"0");
    write(L"Display", L"SplitSegmentTime", g_segment_row_mode == SplitRowMode::Off ? L"0" : L"1");
    write(L"Display", L"SplitSegmentLabel", g_segment_row_mode == SplitRowMode::HeaderAndTime ? L"1" : L"0");
    write(L"Display", L"SplitFastestDelta", g_show_split_fastest_delta ? L"1" : L"0");
    write(L"Display", L"SplitFastestDeltaTime", g_show_fastest_delta_time ? L"1" : L"0");
    write(L"Display", L"SplitFastestDeltaLabel", g_show_fastest_delta_label ? L"1" : L"0");
    write(L"Movement", L"KeepOnGameLaunch", g_keep_movement_on_game_launch ? L"1" : L"0");
    write(L"Movement", L"KeepOnMapRestart", g_keep_movement_on_map_restart ? L"1" : L"0");
    write(L"World", L"KeepOnGameLaunch", g_keep_world_on_game_launch ? L"1" : L"0");
    write(L"World", L"KeepOnMapRestart", g_keep_world_on_map_restart ? L"1" : L"0");
    write(L"Recording", L"SavePersonalBests", g_save_personal_bests ? L"1" : L"0");
    write(L"Recording", L"SaveFastestSegments", g_save_fastest_segments ? L"1" : L"0");
    write(L"Recording", L"SavePBGhosts", g_save_pb_ghosts ? L"1" : L"0");
    write(L"Recording", L"SaveFastestGhosts", g_save_fastest_ghosts ? L"1" : L"0");
    write(L"Recording", L"AutoCreatePBProfile", g_auto_create_pb_profile ? L"1" : L"0");
    write(L"Recording", L"CopyBestSegmentsToPBProfile", g_auto_pb_copy_best_segments ? L"1" : L"0");
    write(L"Recording", L"CheatRecordMode", std::to_wstring(static_cast<int>(g_cheat_record_mode)));
    // Save the intended modes, including the brief hand-off between two actors.
    write(L"Movement", L"Flying", (g_flying || g_restore_flying_pending) ? L"1" : L"0");
    write(L"Movement", L"Noclip", (g_noclip || g_restore_noclip_pending) ? L"1" : L"0");
    write(L"Movement", L"FlySpeed", std::to_wstring(static_cast<int>(g_fly_speed)));
    write(L"Movement", L"WalkSpeedPercent", std::to_wstring(g_walk_speed_percent));
    write(L"Movement", L"SwimSpeedPercent", std::to_wstring(g_swim_speed_percent));
    write(L"Movement", L"ClimbSpeedPercent", std::to_wstring(g_climb_speed_percent));
    write(L"Movement", L"SlideSpeedPercent", std::to_wstring(g_slide_speed_percent));
    write(L"Movement", L"SlideDistancePercent", std::to_wstring(g_slide_distance_percent));
    write(L"Movement", L"JumpHeightPercent", std::to_wstring(g_jump_height_percent));
    write(L"Movement", L"AccelerationPercent", std::to_wstring(g_acceleration_percent));
    write(L"Movement", L"GameSpeedPercent", std::to_wstring(g_game_speed_percent));
    write(L"World", L"PlayerFOV", std::to_wstring(g_player_fov_degrees));
    write(L"Crosshair", L"Style", std::to_wstring(g_crosshair.style));
    write(L"Crosshair", L"Size", std::to_wstring(g_crosshair.size));
    write(L"Crosshair", L"Thickness", std::to_wstring(g_crosshair.thickness));
    write(L"Crosshair", L"Gap", std::to_wstring(g_crosshair.gap));
    write(L"Crosshair", L"Opacity", std::to_wstring(g_crosshair.opacity));
    write(L"Crosshair", L"Rgb", std::to_wstring(g_crosshair.rgb));
    write(L"World", L"AnimateSectionChanges", g_animate_section_changes ? L"1" : L"0");
    write(L"World", L"SectionSpeedPercent", std::to_wstring(g_section_speed_percent));
    write(L"World", L"ButtonsSyncWithSections", g_buttons_sync_with_sections ? L"1" : L"0");
    write(L"Movement", L"PreserveMomentum", g_preserve_momentum ? L"1" : L"0");
    write(L"Movement", L"ScrollToJump", g_scroll_to_jump ? L"1" : L"0");
    write(L"Movement", L"HoldSpaceToJump", g_hold_space_to_jump ? L"1" : L"0");
    write(L"Movement", L"AirJumpEnabled", g_air_jump_enabled ? L"1" : L"0");
    write(L"Movement", L"ScrollJumpTargets", std::to_wstring(static_cast<int>(g_scroll_jump_targets)));
    write(L"Movement", L"HoldJumpTargets", std::to_wstring(static_cast<int>(g_hold_jump_targets)));
    if (g_teleport_target_initialized) {
        write(L"Teleport", L"X", CheckpointFloat(g_teleport_target.x));
        write(L"Teleport", L"Y", CheckpointFloat(g_teleport_target.y));
        write(L"Teleport", L"Z", CheckpointFloat(g_teleport_target.z));
    }
    write(L"Practice", L"SelectedButton", std::to_wstring(g_practice_selected_button));
    write(L"Practice", L"KeepLastButtonMomentum",
          g_keep_last_button_momentum ? L"1" : L"0");
    // Keep the legacy key in sync so switching back to v9.29 preserves H's setting.
    write(L"Restart", L"AnimatePlatformRise", g_progress_rise_animation_enabled ? L"1" : L"0");
    write(L"Practice", L"AnimateProgressRestart", g_progress_rise_animation_enabled ? L"1" : L"0");
    if (ValidProfileName(g_active_profile)) write(L"Splits", L"ActiveProfile", g_active_profile);
    if (ValidProfileName(g_compare_profile)) write(L"Splits", L"CompareProfile", g_compare_profile);
    for (size_t i = 0; i < kActionCount; ++i) {
        write(L"Hotkeys", kActions[i].ini_name, std::to_wstring(g_keys[i].load()));
    }
    // No disk writes in this call; the worker coalesces rapid menu changes.
    if (!EnqueueSettings(std::move(entries))) {
        // Worker creation/event failure: preserve the established save path.
        // A synchronous fallback is intentionally rare, never the steady state.
        for (const auto& entry : entries) {
            WritePrivateProfileStringW(entry.section.c_str(), entry.key.c_str(),
                                       entry.value.c_str(), g_settings_path.c_str());
        }
    }
}

// Read large percentages as text. GetPrivateProfileIntW returns UINT, which
// loses the sign and can give a misleading result for values above INT_MAX.
int ReadUncappedPercentSetting(const wchar_t* key, int minimum,
                               const wchar_t* section) {
    wchar_t buffer[64]{};
    if (!GetPrivateProfileStringW(section, key, L"100", buffer,
            static_cast<DWORD>(std::size(buffer)), g_settings_path.c_str())) {
        return 100;
    }

    wchar_t* end = nullptr;
    errno = 0;
    const long long parsed = std::wcstoll(buffer, &end, 10);
    if (end == buffer) return 100;
    while (*end == L' ' || *end == L'\t') ++end;
    if (*end != L'\0') return 100;
    if (errno == ERANGE) return parsed < 0 ? minimum : kMaximumRepresentablePercent;
    return static_cast<int>(std::clamp<long long>(
        parsed, minimum, kMaximumRepresentablePercent));
}

void LoadSettings() {
    StartupTrace("SETTINGS 01: entered LoadSettings");
    for (size_t i = 0; i < kActionCount; ++i) g_keys[i].store(kActions[i].default_key);
    StartupTrace("SETTINGS 02: before InitializeStorage");
    if (!InitializeStorage()) { StartupTrace("SETTINGS: InitializeStorage returned false"); return; }
    StartupTrace("SETTINGS 03: after InitializeStorage");
    // Performance reporting follows the master log preference. Ignore the
    // retired independent PerformanceMonitor INI key in older installations.
    const bool util_log_requested = GetPrivateProfileIntW(L"Settings", L"UtilLogEnabled", 1,
        g_settings_path.c_str()) != 0;
    g_util_log_enabled.store(util_log_requested,
                            std::memory_order_release);
    g_startup_log_enabled.store(GetPrivateProfileIntW(L"Settings", L"StartupLogEnabled", 1,
        g_settings_path.c_str()) != 0, std::memory_order_release);
    ConfigureUtilLogDirectory(g_storage_dir);
    SetPerformanceMonitorEnabled(util_log_requested);
    wchar_t profile[64]{};
    GetPrivateProfileStringW(L"Splits", L"ActiveProfile", L"Any%", profile,
                             static_cast<DWORD>(std::size(profile)), g_settings_path.c_str());
    if (ValidProfileName(profile)) g_active_profile = profile;
    wchar_t comparison_profile[64]{};
    GetPrivateProfileStringW(L"Splits", L"CompareProfile", L"", comparison_profile,
                             static_cast<DWORD>(std::size(comparison_profile)), g_settings_path.c_str());
    g_compare_profile = ValidProfileName(comparison_profile)
        ? comparison_profile : L"";
    // Only accept a complete, finite, in-bounds target. Corrupt/missing fields
    // leave the target uninitialized and allow the menu to seed the live position.
    const auto read_teleport_axis = [&](const wchar_t* key, float& result) {
        wchar_t buffer[64]{};
        if (GetPrivateProfileStringW(L"Teleport", key, L"", buffer,
              static_cast<DWORD>(std::size(buffer)), g_settings_path.c_str()) == 0)
            return false;
        wchar_t* end = nullptr;
        errno = 0;
        const float value = std::wcstof(buffer, &end);
        if (end == buffer || *end != L'\0' || errno == ERANGE ||
            !std::isfinite(value) || std::abs(value) > 100000.0f)
            return false;
        result = value;
        return true;
    };
    Vec3 saved_teleport{};
    if (read_teleport_axis(L"X", saved_teleport.x) &&
        read_teleport_axis(L"Y", saved_teleport.y) &&
        read_teleport_axis(L"Z", saved_teleport.z)) {
        g_teleport_target = saved_teleport;
        g_teleport_target_initialized = true;
    }
    g_show_coordinates = GetPrivateProfileIntW(L"Display", L"Coordinates", 1, g_settings_path.c_str()) != 0;
    g_show_timer = GetPrivateProfileIntW(L"Display", L"Timer", 1, g_settings_path.c_str()) != 0;
    g_show_fps = GetPrivateProfileIntW(L"Display", L"ShowFPS", 0, g_settings_path.c_str()) != 0;
    g_show_game_time_hud = GetPrivateProfileIntW(L"Display", L"ShowGameTime", 0, g_settings_path.c_str()) != 0;
    g_show_local_time_hud = GetPrivateProfileIntW(L"Display", L"ShowLocalTime", 0, g_settings_path.c_str()) != 0;
    g_show_speed = GetPrivateProfileIntW(L"Display", L"ShowSpeed", 0, g_settings_path.c_str()) != 0;
    g_show_completion_percent = GetPrivateProfileIntW(
        L"Display", L"ShowCompletionPercent", 0, g_settings_path.c_str()) != 0;
    g_show_button_number = GetPrivateProfileIntW(
        L"Display", L"ShowButtonNumber", 0, g_settings_path.c_str()) != 0;
    g_show_key_display = GetPrivateProfileIntW(
        L"Display", L"ShowKeyDisplay", 0, g_settings_path.c_str()) != 0;
    g_show_practice_status_hud = GetPrivateProfileIntW(
        L"Display", L"ShowPracticeStatus", 1, g_settings_path.c_str()) != 0;
    g_show_record_status_hud = GetPrivateProfileIntW(
        L"Display", L"ShowRecordStatus", 1, g_settings_path.c_str()) != 0;
    g_show_enabled_modules = GetPrivateProfileIntW(L"Display", L"ShowEnabledModules", 0, g_settings_path.c_str()) != 0;
    g_show_active_cheats = GetPrivateProfileIntW(L"Display", L"ShowActiveCheats", 0, g_settings_path.c_str()) != 0;
    g_show_active_customisations = GetPrivateProfileIntW(L"Display", L"ShowActiveCustomisations", 0, g_settings_path.c_str()) != 0;
    g_show_menu_descriptions = GetPrivateProfileIntW(L"Display", L"ShowMenuDescriptions", 0, g_settings_path.c_str()) != 0;
    for (size_t i = 0; i < kHudElementCount; ++i) {
        const int fallback = static_cast<int>(kDefaultHudCorners[i]);
        const int value = GetPrivateProfileIntW(
            L"HudPositions", kHudPositionKeys[i], fallback, g_settings_path.c_str());
        g_hud_corners[i] = static_cast<HudCorner>(
            value >= 0 && value <= static_cast<int>(HudCorner::Center) ? value : fallback);
    }
    for (size_t i = 0; i < kHudElementCount; ++i) {
        const std::wstring key = kHudPositionKeys[i];
        const int colour = GetPrivateProfileIntW(
            L"HudAppearance", (key + L"ColourMode").c_str(), 0,
            g_settings_path.c_str());
        const int rgb = GetPrivateProfileIntW(
            L"HudAppearance", (key + L"CustomRGB").c_str(), 0xFFFFFF,
            g_settings_path.c_str());
        const int size = GetPrivateProfileIntW(
            L"HudAppearance", (key + L"SizePercent").c_str(), 100,
            g_settings_path.c_str());
        g_hud_colour_modes[i] = static_cast<uint8_t>(
            colour >= 0 && colour < kHudColourModeCount ? colour : 0);
        g_hud_custom_colours[i] = rgb >= 0 && rgb <= 0xFFFFFF
            ? static_cast<uint32_t>(rgb) : 0xFFFFFFu;
        g_hud_size_percent[i] = (size >= kHudMinSizePercent &&
            size <= kHudMaxSizePercent && size % kHudSizeStepPercent == 0)
            ? size : 100;
        g_hud_show_headers[i] = GetPrivateProfileIntW(
            L"HudAppearance", (key + L"ShowHeader").c_str(),
            kDefaultHudShowHeaders[i] ? 1 : 0, g_settings_path.c_str()) != 0;
    }
    const int saved_menu_position = GetPrivateProfileIntW(
        L"HudPositions", L"F5MenuPosition", static_cast<int>(MenuPosition::TopRight),
        g_settings_path.c_str());
    // Migrate the previous Centre value (0) to TopRight without altering
    // existing corner choices (1..4).
    g_menu_position = static_cast<MenuPosition>(
        saved_menu_position >= 1 && saved_menu_position <= 4
            ? saved_menu_position : static_cast<int>(MenuPosition::TopRight));
    const auto read_display_mode = [&](const wchar_t* new_key, const wchar_t* legacy_key) {
        // Legacy ON maps to Total so existing users see the same display.
        const int fallback = GetPrivateProfileIntW(L"Display", legacy_key, 0, g_settings_path.c_str())
            ? static_cast<int>(ProgressDisplayMode::Total)
            : static_cast<int>(ProgressDisplayMode::Off);
        const int raw = GetPrivateProfileIntW(L"Display", new_key, fallback, g_settings_path.c_str());
        return static_cast<ProgressDisplayMode>(raw >= 0 && raw <= 2 ? raw : fallback);
    };
    g_cube_display_mode = read_display_mode(L"CubeDisplayMode", L"CubeProgress");
    g_tile_display_mode = read_display_mode(L"TileDisplayMode", L"TileProgress");
    g_show_splits = GetPrivateProfileIntW(L"Display", L"ButtonSplits", 1, g_settings_path.c_str()) != 0;
    // Missing keys default OFF for existing installations.
    g_show_pb_run_hud = GetPrivateProfileIntW(L"Display", L"ShowPBRun", 0, g_settings_path.c_str()) != 0;
    g_show_sum_of_best_hud = GetPrivateProfileIntW(L"Display", L"ShowSumOfBest", 0, g_settings_path.c_str()) != 0;
    g_show_last_button_split_hud = GetPrivateProfileIntW(L"Display", L"ShowLastButtonSplit", 0, g_settings_path.c_str()) != 0;
    g_show_compare_hud = GetPrivateProfileIntW(L"Display", L"ShowCompareTime", 0, g_settings_path.c_str()) != 0;
    g_show_best_segment_time_hud = GetPrivateProfileIntW(
        L"Display", L"ShowBestSegmentTime", 0, g_settings_path.c_str()) != 0;
    g_show_pb_split_time_hud = GetPrivateProfileIntW(
        L"Display", L"ShowPBSplitTime", 0, g_settings_path.c_str()) != 0;
    const auto read_record_time_view = [&](const wchar_t* key, RecordTimeView fallback) {
        const int raw = GetPrivateProfileIntW(
            L"Display", key, static_cast<int>(fallback), g_settings_path.c_str());
        return raw == static_cast<int>(RecordTimeView::Total) ||
               raw == static_cast<int>(RecordTimeView::Segment)
            ? static_cast<RecordTimeView>(raw) : fallback;
    };
    g_best_segment_time_view = read_record_time_view(L"BestSegmentTimeView", RecordTimeView::Segment);
    g_pb_split_time_view = read_record_time_view(L"PBSplitTimeView", RecordTimeView::Total);
    // Existing Compare Time HUD installations remain in Total mode.
    g_compare_hud_view = read_record_time_view(L"CompareTimeView", RecordTimeView::Total);
    // Preserve existing installations: new section comparisons are disabled
    // until explicitly selected in HUD > Timers & Splits > Split Display.
    const auto read_section_display = [&](const wchar_t* key) {
        const int raw = GetPrivateProfileIntW(L"Display", key,
            static_cast<int>(SectionCompareDisplayMode::Off), g_settings_path.c_str());
        return raw >= 0 && raw <= 4
            ? static_cast<SectionCompareDisplayMode>(raw) : SectionCompareDisplayMode::Off;
    };
    g_pb_gain_loss_display = read_section_display(L"PBGainLossDisplay");
    g_section_vs_pb_display = read_section_display(L"SectionVsPBDisplay");
    g_section_vs_file_display = read_section_display(L"SectionVsFileDisplay");
    g_show_compare_popup = GetPrivateProfileIntW(L"Display", L"SplitComparePopup", 0, g_settings_path.c_str()) != 0;
    // Previously, enabled comparison popups always included the header and time.
    g_compare_popup_show_time = GetPrivateProfileIntW(
        L"Display", L"SplitComparePopupTime", 1, g_settings_path.c_str()) != 0;
    g_compare_popup_show_header = GetPrivateProfileIntW(
        L"Display", L"SplitComparePopupHeader", 1, g_settings_path.c_str()) != 0;
    const int compare_mode = GetPrivateProfileIntW(L"Display", L"CompareMode", 0, g_settings_path.c_str());
    g_compare_mode = compare_mode == 1 ? ProfileCompareMode::BestSegments : ProfileCompareMode::PersonalBest;
    // Upgrade old ON/OFF + label switches into three states without changing
    // a user's current on-screen appearance. Invalid new values use legacy defaults.
    const bool old_pb_visible = GetPrivateProfileIntW(
        L"Display", L"ComparePB", 1, g_settings_path.c_str()) != 0;
    const bool old_segment_visible = GetPrivateProfileIntW(
        L"Display", L"SplitSegmentTime", 1, g_settings_path.c_str()) != 0;
    const bool old_segment_label = GetPrivateProfileIntW(
        L"Display", L"SplitSegmentLabel", 1, g_settings_path.c_str()) != 0;
    const SplitRowMode segment_fallback = !old_segment_visible ? SplitRowMode::Off :
        (old_segment_label ? SplitRowMode::HeaderAndTime : SplitRowMode::TimeOnly);
    const SplitRowMode pb_fallback = old_pb_visible ? SplitRowMode::TimeOnly : SplitRowMode::Off;
    const auto load_row_mode = [&](const wchar_t* key, SplitRowMode fallback) {
        const int raw = GetPrivateProfileIntW(L"Display", key,
            static_cast<int>(fallback), g_settings_path.c_str());
        return raw >= 0 && raw <= 2 ? static_cast<SplitRowMode>(raw) : fallback;
    };
    g_segment_row_mode = load_row_mode(L"SplitSegmentDisplayMode", segment_fallback);
    g_pb_row_mode = load_row_mode(L"SplitPBDisplayMode", pb_fallback);
    // Default all other popup components to ON for existing installations.
    g_show_split_button = GetPrivateProfileIntW(L"Display", L"SplitButtonNumber", 1, g_settings_path.c_str()) != 0;
    g_show_split_total_time = GetPrivateProfileIntW(L"Display", L"SplitTotalTime", 1, g_settings_path.c_str()) != 0;
    // Migrate the old "Fastest segment" visibility preference on first load.
    // The old SplitBestDifference default was OFF, but this replaces that
    // two-line display with one signed comparison enabled by default.
    const int legacy_fastest_visibility = GetPrivateProfileIntW(
        L"Display", L"SplitBestSegment", 1, g_settings_path.c_str());
    g_show_split_fastest_delta = GetPrivateProfileIntW(
        L"Display", L"SplitFastestDelta", legacy_fastest_visibility,
        g_settings_path.c_str()) != 0;
    g_show_fastest_delta_time = GetPrivateProfileIntW(
        L"Display", L"SplitFastestDeltaTime", 1, g_settings_path.c_str()) != 0;
    g_show_fastest_delta_label = GetPrivateProfileIntW(
        L"Display", L"SplitFastestDeltaLabel", 0, g_settings_path.c_str()) != 0;
    g_keep_movement_on_game_launch = GetPrivateProfileIntW(
        L"Movement", L"KeepOnGameLaunch", 0, g_settings_path.c_str()) != 0;
    g_keep_movement_on_map_restart = GetPrivateProfileIntW(
        L"Movement", L"KeepOnMapRestart", 0, g_settings_path.c_str()) != 0;
    g_keep_world_on_game_launch = GetPrivateProfileIntW(
        L"World", L"KeepOnGameLaunch", 0, g_settings_path.c_str()) != 0;
    g_keep_world_on_map_restart = GetPrivateProfileIntW(
        L"World", L"KeepOnMapRestart", 0, g_settings_path.c_str()) != 0;
    g_save_personal_bests = GetPrivateProfileIntW(
        L"Recording", L"SavePersonalBests", 1, g_settings_path.c_str()) != 0;
    g_save_fastest_segments = GetPrivateProfileIntW(
        L"Recording", L"SaveFastestSegments", 1, g_settings_path.c_str()) != 0;
    g_save_pb_ghosts = GetPrivateProfileIntW(
        L"Recording", L"SavePBGhosts", 1, g_settings_path.c_str()) != 0;
    g_save_fastest_ghosts = GetPrivateProfileIntW(
        L"Recording", L"SaveFastestGhosts", 1, g_settings_path.c_str()) != 0;
    g_auto_create_pb_profile = GetPrivateProfileIntW(
        L"Recording", L"AutoCreatePBProfile", 0, g_settings_path.c_str()) != 0;
    g_auto_pb_copy_best_segments = GetPrivateProfileIntW(
        L"Recording", L"CopyBestSegmentsToPBProfile", 1, g_settings_path.c_str()) != 0;
    const UINT raw_cheat_mode = GetPrivateProfileIntW(
        L"Recording", L"CheatRecordMode", 0, g_settings_path.c_str());
    // Reject corrupted/out-of-range settings instead of granting permission.
    g_cheat_record_mode = raw_cheat_mode <= 3
        ? static_cast<CheatRecordMode>(raw_cheat_mode)
        : CheatRecordMode::Off;
    // Don't set g_flying/g_noclip before a character exists; restore safely later.
    if (g_keep_movement_on_game_launch) {
        g_restore_flying_pending = GetPrivateProfileIntW(
            L"Movement", L"Flying", 0, g_settings_path.c_str()) != 0;
        g_restore_noclip_pending = GetPrivateProfileIntW(
            L"Movement", L"Noclip", 0, g_settings_path.c_str()) != 0;
        if (g_restore_noclip_pending) g_restore_flying_pending = true;
    }
    StartupTrace("SETTINGS 04: display/recording preferences loaded");
    const int speed = GetPrivateProfileIntW(L"Movement", L"FlySpeed", 1200, g_settings_path.c_str());
    g_fly_speed = static_cast<float>(std::clamp(speed, 200, 10000));
    g_walk_speed_percent = ReadUncappedPercentSetting(L"WalkSpeedPercent", kMovementMinimumPercent);
    g_swim_speed_percent = ReadUncappedPercentSetting(L"SwimSpeedPercent", kMovementMinimumPercent);
    g_climb_speed_percent = ReadUncappedPercentSetting(L"ClimbSpeedPercent", kMovementMinimumPercent);
    g_slide_speed_percent = ReadUncappedPercentSetting(L"SlideSpeedPercent", kMovementMinimumPercent);
    g_slide_distance_percent = ReadUncappedPercentSetting(L"SlideDistancePercent", kMovementMinimumPercent);
    g_jump_height_percent = ReadUncappedPercentSetting(L"JumpHeightPercent", kMovementMinimumPercent);
    g_acceleration_percent = ReadUncappedPercentSetting(L"AccelerationPercent", kMovementMinimumPercent);
    g_game_speed_percent = ReadUncappedPercentSetting(L"GameSpeedPercent", kGameSpeedMinimumPercent);
    g_animate_section_changes = GetPrivateProfileIntW(
        L"World", L"AnimateSectionChanges", 1, g_settings_path.c_str()) != 0;
    g_section_speed_percent = ReadUncappedPercentSetting(
        L"SectionSpeedPercent", 25, L"World");
    g_buttons_sync_with_sections = GetPrivateProfileIntW(
        L"World", L"ButtonsSyncWithSections", 0, g_settings_path.c_str()) != 0;
    // Preserve any signed, representable FOV. Unreal may render extreme
    // values unpredictably, but that is the user's explicit choice.
    wchar_t fov_text[64]{};
    GetPrivateProfileStringW(L"World", L"PlayerFOV", L"0", fov_text,
                             static_cast<DWORD>(std::size(fov_text)), g_settings_path.c_str());
    wchar_t* fov_end = nullptr;
    errno = 0;
    const long long saved_fov = std::wcstoll(fov_text, &fov_end, 10);
    while (*fov_end == L' ' || *fov_end == L'\t') ++fov_end;
    g_player_fov_degrees = (fov_end != fov_text && *fov_end == L'\0' &&
        errno != ERANGE && saved_fov >= (std::numeric_limits<int>::min)() &&
        saved_fov <= (std::numeric_limits<int>::max)())
        ? static_cast<int>(saved_fov) : 0;
    // Clamp invalid/edited INI values to a safe default rather than rendering
    // arbitrary geometry. Signed conversion handles negative INI input.
    const auto crosshair_read = [&](const wchar_t* key, int def, int lo, int hi) {
        const int n = static_cast<int>(GetPrivateProfileIntW(L"Crosshair", key, def, g_settings_path.c_str()));
        return n >= lo && n <= hi ? n : def;
    };
    g_crosshair.style = crosshair_read(L"Style", 0, 0, 5);
    g_crosshair.size = crosshair_read(L"Size", 8, 1, 100);
    g_crosshair.thickness = crosshair_read(L"Thickness", 2, 1, 20);
    g_crosshair.gap = crosshair_read(L"Gap", 4, 0, 50);
    g_crosshair.opacity = crosshair_read(L"Opacity", 100, 0, 100);
    g_crosshair.rgb = crosshair_read(L"Rgb", 16777215, 0, 16777215);
    g_preserve_momentum = GetPrivateProfileIntW(L"Movement", L"PreserveMomentum", 0,
        g_settings_path.c_str()) != 0;
    g_scroll_to_jump = GetPrivateProfileIntW(L"Movement", L"ScrollToJump", 0,
        g_settings_path.c_str()) != 0;
    g_hold_space_to_jump = GetPrivateProfileIntW(L"Movement", L"HoldSpaceToJump", 0,
        g_settings_path.c_str()) != 0;
    g_air_jump_enabled = GetPrivateProfileIntW(L"Movement", L"AirJumpEnabled", 0,
        g_settings_path.c_str()) != 0;
    // Legacy ScrollJumpMethod is ignored: scroll-up always uses Space remap.
    // Legacy HoldJumpMethod settings are ignored: holding Space always uses
    // the normal Space-input path, regardless of a saved v9.25 preference.
    // Values 0..2 retain their original meanings; 3..6 add climb combinations.
    // Invalid/old values safely fall back to Ground Only.
    g_scroll_jump_targets = JumpTargetsFromStoredValue(GetPrivateProfileIntW(
        L"Movement", L"ScrollJumpTargets", 0, g_settings_path.c_str()));
    g_hold_jump_targets = JumpTargetsFromStoredValue(GetPrivateProfileIntW(
        L"Movement", L"HoldJumpTargets", 0, g_settings_path.c_str()));
    g_practice_selected_button = std::clamp<int>(GetPrivateProfileIntW(L"Practice", L"SelectedButton", 1, g_settings_path.c_str()), 1, kRefunctButtonCount);
    g_keep_last_button_momentum = GetPrivateProfileIntW(
        L"Practice", L"KeepLastButtonMomentum", 0, g_settings_path.c_str()) != 0;
    // Existing v9.29 settings migrate without losing H's current toggle.
    // New installs default ON so a regular R remains vanilla/record eligible.
    const int old_rise_preference = GetPrivateProfileIntW(
        L"Practice", L"AnimateProgressRestart", 1, g_settings_path.c_str());
    g_progress_rise_animation_enabled = GetPrivateProfileIntW(
        L"Restart", L"AnimatePlatformRise", old_rise_preference, g_settings_path.c_str()) != 0;
    StartupTrace("SETTINGS 05: movement/restart preferences loaded");
    std::array<int, kActionCount> loaded{};
    bool valid_bindings = true;
    bool migrated_f11 = false;
    bool migrated_new_keys = false;
    for (size_t i = 0; i < kActionCount; ++i) {
        loaded[i] = GetPrivateProfileIntW(L"Hotkeys", kActions[i].ini_name,
                                          kActions[i].default_key, g_settings_path.c_str());
        // v2 assigned F11 to Timer; migrate old settings automatically.
        if (i == static_cast<size_t>(Action::Timer) && loaded[i] == VK_F11) {
            loaded[i] = VK_F3;
            migrated_f11 = true;
        }
        // 0 is the explicit unassigned value; multiple actions may be None.
        if (loaded[i] != 0 && !IsBindable(loaded[i])) valid_bindings = false;
        const auto already_used = [&](int vk) {
            for (size_t j = 0; j < i; ++j)
                if (vk != 0 && loaded[j] == vk) return true;
            return false;
        };
        if (already_used(loaded[i])) {
            // Existing custom binds must take priority over newly introduced
            // save-state controls. Pick an unused fallback instead of wiping
            // all user hotkey settings on a v9.2 -> v9.3 upgrade.
            if (i >= static_cast<size_t>(Action::SaveState)) {
                bool migrated = false;
                constexpr std::array<int, 9> candidates{VK_F4, VK_F1, 'B', 'J', 'K', 'L', 'G', 'H', 'Y'};
                for (const int candidate : candidates) {
                    if (IsBindable(candidate) && !already_used(candidate)) {
                        loaded[i] = candidate;
                        migrated = true;
                        migrated_new_keys = true;
                        break;
                    }
                }
                if (!migrated) valid_bindings = false;
            } else {
                valid_bindings = false;
            }
        }
    }
    StartupTrace("SETTINGS 06: hotkeys loaded and checked");
    if (!valid_bindings) {
        Log(L"WARNING: invalid/conflicting hotkeys in settings.ini; restoring default hotkeys");
        return;
    }
    for (size_t i = 0; i < kActionCount; ++i) g_keys[i].store(loaded[i]);
    if (migrated_f11 || migrated_new_keys) {
        if (migrated_f11) Log(L"INFO: migrated legacy F11 timer hotkey to F3");
        if (migrated_new_keys) Log(L"INFO: reassigned new checkpoint keys to preserve existing custom hotkeys");
        SaveSettings();
    }
    StartupTrace("SETTINGS 07: finished LoadSettings");
}


} // namespace refunct

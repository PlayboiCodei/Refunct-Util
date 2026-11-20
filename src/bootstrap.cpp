#include "refunct/internal_api.hpp"
#include "refunct/features/compatibility.hpp"
#include <MinHook.h>
#include "refunct/runtime_logs.hpp"

namespace refunct {
// The bootstrap worker (not DllMain) prepares the shared Logs directory
// even in diagnostic Mode 1, before settings or the C++ logger are initialized.
// Keep the early trace independent of the asynchronous logger.
void StartupTrace(const char* message) {
    if (!g_module || !message) return;
    // Read both saved preferences before the first trace, even in Mode 1.
    // Later calls use the live flags so settings changes are respected.
    static const bool preferences_loaded = [] {
        g_util_log_enabled.store(runtime_logs::SavedLoggingEnabled(), std::memory_order_release);
        g_startup_log_enabled.store(runtime_logs::SavedLoggingEnabled(L"StartupLogEnabled"),
                                    std::memory_order_release);
        return true;
    }();
    (void)preferences_loaded;
    static std::wstring path;
    if (path.empty()) {
        try {
            const auto logs = runtime_logs::Prepare(g_module);
            if (!logs.empty()) path = logs + L"\\RefunctUtility.startup.txt";
        } catch (...) {
            // Diagnostics must not prevent the mod from initializing.
        }
    }
    if (!g_startup_log_enabled.load(std::memory_order_acquire) ||
        !g_util_log_enabled.load(std::memory_order_acquire)) return;
    if (path.empty()) { OutputDebugStringA(message); return; }
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { OutputDebugStringA(message); return; }
    DWORD written = 0;
    WriteFile(file, message, static_cast<DWORD>(strlen(message)), &written, nullptr);
    const char ending[] = "\r\n";
    WriteFile(file, ending, 2, &written, nullptr);
    CloseHandle(file);
}

int DiagnosticMode() {
    // A missing diagnostic INI means normal full-featured operation (Mode 6).
    // Unreadable paths or malformed mode values use safe Mode 1 for diagnostics.
    if (!g_module) {
        StartupTrace("INI: module handle missing; defaulting to safe Mode 1");
        return 1;
    }

    wchar_t path[MAX_PATH]{};
    const DWORD count = GetModuleFileNameW(g_module, path, MAX_PATH);
    if (count == 0 || count >= MAX_PATH) {
        StartupTrace("INI: failed to resolve ASI path; defaulting to safe Mode 1");
        return 1;
    }
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) {
        StartupTrace("INI: ASI path has no parent folder; defaulting to safe Mode 1");
        return 1;
    }
    const wchar_t file_name[] = L"RefunctUtility.diagnostic.ini";
    const size_t remaining = MAX_PATH - static_cast<size_t>(slash + 1 - path);
    if (remaining < std::size(file_name)) {
        StartupTrace("INI: diagnostic filename exceeds MAX_PATH; defaulting to safe Mode 1");
        return 1;
    }
    wcscpy_s(slash + 1, remaining, file_name);

    // Log the exact file path Windows is reading, including unexpected folders.
    char config_path_utf8[MAX_PATH * 4]{};
    if (WideCharToMultiByte(CP_UTF8, 0, path, -1, config_path_utf8,
            static_cast<int>(std::size(config_path_utf8)), nullptr, nullptr) > 0) {
        StartupTrace("INI: expected configuration at:");
        StartupTrace(config_path_utf8);
    }

    const DWORD attributes = GetFileAttributesW(path);
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        // Without a diagnostic override, start the normal full-featured mod.
        StartupTrace("INI: diagnostic configuration missing; using full Mode 6");
        return 6;
    }

    const int mode = GetPrivateProfileIntW(L"Startup", L"Mode", 1, path);
    if (mode < 1 || mode > 6) {
        StartupTrace("INI: invalid mode; using safe Mode 1");
        return 1;
    }
    StartupTrace("INI: valid mode read from configuration");
    return mode;
}

DWORD WINAPI Initialize(LPVOID) {
    StartupTrace("01: Initialize thread entered");
    StartupTrace("BUILD: Optimisation Log menu fix 1");
    const int diagnostic_mode = DiagnosticMode();
    switch (diagnostic_mode) {
        case 1:
            StartupTrace("02: Safe mode 1: returned before settings and hooks");
            return 0;
        case 2: StartupTrace("02: Mode 2: settings only, no hooks"); break;
        case 3: StartupTrace("02: Mode 3: tick hook only"); break;
        case 4: StartupTrace("02: Mode 4: tick and HUD hooks"); break;
        case 5: StartupTrace("02: Mode 5: tick, HUD, input hooks; no new-game hook"); break;
        default: StartupTrace("02: Mode 6: full plugin"); break;
    }
    StartupTrace("02a: before Log() - native SRWLOCK logger");
    Log(L"Refunct Utility v1.0.0 diagnostics: initialization started");
    StartupTrace("02b: after Log()");
    g_game_module = GetModuleHandleW(nullptr);
    wchar_t game_path[MAX_PATH]{};
    const DWORD game_path_length = GetModuleFileNameW(nullptr, game_path, MAX_PATH);
    const wchar_t* game_filename = wcsrchr(game_path, L'\\');
    game_filename = game_filename ? game_filename + 1 : game_path;
    if (!g_game_module || game_path_length == 0 || game_path_length >= MAX_PATH ||
        _wcsicmp(game_filename, L"Refunct-Win32-Shipping.exe") != 0) {
        Log(L"ABORT: expected the exact Refunct-Win32-Shipping.exe game process");
        return 1;
    }
    // Create game-local settings and profiles immediately on loading the ASI,
    // before waiting for a valid game timer or installing engine hooks.
    StartupTrace("02c: before LoadSettings()");
    LoadSettings();
    InitialiseCustomisationProfiles();
    InitialiseWorldProfiles();
    InitialiseHudProfiles();
    InitialiseHotkeyProfiles();
    StartupTrace("02d: after LoadSettings()");
    // Relaunch persistence is independent of the map-restart preference.
    // The actual flying/collision modes are installed only after the first
    // valid character tick; percentages and jump toggles are already loaded.
    StartupTrace("02e: before movement persistence processing");
    if (!g_keep_movement_on_game_launch) {
        const bool had_modes = g_restore_flying_pending || g_restore_noclip_pending;
        g_restore_flying_pending = false;
        g_restore_noclip_pending = false;
        if (ResetMovementConfiguration() || had_modes) {
            SaveSettings();
            Log(L"Movement defaults restored on game launch");
        }
    } else {
        Log(L"Movement settings retained on game launch");
    }
    StartupTrace("03: settings initialization finished");
    StartupTrace("03a: before LoadPersistedCheckpoint()");
    LoadPersistedCheckpoint();
    StartupTrace("03b: after LoadPersistedCheckpoint()");
    StartupTrace("03c: before ScanProfiles()");
    ScanProfiles(true);
    StartupTrace("03d: after ScanProfiles()");
    StartupTrace("03e: before RefreshProfiles()");
    RefreshProfiles();
    StartupTrace("03f: after RefreshProfiles()");
    if (!g_pb_path.empty()) {
        const std::wstring log_path = L"PB storage path: " + g_pb_path;
        Log(log_path.c_str());
        if (GetFileAttributesW(g_pb_path.c_str()) == INVALID_FILE_ATTRIBUTES)
            Log(L"ERROR: active split profile is missing after startup initialization");
    }
    if (!g_settings_path.empty() &&
        GetFileAttributesW(g_settings_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        SaveSettings();  // Also create settings.ini on first launch.
    }

    StartupTrace("04: profiles and checkpoints loaded");
    if (diagnostic_mode == 2) {
        StartupTrace("05: Mode 2 completed without hooks");
        return 0;
    }
    // The historical addresses remain valid only for the *exact* executable
    // that was tested. Do not create any MinHook detours if the on-disk game
    // has changed since its first known-working run.
    StartupTrace("04a: verifying SHA-256 pinned Refunct game executable");
    if (!ValidateGameExecutable(game_path)) {
        StartupTrace("ABORT: game executable fingerprint not trusted; no hooks installed");
        return 7;
    }
    StartupTrace("04b: game executable fingerprint accepted");
    const uintptr_t function_rvas[] = {
        kCharacterTickRva, kHudDrawRva, kHudDrawTextRva, kHudDrawRectRva,
        kHudTextSizeRva, kViewportSizeRva,
        kSetCollisionRva, kSlateKeyDownRva, kSlateKeyUpRva, kProcessEventRva,
        kFNameAppendRva, kMemoryFreeRva, kAccurateTimeRva,
        kAddBasedCharacterRva, kRemoveBasedCharacterRva
    };
    for (uintptr_t rva : function_rvas) {
        if (!IsRvaInSection(g_game_module, rva, true)) {
            Log(L"ABORT: historical function RVA not in executable memory");
            return 2;
        }
    }
    if (!IsRvaInSection(g_game_module, kGlobalWorldRva, false) ||
        !IsRvaInSection(g_game_module, kGlobalObjectArrayRva, false)) {
        Log(L"ABORT: historical GWorld RVA not in mapped executable");
        return 3;
    }
    StartupTrace("05: executable RVA checks passed");
    QueryPerformanceFrequency(&g_frequency);
    QueryPerformanceCounter(&g_session_start);
    const auto base = reinterpret_cast<uintptr_t>(g_game_module);
    g_draw_text = reinterpret_cast<HudDrawTextFn>(base + kHudDrawTextRva);
    g_draw_rect = reinterpret_cast<HudDrawRectFn>(base + kHudDrawRectRva);
    g_text_size = reinterpret_cast<HudTextSizeFn>(base + kHudTextSizeRva);
    g_viewport_size = reinterpret_cast<ViewportSizeFn>(base + kViewportSizeRva);
    g_set_collision = reinterpret_cast<SetCollisionFn>(base + kSetCollisionRva);
    g_fname_append = reinterpret_cast<FNameAppendFn>(base + kFNameAppendRva);
    g_memory_free = reinterpret_cast<MemoryFreeFn>(base + kMemoryFreeRva);
    g_process_event = reinterpret_cast<ProcessEventFn>(base + kProcessEventRva);
    g_get_accurate_time = reinterpret_cast<GetAccurateTimeFn>(base + kAccurateTimeRva);
    g_add_based_character = reinterpret_cast<BasedCharacterFn>(base + kAddBasedCharacterRva);
    g_remove_based_character = reinterpret_cast<BasedCharacterFn>(base + kRemoveBasedCharacterRva);

    StartupTrace("06: initializing MinHook");
    if (MH_Initialize() != MH_OK) { Log(L"ABORT: MH_Initialize failed"); StartupTrace("FAILED: MH_Initialize"); return 4; }
    struct Hook { uintptr_t rva; void* detour; void** original; };
    // Progressive hook isolation: do not install later hooks in earlier modes.
    const Hook hooks[] = {
        {kCharacterTickRva, reinterpret_cast<void*>(&CharacterTickHook), reinterpret_cast<void**>(&g_original_tick)},
        {kHudDrawRva, reinterpret_cast<void*>(&HudDrawHook), reinterpret_cast<void**>(&g_original_hud)},
        {kSlateKeyDownRva, reinterpret_cast<void*>(&KeyDownHook), reinterpret_cast<void**>(&g_original_keydown)},
        {kSlateKeyUpRva, reinterpret_cast<void*>(&KeyUpHook), reinterpret_cast<void**>(&g_original_keyup)},
    };
    const size_t hook_count = diagnostic_mode == 3 ? 1 :
                              diagnostic_mode == 4 ? 2 : std::size(hooks);
    for (size_t index = 0; index < hook_count; ++index) {
        const Hook& hook = hooks[index];
        switch (index) {
            case 0: StartupTrace("07: creating tick hook"); break;
            case 1: StartupTrace("08: creating HUD hook"); break;
            case 2: StartupTrace("09: creating keydown hook"); break;
            case 3: StartupTrace("10: creating keyup hook"); break;
        }
        void* address = reinterpret_cast<void*>(base + hook.rva);
        if (MH_CreateHook(address, hook.detour, hook.original) != MH_OK) {
            Log(L"ABORT: failed to create input/game/HUD hook");
            StartupTrace("FAILED: MH_CreateHook");
            MH_Uninitialize();
            return 5;
        }
        if (MH_QueueEnableHook(address) != MH_OK) {
            Log(L"ABORT: failed to queue hook");
            StartupTrace("FAILED: MH_QueueEnableHook");
            MH_Uninitialize();
            return 5;
        }
    }
    // Optional native button-contact gate for World > Sections. An actor's
    // collision flag alone cannot prevent ALiftBase::AddBasedCharacter from
    // registering a button press. Use the same real callback Refunct TAS hooks.
    // Failure must not disable the rest of the mod: the toggle then refuses ON.
    bool section_button_hook_queued = false;
    if (diagnostic_mode == 6) {
        void* callback = reinterpret_cast<void*>(base + kAddBasedCharacterRva);
        if (MH_CreateHook(callback,
                reinterpret_cast<void*>(&SectionButtonContactHook),
                reinterpret_cast<void**>(&g_original_add_based_character)) == MH_OK) {
            if (MH_QueueEnableHook(callback) == MH_OK) {
                section_button_hook_queued = true;
            } else {
                Log(L"WARNING: native button-contact hook could not be queued");
                MH_RemoveHook(callback);
                g_original_add_based_character = nullptr;
            }
        } else {
            Log(L"WARNING: native button-contact hook could not be installed");
        }
    }
    // Jump-pad launch-height adjustment must observe the actual Blueprint
    // PerformJumppadJump event. The previous character-property heuristic
    // cannot work: Jumppad is an argument of that function, not a field.
    // Hooking ProcessEvent is optional, full mode only, and checks a cached
    // UFunction pointer on the hot path. Never block normal game startup.
    bool jump_pad_event_hook_queued = false;
    if (diagnostic_mode == 6) {
        void* callback = reinterpret_cast<void*>(base + kProcessEventRva);
        if (MH_CreateHook(callback,
                reinterpret_cast<void*>(&JumpPadProcessEventHook),
                reinterpret_cast<void**>(&g_original_jump_pad_process_event)) == MH_OK) {
            if (MH_QueueEnableHook(callback) == MH_OK) {
                jump_pad_event_hook_queued = true;
            } else {
                Log(L"WARNING: jump pad launch event hook could not be queued");
                MH_RemoveHook(callback);
                g_original_jump_pad_process_event = nullptr;
            }
        } else {
            Log(L"WARNING: jump pad launch event hook could not be created");
        }
    }
    // Optional TAS new-game callback. A version mismatch must not prevent the
    // reliable HUD, movement, timers and normal restart from loading.
    if (diagnostic_mode == 6 && IsRvaInSection(g_game_module, kNewGameEventRva, true)) {
        void* callback = reinterpret_cast<void*>(base + kNewGameEventRva);
        if (MH_CreateHook(callback, reinterpret_cast<void*>(&NewGameEventHook),
                          reinterpret_cast<void**>(&g_original_new_game_event)) == MH_OK) {
            if (MH_QueueEnableHook(callback) == MH_OK) {
                g_preloaded_hook_available = true;
            } else {
                Log(L"WARNING: unable to enable optional native new-game callback");
            }
        } else {
            Log(L"WARNING: optional native new-game callback could not be hooked");
        }
    } else if (diagnostic_mode == 6) {
        Log(L"WARNING: native new-game RVA unavailable; preloaded restart disabled");
    }
    StartupTrace("11: applying queued hooks");
    if (MH_ApplyQueued() != MH_OK) {
        Log(L"ABORT: failed to enable hooks");
        MH_Uninitialize();
        return 6;
    }
    g_section_button_contact_hook_available = section_button_hook_queued;
    g_jump_pad_launch_hook_available = jump_pad_event_hook_queued;
    if (diagnostic_mode == 6) {
        Log(jump_pad_event_hook_queued ?
            L"Jump pad launch height: ProcessEvent hook ready" :
            L"WARNING: jump pad launch height hook not available");
        Log(section_button_hook_queued ?
            L"World sections: native button-contact gate ready" :
            L"World sections: native button-contact gate unavailable; toggle disabled");
    }
    StartupTrace("12: enabled selected hooks successfully");
    Log(L"Refunct Utility v1.0.0 diagnostics active");
    return 0;
}

} // namespace refunct

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        refunct::g_module = module;
        DisableThreadLibraryCalls(module);
        HANDLE thread = CreateThread(nullptr, 0, refunct::Initialize, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}

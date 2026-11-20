#pragma once

// Shared startup/storage paths and explicit background log maintenance.
// Never enumerate files during a character tick or normal log writes.
#include <Windows.h>
#include <string>
#include <vector>
#include <cwchar>
#include <initializer_list>

namespace refunct::runtime_logs {

inline std::wstring ParentDirectory(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring{} : path.substr(0, slash);
}

inline std::wstring ModulePath(HMODULE module) {
    wchar_t path[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(module, path, MAX_PATH);
    return n && n < MAX_PATH ? std::wstring(path, n) : std::wstring{};
}

inline std::wstring StorageDirectory(const std::wstring& executable,
                                     bool* standard_layout = nullptr) {
    constexpr wchar_t suffix[] = L"\\Refunct\\Binaries\\Win32\\Refunct-Win32-Shipping.exe";
    constexpr size_t length = sizeof(suffix) / sizeof(suffix[0]) - 1;
    const bool standard = executable.size() >= length &&
        _wcsicmp(executable.c_str() + executable.size() - length, suffix) == 0;
    if (standard_layout) *standard_layout = standard;
    if (standard) return executable.substr(0, executable.size() - length) + L"\\RefunctUtility";
    const auto parent = ParentDirectory(executable);
    return parent.empty() ? std::wstring{} : parent + L"\\RefunctUtility";
}

inline std::wstring Directory(const std::wstring& storage) {
    return storage.empty() ? std::wstring{} : storage + L"\\Logs";
}

inline bool EnsureDirectory(const std::wstring& path) {
    if (path.empty()) return false;
    if (CreateDirectoryW(path.c_str(), nullptr)) return true;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

inline bool IsRegularFile(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

// Startup tracing begins before LoadSettings. Resolve the same local/legacy
// settings preference without creating a log file or reading INI data per tick.
inline bool SavedLoggingEnabled(const wchar_t* key = L"UtilLogEnabled") {
    const auto storage = StorageDirectory(ModulePath(nullptr));
    if (storage.empty()) return true;
    auto settings = storage + L"\\settings.ini";
    if (!IsRegularFile(settings)) {
        wchar_t local[MAX_PATH]{};
        const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
        if (n && n < MAX_PATH)
            settings = std::wstring(local, n) + L"\\RefunctUtility\\settings.ini";
    }
    return GetPrivateProfileIntW(L"Settings", key, 1, settings.c_str()) != 0;
}

// Never overwrite a log. A collision is archived under a numbered filename;
// permission/sharing failures leave the source untouched for the next launch.
inline void MoveLegacyLog(const std::wstring& source_dir, const std::wstring& logs,
                          const std::wstring& filename) {
    if (source_dir.empty() || logs.empty() || _wcsicmp(source_dir.c_str(), logs.c_str()) == 0) return;
    const auto source = source_dir + L"\\" + filename;
    if (!IsRegularFile(source)) return;
    const auto dot = filename.find_last_of(L'.');
    const auto stem = filename.substr(0, dot);
    const auto extension = dot == std::wstring::npos ? std::wstring{} : filename.substr(dot);
    for (unsigned i = 0; i < 1000; ++i) {
        const auto target = logs + L"\\" + (i == 0 ? filename :
            stem + L".legacy-" + std::to_wstring(i) + extension);
        if (MoveFileW(source.c_str(), target.c_str())) return;
        const DWORD error = GetLastError();
        if (error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_EXISTS &&
            GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES) return;
        // Existing directory or file at the target: try another name.
    }
}

inline void MigrateLegacyLogs(const std::wstring& storage, const std::wstring& logs,
                               const std::wstring& executable_dir, const std::wstring& asi_dir) {
    // Collect first so renames cannot invalidate the active enumeration.
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW item{};
    HANDLE search = FindFirstFileW((storage + L"\\*.log").c_str(), &item);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name(item.cFileName);
            if (!(item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && name.size() >= 4 &&
                _wcsicmp(name.c_str() + name.size() - 4, L".log") == 0)
                names.push_back(name);
        } while (FindNextFileW(search, &item));
        FindClose(search);
    }
    for (const auto& name : names) MoveLegacyLog(storage, logs, name);
    MoveLegacyLog(storage, logs, L"button-sync-diagnostics.txt");
    MoveLegacyLog(storage, logs, L"RefunctUtility.startup.txt");
    // Older builds placed the startup trace beside the ASI. The retired
    // restart helper also wrote beside its executable. Do not move unrelated
    // game/loader logs from these external directories.
    for (const auto* directory : {&asi_dir, &executable_dir}) {
        MoveLegacyLog(*directory, logs, L"RefunctUtility.startup.txt");
        MoveLegacyLog(*directory, logs, L"RefunctUtilityRestart.log");
    }
}

// Only log files in the dedicated directory are eligible. Never traverse a
// subdirectory or junction, and never delete settings or unrelated text files.
inline bool ClearDirectory(const std::wstring& logs) {
    if (logs.empty()) return false;
    const DWORD attributes = GetFileAttributesW(logs.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    auto ends = [](const std::wstring& name, const wchar_t* suffix) {
        const size_t n = std::wcslen(suffix);
        return name.size() >= n && _wcsicmp(name.c_str() + name.size() - n, suffix) == 0;
    };
    auto diagnostic = [&](const std::wstring& name, const wchar_t* stem) {
        const std::wstring base(stem);
        if (_wcsicmp(name.c_str(), (base + L".txt").c_str()) == 0) return true;
        const auto prefix = base + L".legacy-";
        if (!ends(name, L".txt") || name.size() <= prefix.size() + 4 ||
            _wcsicmp(name.substr(0, prefix.size()).c_str(), prefix.c_str()) != 0) return false;
        for (size_t i = prefix.size(); i < name.size() - 4; ++i)
            if (name[i] < L'0' || name[i] > L'9') return false;
        return true;
    };
    WIN32_FIND_DATAW item{};
    HANDLE search = FindFirstFileW((logs + L"\\*").c_str(), &item);
    if (search == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    struct SearchGuard {
        HANDLE handle;
        ~SearchGuard() { FindClose(handle); }
    } guard{search};
    bool success = true;
    do {
        if (item.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        const std::wstring name(item.cFileName);
        if (!ends(name, L".log") && !diagnostic(name, L"RefunctUtility.startup") &&
            !diagnostic(name, L"button-sync-diagnostics")) continue;
        if (!DeleteFileW((logs + L"\\" + name).c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND)
            success = false;
    } while (FindNextFileW(search, &item));
    if (GetLastError() != ERROR_NO_MORE_FILES) success = false;
    return success;
}

inline std::wstring Prepare(HMODULE asi) {
    const auto executable = ModulePath(nullptr);
    const auto storage = StorageDirectory(executable);
    const auto logs = Directory(storage);
    if (!EnsureDirectory(storage) || !EnsureDirectory(logs)) return {};
    MigrateLegacyLogs(storage, logs, ParentDirectory(executable), ParentDirectory(ModulePath(asi)));
    return logs;
}

} // namespace refunct::runtime_logs

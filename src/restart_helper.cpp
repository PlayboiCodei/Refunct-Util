// Refunct Utility restart helper. Compiled as a separate WIN32 subsystem EXE.
// The game sends WM_CLOSE to itself; this process waits for the old game to
// *finish exiting* before starting a new copy, preventing double instances.
#include <Windows.h>
#include <shellapi.h>
#include <cerrno>
#include <cwchar>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::wstring HelperDirectory() {
    std::wstring path(MAX_PATH, L'\0');
    while (path.size() <= 32768) {
        const DWORD count = GetModuleFileNameW(nullptr, path.data(),
                                               static_cast<DWORD>(path.size()));
        if (!count) return {};
        if (count < path.size()) {
            path.resize(count);
            const auto slash = path.find_last_of(L"\\/");
            return slash == std::wstring::npos ? std::wstring{} : path.substr(0, slash);
        }
        path.resize(path.size() * 2);
    }
    return {};
}

void LogFailure(std::wstring_view message, DWORD error = 0) {
    const std::wstring directory = HelperDirectory();
    if (directory.empty()) return;
    const std::wstring path = directory + L"\\RefunctUtilityRestart.log";
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    std::wstring line(message);
    if (error) line += L" (Win32 error " + std::to_wstring(error) + L")";
    line += L"\r\n";
    DWORD written = 0;
    WriteFile(file, line.data(), static_cast<DWORD>(line.size() * sizeof(wchar_t)),
              &written, nullptr);
    CloseHandle(file);
}

bool ParseProcessId(const wchar_t* arg, DWORD& result) {
    if (!arg || !*arg || *arg == L'-') return false;
    wchar_t* end = nullptr;
    errno = 0;
    const unsigned long parsed = std::wcstoul(arg, &end, 10);
    if (errno == ERANGE || end == arg || *end || !parsed) return false;
    result = static_cast<DWORD>(parsed);
    return true;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        LogFailure(L"Restart helper could not parse command line", GetLastError());
        return 1;
    }
    // Explicit fixed argument layout avoids guessing at arbitrary arguments.
    const bool shape_valid = argc == 9 &&
        std::wcscmp(argv[1], L"--parent") == 0 &&
        std::wcscmp(argv[3], L"--exe") == 0 &&
        std::wcscmp(argv[5], L"--cwd") == 0 &&
        std::wcscmp(argv[7], L"--cmdline") == 0;
    DWORD original_pid = 0;
    if (!shape_valid || !ParseProcessId(shape_valid ? argv[2] : nullptr, original_pid)) {
        LocalFree(argv);
        LogFailure(L"Restart helper received invalid arguments");
        return 2;
    }
    const std::wstring game_path = argv[4];
    const std::wstring game_dir = argv[6];
    const std::wstring original_command = argv[8];
    LocalFree(argv);
    if (game_path.empty() || game_dir.empty() || original_command.empty()) {
        LogFailure(L"Restart helper received empty game path, cwd or command line");
        return 3;
    }
    const DWORD attributes = GetFileAttributesW(game_path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        LogFailure(L"Restart helper cannot locate Refunct executable", GetLastError());
        return 4;
    }

    // OpenProcess is safe here: no injection, memory writes or manipulation of
    // the game. Only SYNCHRONIZE is needed to wait for its normal termination.
    HANDLE old_game = OpenProcess(SYNCHRONIZE, FALSE, original_pid);
    if (old_game) {
        const DWORD result = WaitForSingleObject(old_game, 120000);
        const DWORD error = result == WAIT_FAILED ? GetLastError() : 0;
        CloseHandle(old_game);
        if (result != WAIT_OBJECT_0) {
            LogFailure(result == WAIT_TIMEOUT
                           ? L"Refunct did not close within 120 seconds; no relaunch attempted"
                           : L"Restart helper could not wait for Refunct to exit", error);
            return 5;
        }
    } else if (GetLastError() != ERROR_INVALID_PARAMETER) {
        // ERROR_INVALID_PARAMETER indicates the old PID has already exited;
        // other errors (e.g. access denied) cannot safely imply that.
        LogFailure(L"Restart helper cannot open old Refunct process", GetLastError());
        return 6;
    }
    Sleep(350); // Allow the OS and Steam to release the old game instance.

    std::vector<wchar_t> mutable_command(original_command.begin(), original_command.end());
    mutable_command.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION new_game{};
    if (!CreateProcessW(game_path.c_str(), mutable_command.data(),
                        nullptr, nullptr, FALSE, 0, nullptr,
                        game_dir.c_str(), &startup, &new_game)) {
        LogFailure(L"Restart helper failed to relaunch Refunct", GetLastError());
        return 7;
    }
    CloseHandle(new_game.hThread);
    CloseHandle(new_game.hProcess);
    return 0;
}

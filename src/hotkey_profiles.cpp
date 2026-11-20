#include "refunct/internal_api.hpp"
#include "refunct/features/hotkey_profiles.hpp"
#include <charconv>
#include <map>
#include <string_view>

namespace refunct {
namespace {
using Values = std::map<std::string, int>;
std::vector<std::wstring> s_names;
std::wstring s_active = L"Default";
bool s_ready = false;

std::wstring Root() { return g_storage_dir + L"\\HotkeyProfiles"; }
std::wstring Path(const std::wstring& name) {
    if (g_storage_dir.empty() || !ValidProfileName(name)) return L"";
    return Root() + L"\\" + name + L".ini";
}
bool Exists(const std::wstring& name) { return IsRegularFile(Path(name)); }
// Stable action names, not enum indices, are persisted in each profile.
Values Capture() {
    Values values;
    for (size_t i = 0; i < kActionCount; ++i) {
        const std::wstring name(kActions[i].ini_name);
        values.emplace(std::string(name.begin(), name.end()), g_keys[i].load());
    }
    return values;
}
bool Validate(const Values& values) {
    if (values.size() != kActionCount) return false;
    std::array<bool, 256> used{};
    for (const auto& [name, ignored] : Capture()) {
        const auto it = values.find(name);
        if (it == values.end()) return false;
        const int key = it->second;
        if (key == 0) continue; // Multiple unbound actions are allowed.
        if (key < 0 || key >= static_cast<int>(used.size()) ||
            !IsBindable(key) || used[key]) return false;
        used[key] = true;
    }
    // F5 must remain available as the emergency menu key.
    return values.at("Menu") != 0 || !used[VK_F5];
}

bool WriteProfile(const std::wstring& name, const Values& values) {
    const auto path = Path(name);
    if (path.empty() || !Validate(values) || !EnsureDirectory(Root())) return false;
    std::string contents = "[HotkeyProfile]\r\nVersion=1\r\n";
    for (const auto& [key, value] : values)
        contents += key + "=" + std::to_string(value) + "\r\n";
    if (contents.size() > 32768) return false;
    const std::wstring temp = path + L".tmp";
    HANDLE h = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(h, contents.data(), static_cast<DWORD>(contents.size()),
                              &written, nullptr) != 0 && written == contents.size() &&
                    FlushFileBuffers(h) != 0;
    CloseHandle(h);
    if (ok && MoveFileExW(temp.c_str(), path.c_str(),
                          MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    DeleteFileW(temp.c_str());
    return false;
}

bool ReadProfile(const std::wstring& name, Values& out) {
    const auto path = Path(name);
    if (path.empty()) return false;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const DWORD length = GetFileSize(h, nullptr);
    if (length == INVALID_FILE_SIZE || length == 0 || length > 32768) {
        CloseHandle(h);
        return false;
    }
    std::string contents(length, '\0');
    DWORD read = 0;
    const bool ok = ReadFile(h, contents.data(), length, &read, nullptr) != 0 &&
                    read == length;
    CloseHandle(h);
    if (!ok) return false;
    Values values;
    size_t start = 0;
    bool header = false;
    bool version = false;
    while (start < contents.size()) {
        const auto end = contents.find('\n', start);
        const std::string_view line(contents.data() + start,
            (end == std::string::npos ? contents.size() : end) - start);
        auto text = line;
        if (!text.empty() && text.back() == '\r') text.remove_suffix(1);
        if (!text.empty()) {
            if (!header) {
                if (text != "[HotkeyProfile]") return false;
                header = true;
            } else {
                const size_t pos = text.find('=');
                if (pos == std::string_view::npos || pos == 0) return false;
                if (text.substr(0, pos) == "Version") {
                    if (version || text.substr(pos + 1) != "1") return false;
                    version = true;
                } else {
                    int value = 0;
                    const auto raw = text.substr(pos + 1);
                    const auto result = std::from_chars(raw.data(), raw.data() + raw.size(), value);
                    if (result.ec != std::errc{} || result.ptr != raw.data() + raw.size() ||
                        !values.emplace(std::string(text.substr(0, pos)), value).second)
                        return false;
                }
            }
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    if (!header || !version || !Validate(values)) return false;
    out = std::move(values);
    return true;
}

void Apply(const Values& target) {
    for (size_t i = 0; i < kActionCount; ++i) {
        const std::wstring name(kActions[i].ini_name);
        g_keys[i].store(target.at(std::string(name.begin(), name.end())));
    }
}

void Scan() {
    s_names.clear();
    const auto root = Root();
    WIN32_FIND_DATAW file{};
    HANDLE find = FindFirstFileW((root + L"\\*.ini").c_str(), &file);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (file.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const std::wstring filename(file.cFileName);
            if (filename.size() <= 4 || _wcsicmp(filename.c_str() + filename.size() - 4,
                                                  L".ini") != 0) continue;
            const std::wstring name = filename.substr(0, filename.size() - 4);
            if (ValidProfileName(name)) s_names.push_back(name);
        } while (FindNextFileW(find, &file));
        FindClose(find);
    }
    std::sort(s_names.begin(), s_names.end(), [](const auto& a, const auto& b) {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    });
}
bool Found(const std::wstring& name) {
    return std::any_of(s_names.begin(), s_names.end(), [&](const auto& existing) {
        return _wcsicmp(existing.c_str(), name.c_str()) == 0;
    });
}
void PersistActive() {
    SaveSettings();
}
} // namespace

void InitialiseHotkeyProfiles() {
    if (s_ready || g_storage_dir.empty() || !EnsureDirectory(Root())) return;
    Scan();
    wchar_t saved[64]{};
    GetPrivateProfileStringW(L"HotkeyProfiles", L"ActiveProfile",
                             L"Default", saved, static_cast<DWORD>(std::size(saved)),
                             g_settings_path.c_str());
    s_active = L"Default";
    if (ValidProfileName(saved) && Found(saved)) {
        for (const auto& name : s_names)
            if (_wcsicmp(name.c_str(), saved) == 0) { s_active = name; break; }
    }
    // Recover legacy configurations in which an unbound Menu shadowed an F5 action.
    bool repaired = false;
    if (g_keys[static_cast<size_t>(Action::Menu)].load() == 0) {
        for (auto& key : g_keys) {
            if (key.load() == VK_F5) { key.store(0); repaired = true; }
        }
    }
    if (repaired) Log(L"Hotkey profiles: released F5 for emergency menu access");
    if (!Exists(L"Default") && !WriteProfile(L"Default", Capture())) {
        Log(L"Hotkey profiles: could not create Default");
        return;
    }
    // settings.ini is already the current live state at launch; absorb any
    // changes saved during the previous session, including pre-profile installs.
    if (!WriteProfile(s_active, Capture())) {
        Log(L"Hotkey profiles: could not initialise active profile");
        return;
    }
    Scan();
    s_ready = true;
    if (repaired) SaveSettings();
    else QueueSettingsValue(L"HotkeyProfiles", L"ActiveProfile", s_active.c_str());
}
bool HotkeyProfilesReady() { return s_ready; }
const std::vector<std::wstring>& HotkeyProfileNames() { return s_names; }
const std::wstring& ActiveHotkeyProfile() { return s_active; }
void RefreshHotkeyProfiles() {
    if (!s_ready) InitialiseHotkeyProfiles();
    if (s_ready) Scan();
}
bool SaveActiveHotkeyProfile() {
    if (!s_ready) return false;
    if (!WriteProfile(s_active, Capture())) {
        Notify(L"Could not save hotkey profile");
        return false;
    }
    return true;
}
bool SelectHotkeyProfile(const std::wstring& name) {
    if (!s_ready || !ValidProfileName(name) || !Exists(name)) return false;
    if (_wcsicmp(name.c_str(), s_active.c_str()) == 0) return true;
    Values target;
    if (!ReadProfile(name, target)) {
        Notify(L"Hotkey profile is incomplete or damaged");
        return false;
    }
    if (!SaveActiveHotkeyProfile()) return false;
    Apply(target);
    s_active = name;
    PersistActive();
    Notify(L"Hotkey profile: " + name);
    return true;
}
bool CreateHotkeyProfile(const std::wstring& name) {
    if (!s_ready || !ValidProfileName(name) || Found(name) || Exists(name)) {
        Notify(L"Invalid or duplicate hotkey profile name");
        return false;
    }
    if (!SaveActiveHotkeyProfile() || !WriteProfile(name, Capture())) {
        Notify(L"Could not create hotkey profile");
        return false;
    }
    s_active = name;
    Scan();
    PersistActive();
    Notify(L"Created hotkey profile: " + name);
    return true;
}
bool RenameActiveHotkeyProfile(const std::wstring& name) {
    if (!s_ready || !ValidProfileName(name) || Found(name) || Exists(name) ||
        _wcsicmp(s_active.c_str(), L"Default") == 0) {
        Notify(L"Cannot rename Default, or name is already taken/invalid");
        return false;
    }
    if (!SaveActiveHotkeyProfile() ||
        !MoveFileW(Path(s_active).c_str(), Path(name).c_str())) {
        Notify(L"Could not rename hotkey profile");
        return false;
    }
    s_active = name;
    Scan();
    PersistActive();
    Notify(L"Renamed hotkey profile: " + name);
    return true;
}
bool DeleteActiveHotkeyProfile() {
    if (!s_ready || _wcsicmp(s_active.c_str(), L"Default") == 0) {
        Notify(L"Default hotkey profile cannot be deleted");
        return false;
    }
    Values defaults;
    if (!ReadProfile(L"Default", defaults)) {
        Notify(L"Default hotkey profile is unavailable");
        return false;
    }
    const std::wstring remove = Path(s_active);
    if (!DeleteFileW(remove.c_str())) {
        Notify(L"Could not delete hotkey profile");
        return false;
    }
    Apply(defaults);
    s_active = L"Default";
    Scan();
    PersistActive();
    Notify(L"Deleted profile; Default hotkey active");
    return true;
}
bool ReloadActiveHotkeyProfile() {
    if (!s_ready) return false;
    Values target;
    if (!ReadProfile(s_active, target)) {
        Notify(L"Saved hotkey profile is invalid");
        return false;
    }
    Apply(target);
    SaveSettings();
    Notify(L"Reloaded hotkey profile: " + s_active);
    return true;
}
} // namespace refunct

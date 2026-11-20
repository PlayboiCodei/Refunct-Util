#include "refunct/internal_api.hpp"
#include "refunct/features/world_profiles.hpp"
#include <charconv>
#include <map>
#include <string_view>
#include <optional>

namespace refunct {
namespace {
using Values = std::map<std::string, int>;
std::vector<std::wstring> s_names;
std::wstring s_active = L"Default";
bool s_ready = false;

std::wstring Root() { return g_storage_dir + L"\\WorldProfiles"; }
std::wstring Path(const std::wstring& name) {
    if (g_storage_dir.empty() || !ValidProfileName(name)) return L"";
    return Root() + L"\\" + name + L".ini";
}
bool Exists(const std::wstring& name) { return IsRegularFile(Path(name)); }
// World profiles store preferences, never native progress or session-only edits.
constexpr const char* removalKeys[] = {"RemoveTubes", "RemoveButtons", "RemoveJumpPads",
    "RemoveCubes", "RemovePlatforms", "RemoveTiles"};
static_assert(std::size(removalKeys) == static_cast<size_t>(WorldObjectKind::Count));
std::optional<Values> s_live_baseline;
Values Capture() {
    Values v{{"AnimateSections", g_animate_section_changes},
        {"SyncButtons", g_buttons_sync_with_sections},
        {"SectionSpeed", GetSectionSpeedPercent()},
        {"TubeSpeed", GetTubeTravelSpeedPercent()},
        {"PlatformSpeed", GetPlatformSpeedPercent()},
        {"JumpPadHeight", GetJumpPadHeightPercent()},
        {"DisableButtonTiles", ButtonTileSelectionDisabledPreference()},
        {"DisableTouchTiles", TileSelectionOnContactDisabledPreference()},
        {"RemoveWater", IsAllWaterRemoved()}, {"HideOcean", IsWaterRemoved()},
        {"DisableSwimming", IsWaterSwimmingDisabled()}, {"WaterHeight", GetWaterHeightOffset()}};
    for (size_t i = 0; i < std::size(removalKeys); ++i)
        v.emplace(removalKeys[i], IsWorldObjectRemoved(static_cast<WorldObjectKind>(i)));
    return v;
}
bool Validate(const Values& v) {
    const auto expected = Capture();
    if (v.size() != expected.size()) return false;
    for (const auto& [key, ignored] : expected) {
        const auto it = v.find(key);
        if (it == v.end()) return false;
        const int value = it->second;
        if (key == "SectionSpeed" || key == "TubeSpeed" || key == "PlatformSpeed" || key == "JumpPadHeight") {
            if (value < 25) return false;
        } else if (key == "WaterHeight") {
            if (value < -100000 || value > 100000) return false;
        } else if (value != 0 && value != 1) return false;
    }
    return true;
}

bool WriteProfile(const std::wstring& name, const Values& values) {
    const auto path = Path(name);
    if (path.empty() || !EnsureDirectory(Root())) return false;
    std::string contents = "[WorldProfile]\r\nVersion=1\r\n";
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
                if (text != "[WorldProfile]") return false;
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

// The only fallible controls require native hooks. Check capabilities before
// touching other preferences, and use the original gameplay setters.
bool Apply(const Values& target) {
    const Values current = Capture();
    const auto value = [&](const char* key) { return target.at(key); };
    const auto changed = [&](const char* key) { return value(key) != current.at(key); };
    if (((changed("SyncButtons") && value("SyncButtons")) || changed("DisableButtonTiles") ||
         changed("DisableTouchTiles")) && !g_section_button_contact_hook_available.load()) {
        Notify(L"World profile needs the native contact hook; no settings changed");
        return false;
    }
    if (changed("SyncButtons") && !SetButtonsSyncWithSections(value("SyncButtons") != 0)) return false;
    if (changed("DisableButtonTiles") && !ToggleButtonTileSelection()) return false;
    if (changed("DisableTouchTiles") && !ToggleTileSelectionOnContact()) return false;
    if (changed("AnimateSections")) {
        g_animate_section_changes = value("AnimateSections") != 0;
        if (!g_animate_section_changes) {
            FinishSectionPlacementAnimations();
            g_recording_tainted = true;
            if (!g_pb_cheats_allowed_for_run) g_run_pb_eligible = false;
        }
    }
    SetSectionSpeedProfilePercent(value("SectionSpeed"));
    SetTubeTravelSpeedProfilePercent(value("TubeSpeed"));
    SetPlatformSpeedProfilePercent(value("PlatformSpeed"));
    SetJumpPadHeightProfilePercent(value("JumpPadHeight"));
    std::array<bool, std::size(removalKeys)> removals{};
    for (size_t i = 0; i < removals.size(); ++i) removals[i] = value(removalKeys[i]) != 0;
    SetWorldObjectRemovalProfile(removals);
    if (changed("HideOcean")) SetWaterRemoved(value("HideOcean") != 0);
    if (changed("DisableSwimming")) SetWaterSwimmingDisabled(value("DisableSwimming") != 0);
    if (changed("WaterHeight")) SetWaterHeightOffset(value("WaterHeight"));
    if (changed("RemoveWater")) SetAllWaterRemoved(value("RemoveWater") != 0);
    SaveSettings();
    return true;
}

// Automatic persistence resets must not overwrite a saved profile. Only
// save outgoing live edits if they differ from the last known baseline.
bool SaveOutgoing() {
    const auto current = Capture();
    if (s_live_baseline && *s_live_baseline == current) return true;
    if (!WriteProfile(s_active, current)) {
        Notify(L"Could not save world profile");
        return false;
    }
    s_live_baseline = current;
    return true;
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
    QueueSettingsValue(L"WorldProfiles", L"ActiveProfile", s_active.c_str());
}
} // namespace

void InitialiseWorldProfiles() {
    if (s_ready || g_storage_dir.empty() || !EnsureDirectory(Root())) return;
    Scan();
    wchar_t saved[64]{};
    GetPrivateProfileStringW(L"WorldProfiles", L"ActiveProfile",
                             L"Default", saved, static_cast<DWORD>(std::size(saved)),
                             g_settings_path.c_str());
    s_active = L"Default";
    if (ValidProfileName(saved) && Found(saved)) {
        for (const auto& name : s_names)
            if (_wcsicmp(name.c_str(), saved) == 0) { s_active = name; break; }
    }
    // Never overwrite an explicit saved snapshot at startup.
    const auto current = Capture();
    if ((!Exists(L"Default") && !WriteProfile(L"Default", current)) ||
        (!Exists(s_active) && !WriteProfile(s_active, current))) {
        Log(L"World profiles: could not initialise active profile");
        return;
    }
    Scan();
    s_live_baseline = current;
    s_ready = true;
    PersistActive();
}
void NoteWorldProfilePersistenceReset() {
    if (s_ready) s_live_baseline = Capture();
}
const std::vector<std::wstring>& WorldProfileNames() { return s_names; }
const std::wstring& ActiveWorldProfile() { return s_active; }
void RefreshWorldProfiles() {
    if (!s_ready) InitialiseWorldProfiles();
    if (s_ready) Scan();
}
bool SaveActiveWorldProfile() {
    if (!s_ready) return false;
    if (!WriteProfile(s_active, Capture())) {
        Notify(L"Could not save world profile");
        return false;
    }
    s_live_baseline = Capture();
    return true;
}
bool SelectWorldProfile(const std::wstring& name) {
    if (!s_ready || !ValidProfileName(name) || !Exists(name)) return false;
    if (_wcsicmp(name.c_str(), s_active.c_str()) == 0) return true;
    Values target;
    if (!ReadProfile(name, target)) {
        Notify(L"World profile is incomplete or damaged");
        return false;
    }
    if (!SaveOutgoing() || !Apply(target)) return false;
    s_live_baseline = Capture();
    s_active = name;
    PersistActive();
    Notify(L"World profile: " + name);
    return true;
}
bool CreateWorldProfile(const std::wstring& name) {
    if (!s_ready || !ValidProfileName(name) || Found(name) || Exists(name)) {
        Notify(L"Invalid or duplicate world profile name");
        return false;
    }
    if (!SaveOutgoing() || !WriteProfile(name, Capture())) {
        Notify(L"Could not create world profile");
        return false;
    }
    s_active = name;
    s_live_baseline = Capture();
    Scan();
    PersistActive();
    Notify(L"Created world profile: " + name);
    return true;
}
bool RenameActiveWorldProfile(const std::wstring& name) {
    if (!s_ready || !ValidProfileName(name) || Found(name) || Exists(name) ||
        _wcsicmp(s_active.c_str(), L"Default") == 0) {
        Notify(L"Cannot rename Default, or name is already taken/invalid");
        return false;
    }
    if (!SaveOutgoing() ||
        !MoveFileW(Path(s_active).c_str(), Path(name).c_str())) {
        Notify(L"Could not rename world profile");
        return false;
    }
    s_active = name;
    s_live_baseline = Capture();
    Scan();
    PersistActive();
    Notify(L"Renamed world profile: " + name);
    return true;
}
bool DeleteActiveWorldProfile() {
    if (!s_ready || _wcsicmp(s_active.c_str(), L"Default") == 0) {
        Notify(L"Default world profile cannot be deleted");
        return false;
    }
    Values defaults;
    if (!ReadProfile(L"Default", defaults)) {
        Notify(L"Default world profile is unavailable");
        return false;
    }
    const auto previous = Capture();
    if (!Apply(defaults)) return false;
    const std::wstring remove = Path(s_active);
    if (!DeleteFileW(remove.c_str())) {
        if (!Apply(previous)) Notify(L"Could not restore previous world settings; reload profile");
        Notify(L"Could not delete world profile");
        return false;
    }
    s_live_baseline = Capture();
    s_active = L"Default";
    Scan();
    PersistActive();
    Notify(L"Deleted profile; Default world active");
    return true;
}
bool ReloadActiveWorldProfile() {
    if (!s_ready) return false;
    Values target;
    if (!ReadProfile(s_active, target)) {
        Notify(L"Saved world profile is invalid");
        return false;
    }
    if (!Apply(target)) return false;
    s_live_baseline = Capture();
    Notify(L"Reloaded world profile: " + s_active);
    return true;
}
} // namespace refunct

#include "refunct/internal_api.hpp"
#include "refunct/features/hud_profiles.hpp"
#include <type_traits>
#include <charconv>
#include <map>
#include <string_view>

namespace refunct {
namespace {
using Values = std::map<std::string, int>;
std::vector<std::wstring> s_names;
std::wstring s_active = L"Default";
bool s_ready = false;

std::wstring Root() { return g_storage_dir + L"\\HUDProfiles"; }
std::wstring Path(const std::wstring& name) {
    if (g_storage_dir.empty() || !ValidProfileName(name)) return L"";
    return Root() + L"\\" + name + L".ini";
}
bool Exists(const std::wstring& name) { return IsRegularFile(Path(name)); }
// Only presentation preferences. No PB data, active split/ghost profile,
// comparison data source, actual inputs, hotkeys or world settings.
// One schema defines capture, validation and application, preventing drift.
template<class Visitor> void VisitHudFields(Visitor&& visit) {
    auto field = [&](const std::string& key, auto& ref, int min, int max, int step = 1) {
        visit(key, ref, min, max, step);
    };
    field("Coordinates", g_show_coordinates, 0, 1);
    field("Timer", g_show_timer, 0, 1);
    field("ShowFPS", g_show_fps, 0, 1);
    field("ShowGameTime", g_show_game_time_hud, 0, 1);
    field("ShowLocalTime", g_show_local_time_hud, 0, 1);
    field("ShowSpeed", g_show_speed, 0, 1);
    field("ShowCompletionPercent", g_show_completion_percent, 0, 1);
    field("ShowButtonNumber", g_show_button_number, 0, 1);
    field("ShowKeyDisplay", g_show_key_display, 0, 1);
    field("ShowPracticeStatus", g_show_practice_status_hud, 0, 1);
    field("ShowRecordStatus", g_show_record_status_hud, 0, 1);
    field("ShowEnabledModules", g_show_enabled_modules, 0, 1);
    field("ShowActiveCheats", g_show_active_cheats, 0, 1);
    field("ShowActiveCustomisations", g_show_active_customisations, 0, 1);
    field("ShowMenuDescriptions", g_show_menu_descriptions, 0, 1);
    field("ButtonSplits", g_show_splits, 0, 1);
    field("ShowPBRun", g_show_pb_run_hud, 0, 1);
    field("ShowSumOfBest", g_show_sum_of_best_hud, 0, 1);
    field("ShowLastButtonSplit", g_show_last_button_split_hud, 0, 1);
    field("ShowCompareTime", g_show_compare_hud, 0, 1);
    field("ShowBestSegmentTime", g_show_best_segment_time_hud, 0, 1);
    field("ShowPBSplitTime", g_show_pb_split_time_hud, 0, 1);
    field("SplitComparePopup", g_show_compare_popup, 0, 1);
    field("SplitComparePopupTime", g_compare_popup_show_time, 0, 1);
    field("SplitComparePopupHeader", g_compare_popup_show_header, 0, 1);
    field("SplitButtonNumber", g_show_split_button, 0, 1);
    field("SplitTotalTime", g_show_split_total_time, 0, 1);
    field("SplitFastestDelta", g_show_split_fastest_delta, 0, 1);
    field("SplitFastestDeltaTime", g_show_fastest_delta_time, 0, 1);
    field("SplitFastestDeltaLabel", g_show_fastest_delta_label, 0, 1);
    field("CubeDisplayMode", g_cube_display_mode, 0, 2);
    field("TileDisplayMode", g_tile_display_mode, 0, 2);
    field("CompareTimeView", g_compare_hud_view, 0, 1);
    field("BestSegmentTimeView", g_best_segment_time_view, 0, 1);
    field("PBSplitTimeView", g_pb_split_time_view, 0, 1);
    field("PBGainLossDisplay", g_pb_gain_loss_display, 0, 4);
    field("SectionVsPBDisplay", g_section_vs_pb_display, 0, 4);
    field("SectionVsFileDisplay", g_section_vs_file_display, 0, 4);
    field("CompareMode", g_compare_mode, 0, 1);
    field("SplitSegmentDisplayMode", g_segment_row_mode, 0, 2);
    field("SplitPBDisplayMode", g_pb_row_mode, 0, 2);
    field("F5MenuPosition", g_menu_position, 1, 4);
    for (size_t i = 0; i < kHudElementCount; ++i) {
        const std::wstring wide(kHudPositionKeys[i]);
        const std::string key(wide.begin(), wide.end());
        field(key, g_hud_corners[i], 0, static_cast<int>(HudCorner::Center));
        field(key + "ColourMode", g_hud_colour_modes[i], 0, kHudColourModeCount - 1);
        field(key + "CustomRGB", g_hud_custom_colours[i], 0, 0xFFFFFF);
        field(key + "SizePercent", g_hud_size_percent[i], kHudMinSizePercent,
              kHudMaxSizePercent, kHudSizeStepPercent);
        field(key + "ShowHeader", g_hud_show_headers[i], 0, 1);
    }
}
Values Capture() {
    Values values;
    VisitHudFields([&](const std::string& key, const auto& ref, int, int, int) {
        values.emplace(key, static_cast<int>(ref));
    });
    return values;
}
bool Validate(const Values& values) {
    size_t count = 0;
    bool valid = true;
    VisitHudFields([&](const std::string& key, const auto&, int min, int max, int step) {
        ++count;
        const auto it = values.find(key);
        if (it == values.end() || it->second < min || it->second > max ||
            (it->second - min) % step != 0) valid = false;
    });
    return valid && values.size() == count;
}
void Apply(const Values& target) {
    VisitHudFields([&](const std::string& key, auto& ref, int, int, int) {
        using T = std::remove_reference_t<decltype(ref)>;
        ref = static_cast<T>(target.at(key));
    });
}
bool WriteProfile(const std::wstring& name, const Values& values) {
    const auto path = Path(name);
    if (path.empty() || !EnsureDirectory(Root())) return false;
    std::string contents = "[HUDProfile]\r\nVersion=1\r\n";
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
                if (text != "[HUDProfile]") return false;
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

void InitialiseHudProfiles() {
    if (s_ready || g_storage_dir.empty() || !EnsureDirectory(Root())) return;
    Scan();
    wchar_t saved[64]{};
    GetPrivateProfileStringW(L"HUDProfiles", L"ActiveProfile",
                             L"Default", saved, static_cast<DWORD>(std::size(saved)),
                             g_settings_path.c_str());
    if (ValidProfileName(saved) && Found(saved)) {
        for (const auto& name : s_names)
            if (_wcsicmp(name.c_str(), saved) == 0) { s_active = name; break; }
    }
    else s_active = L"Default";
    // settings.ini preserves the live HUD across relaunches. Create missing
    // snapshots, but do not overwrite an existing explicit saved snapshot.
    if (!Exists(L"Default") && !WriteProfile(L"Default", Capture())) {
        Log(L"HUD profiles: could not create Default profile");
        return;
    }
    if (!Exists(s_active) && !WriteProfile(s_active, Capture())) {
        Log(L"HUD profiles: could not initialise active profile");
        return;
    }
    Scan();
    s_ready = true;
    PersistActive();
}
bool HudProfilesReady() { return s_ready; }
const std::vector<std::wstring>& HudProfileNames() { return s_names; }
const std::wstring& ActiveHudProfile() { return s_active; }
void RefreshHudProfiles() {
    if (!s_ready) InitialiseHudProfiles();
    if (s_ready) Scan();
}
bool SaveActiveHudProfile() {
    if (!s_ready) return false;
    if (!WriteProfile(s_active, Capture())) {
        Notify(L"Could not save HUD profile");
        return false;
    }
    return true;
}
bool SelectHudProfile(const std::wstring& name) {
    if (!s_ready || !ValidProfileName(name) || !Exists(name)) return false;
    if (_wcsicmp(name.c_str(), s_active.c_str()) == 0) return true;
    Values target;
    if (!ReadProfile(name, target)) {
        Notify(L"HUD profile is incomplete or damaged");
        return false;
    }
    if (!SaveActiveHudProfile()) return false;
    Apply(target);
    s_active = name;
    PersistActive();
    Notify(L"HUD profile: " + name);
    return true;
}
bool CreateHudProfile(const std::wstring& name) {
    if (!s_ready || !ValidProfileName(name) || Found(name) || Exists(name)) {
        Notify(L"Invalid or duplicate HUD profile name");
        return false;
    }
    if (!SaveActiveHudProfile() || !WriteProfile(name, Capture())) {
        Notify(L"Could not create HUD profile");
        return false;
    }
    s_active = name;
    Scan();
    PersistActive();
    Notify(L"Created HUD profile: " + name);
    return true;
}
bool RenameActiveHudProfile(const std::wstring& name) {
    if (!s_ready || !ValidProfileName(name) || Found(name) || Exists(name) ||
        _wcsicmp(s_active.c_str(), L"Default") == 0) {
        Notify(L"Cannot rename Default, or name is already taken/invalid");
        return false;
    }
    if (!SaveActiveHudProfile() ||
        !MoveFileW(Path(s_active).c_str(), Path(name).c_str())) {
        Notify(L"Could not rename HUD profile");
        return false;
    }
    s_active = name;
    Scan();
    PersistActive();
    Notify(L"Renamed HUD profile: " + name);
    return true;
}
bool DeleteActiveHudProfile() {
    if (!s_ready || _wcsicmp(s_active.c_str(), L"Default") == 0) {
        Notify(L"Default HUD profile cannot be deleted");
        return false;
    }
    Values defaults;
    if (!ReadProfile(L"Default", defaults)) {
        Notify(L"Default HUD profile is unavailable");
        return false;
    }
    const std::wstring remove = Path(s_active);
    if (!DeleteFileW(remove.c_str())) {
        Notify(L"Could not delete HUD profile");
        return false;
    }
    Apply(defaults);
    s_active = L"Default";
    Scan();
    PersistActive();
    Notify(L"Deleted profile; Default HUD active");
    return true;
}
bool ReloadActiveHudProfile() {
    if (!s_ready) return false;
    Values target;
    if (!ReadProfile(s_active, target)) {
        Notify(L"Saved HUD profile is invalid");
        return false;
    }
    Apply(target);
    SaveSettings();
    Notify(L"Reloaded HUD profile: " + s_active);
    return true;
}
} // namespace refunct

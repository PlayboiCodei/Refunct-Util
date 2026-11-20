#include "refunct/crosshair.hpp"
#include "refunct/internal_api.hpp"
#include "refunct/features/customisation_profiles.hpp"
#include "refunct/features/button_colours.hpp"
#include <charconv>
#include <map>
#include <string_view>

namespace refunct {
namespace {
using Values = std::map<std::string, int>;
std::vector<std::wstring> s_names;
std::wstring s_active = L"Default";
bool s_ready = false;

std::wstring Root() { return g_storage_dir + L"\\CustomisationProfiles"; }
std::wstring Path(const std::wstring& name) {
    if (g_storage_dir.empty() || !ValidProfileName(name)) return L"";
    return Root() + L"\\" + name + L".ini";
}
bool Exists(const std::wstring& name) { return IsRegularFile(Path(name)); }
std::uint32_t ParseRgb(const std::wstring& text) {
    std::wstring_view digits(text);
    if (!digits.empty() && digits.front() == L'#') digits.remove_prefix(1);
    if (digits.size() != 6) return 0;
    std::uint32_t rgb = 0;
    for (wchar_t c : digits) {
        const int n = c >= L'0' && c <= L'9' ? c - L'0' :
            c >= L'A' && c <= L'F' ? c - L'A' + 10 :
            c >= L'a' && c <= L'f' ? c - L'a' + 10 : -1;
        if (n < 0) return 0;
        rgb = (rgb << 4) | static_cast<std::uint32_t>(n);
    }
    return rgb;
}
int Flag(bool value) { return value ? 1 : 0; }

// Capture only Customisation-menu state. Gameplay speeds, HUD, PB data,
// hotkeys, checkpoints and split recording are intentionally excluded.
Values Capture() {
    Values v;
    auto add = [&](const char* name, int value) { v.emplace(name, value); };
    auto colour = [&](const char* prefix, bool enabled, const std::wstring& rgb) {
        add((std::string(prefix) + "Enabled").c_str(), Flag(enabled));
        add((std::string(prefix) + "Rgb").c_str(), static_cast<int>(ParseRgb(rgb)));
    };
    add("CameraFov", g_player_fov_degrees);
    add("CrosshairStyle", g_crosshair.style);
    add("CrosshairSize", g_crosshair.size);
    add("CrosshairThickness", g_crosshair.thickness);
    add("CrosshairGap", g_crosshair.gap);
    add("CrosshairOpacity", g_crosshair.opacity);
    add("CrosshairRgb", g_crosshair.rgb);
    for (bool pressed : {false, true}) {
        const char* type = pressed ? "ButtonPressed" : "ButtonActive";
        const char* border = pressed ? "BorderPressed" : "BorderActive";
        colour(type, IsButtonCustomColourEnabled(pressed), GetButtonCustomColourHex(pressed));
        colour(border, IsButtonBorderCustomColourEnabled(pressed), GetButtonBorderColourHex(pressed));
    }
    add("BeaconVisible", Flag(IsButtonBeaconVisible()));
    colour("Beacon", IsButtonBeaconCustomColourEnabled(), GetButtonBeaconColourHex());
    for (bool selected : {false, true}) {
        const char* tile = selected ? "TileSelected" : "TileUnselected";
        const char* wall = selected ? "WallSelected" : "WallUnselected";
        colour(tile, IsTileCustomColourEnabled(selected), GetTileHexColour(selected));
        add((std::string(tile) + "Preset").c_str(), GetTileColourPreset(selected));
        colour(wall, IsWallCustomColourEnabled(selected), GetWallHexColour(selected));
    }
    add("TileRandomRgb", Flag(IsRandomTileRGBEnabled()));
    colour("Tube", IsTubeCustomColourEnabled(), GetTubeCustomColourHex());
    colour("Platform", IsPlatformCustomColourEnabled(), GetPlatformCustomColourHex());
    colour("JumpPad", IsJumpPadCustomColourEnabled(), GetJumpPadCustomColourHex());
    colour("Pillar", IsPillarColourEnabled(), GetPillarColourHex());
    add("PillarRemoved", Flag(ArePillarsRemoved()));
    colour("Water", IsWaterCustomColourEnabled(), GetWaterCustomColourHex());
    add("WaterIntensity", GetWaterColourIntensityPercent());
    colour("Underwater", IsUnderwaterCustomColourEnabled(), GetUnderwaterCustomColourHex());
    add("UnderwaterIntensity", GetUnderwaterColourIntensityPercent());
    add("UnderwaterTintRemoved", Flag(IsUnderwaterTintRemoved()));
    add("WaterSplashesRemoved", Flag(AreWaterSplashesRemoved()));
    add("WaterReflectionsVisible", Flag(AreWaterReflectionsVisible()));
    add("WaterReflectionGeometryVisible", Flag(AreWaterReflectedWallsVisible()));
    add("WaterReflectionButtonsVisible", Flag(AreWaterReflectedButtonsVisible()));
    add("WaterReflectionPipesVisible", Flag(AreWaterReflectedPipesVisible()));
    add("WaterReflectionBeaconsVisible", Flag(AreWaterReflectedBeaconsVisible()));
    add("WaterReflectionJumpPadsVisible", Flag(AreWaterReflectedJumpPadsVisible()));
    add("WaterReflectionCubesVisible", Flag(AreWaterReflectedCubesVisible()));
    add("TimeEnabled", Flag(IsCustomTimeOfDayEnabled()));
    add("TimeMinutes", GetCustomTimeMinutes());
    add("TimeLocked", Flag(IsCustomTimeLocked()));
    add("TimeSpeed", GetCustomTimeSpeedPercent());
    add("SkyDiscVisible", Flag(IsSkyDiscVisible()));
    for (int i = 0; i < 2; ++i) {
        const auto effect = static_cast<SkyEffect>(i);
        add(i == 0 ? "SkyCloudsVisible" : "SkyStarsVisible",
            Flag(IsSkyEffectVisible(effect)));
    }
    for (int i = 0; i < 4; ++i) {
        const auto region = static_cast<SkyRegion>(i);
        const char* name = i == 0 ? "SkyZenith" : i == 1 ? "SkyHorizon" :
                           i == 2 ? "SkySun" : "SkyClouds";
        colour(name, IsSkyColourEnabled(region), GetSkyColourHex(region));
    }
    for (int i = 0; i < 2; ++i) {
        const auto region = static_cast<FogRegion>(i);
        const char* name = i == 0 ? "FogGeneral" : "FogDirectional";
        colour(name, IsFogColourEnabled(region), GetFogColourHex(region));
        add((std::string(name) + "Visible").c_str(), Flag(IsFogVisible(region)));
    }
    add("ShadowsVisible", Flag(AreShadowsVisible()));
    colour("ShadowAmbient", IsShadowAmbientColourEnabled(),
           GetShadowAmbientColourHex());
    return v;
}

bool Validate(const Values& v) {
    // Read the full profile BEFORE changing any live settings; corrupt or
    // incomplete profiles cannot partially apply to the current world.
    const Values expected = Capture();
    if (v.size() != expected.size()) return false;
    for (const auto& [key, _] : expected) {
        const auto it = v.find(key);
        if (it == v.end()) return false;
        const int value = it->second;
        const std::string_view name(key);
        if (name.starts_with("Crosshair")) {
            // All fields are validated together below.
            continue;
        } else if (name == "TileRandomRgb") {
            if (value != 0 && value != 1) return false;
        } else if (name.ends_with("Rgb")) {
            if (value < 0 || value > 0xFFFFFF) return false;
        } else if (name.ends_with("Enabled") || name.ends_with("Visible") ||
                   name.ends_with("Removed") || name.ends_with("Locked")) {
            if (value != 0 && value != 1) return false;
        } else if (name.ends_with("Preset")) {
            if (value < 0 || value > 8) return false;
        } else if (name == "TimeMinutes") {
            if (value < 0 || value > 1439) return false;
        } else if (name == "TimeSpeed") {
            if (value < 25 || value > 1000) return false;
        } else if (name == "WaterIntensity" || name == "UnderwaterIntensity") {
            if (value < 0) return false;  // Existing intensity controls are uncapped.
        }
    }
    return ValidCrosshair(CrosshairSettings{v.at("CrosshairStyle"), v.at("CrosshairSize"), v.at("CrosshairThickness"), v.at("CrosshairGap"), v.at("CrosshairOpacity"), v.at("CrosshairRgb")});
}

bool WriteProfile(const std::wstring& name, const Values& values) {
    const auto path = Path(name);
    if (path.empty() || !EnsureDirectory(Root())) return false;
    std::string contents = "[CustomisationProfile]\r\nVersion=1\r\n";
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
                if (text != "[CustomisationProfile]") return false;
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
    // Version 1 profiles created before the water-reflection toggle did not
    // contain this key. Migrate only that known optional field; still reject
    // all other incomplete or unexpectedly extended profiles.
    if (header && version && !values.contains("WaterReflectionsVisible"))
        values.emplace("WaterReflectionsVisible", 1);
    // Ignore the retired experimental capture-shadow setting in old profiles.
    // A subsequent save rewrites the profile without the obsolete key.
    values.erase("WaterReflectionShadowsVisible");
    if (header && version && !values.contains("WaterReflectionGeometryVisible"))
        values.emplace("WaterReflectionGeometryVisible", 1);
    // Old profile schema: newly separated reflection categories default to ON.
    for (const auto* name : {"WaterReflectionButtonsVisible",
                             "WaterReflectionPipesVisible", "WaterReflectionBeaconsVisible",
                             "WaterReflectionJumpPadsVisible", "WaterReflectionCubesVisible"}) {
        if (header && version && !values.contains(name)) values.emplace(name, 1);
    }
    // Pre-crosshair profiles migrate as a group; a partially present group is corrupt.
    if (header && version && !values.contains("CrosshairStyle") && !values.contains("CrosshairSize") && !values.contains("CrosshairThickness") && !values.contains("CrosshairGap") && !values.contains("CrosshairOpacity") && !values.contains("CrosshairRgb")) {
        values.emplace("CrosshairStyle", 0);
        values.emplace("CrosshairSize", 8);
        values.emplace("CrosshairThickness", 2);
        values.emplace("CrosshairGap", 4);
        values.emplace("CrosshairOpacity", 100);
        values.emplace("CrosshairRgb", 16777215);
    }
    if (!header || !version || !Validate(values)) return false;
    out = std::move(values);
    return true;
}

// Only change fields that differ. Existing setters persist settings.ini and
// schedule budgeted UE4 material refreshes instead of doing actor scans here.
void Apply(const Values& target) {
    const Values current = Capture();
    const auto value = [&](const std::string& key) { return target.at(key); };
    const auto changed = [&](const std::string& key) {
        return value(key) != current.at(key);
    };
    auto applyColour = [&](const std::string& key, auto getEnabled, auto setEnabled,
                           auto setRgb) {
        const bool enabled = value(key + "Enabled") != 0;
        if (changed(key + "Rgb")) setRgb(static_cast<std::uint32_t>(value(key + "Rgb")));
        if (getEnabled() != enabled) setEnabled(enabled);
    };
    if (changed("CameraFov")) { g_player_fov_degrees = value("CameraFov"); SaveSettings(); }
    const CrosshairSettings crosshair{value("CrosshairStyle"), value("CrosshairSize"), value("CrosshairThickness"), value("CrosshairGap"), value("CrosshairOpacity"), value("CrosshairRgb")};
    if (crosshair != g_crosshair) { g_crosshair = crosshair; SaveSettings(); }
    for (bool pressed : {false, true}) {
        const std::string btn = pressed ? "ButtonPressed" : "ButtonActive";
        applyColour(btn, [&]{ return IsButtonCustomColourEnabled(pressed); },
            [&](bool yes){ SetButtonCustomColourEnabled(pressed, yes); },
            [&](uint32_t rgb){ SetButtonCustomColourHex(pressed, rgb); });
        const std::string border = pressed ? "BorderPressed" : "BorderActive";
        applyColour(border, [&]{ return IsButtonBorderCustomColourEnabled(pressed); },
            [&](bool yes){ SetButtonBorderCustomColourEnabled(pressed, yes); },
            [&](uint32_t rgb){ SetButtonBorderColourHex(pressed, rgb); });
    }
    if (changed("BeaconVisible")) SetButtonBeaconVisible(value("BeaconVisible") != 0);
    applyColour("Beacon", IsButtonBeaconCustomColourEnabled,
        SetButtonBeaconCustomColourEnabled, SetButtonBeaconColourHex);
    // Dedicated setter keeps both palettes, saved HEX, custom toggles and
    // Random RGB consistent without repeatedly cycling materials.
    if (changed("TileSelectedEnabled") || changed("TileSelectedRgb") ||
        changed("TileSelectedPreset") || changed("TileUnselectedEnabled") ||
        changed("TileUnselectedRgb") || changed("TileUnselectedPreset") ||
        changed("TileRandomRgb")) {
        ApplyTileColourProfileState(
            value("TileSelectedPreset"), value("TileUnselectedPreset"),
            value("TileSelectedEnabled") != 0,
            value("TileUnselectedEnabled") != 0,
            static_cast<std::uint32_t>(value("TileSelectedRgb")),
            static_cast<std::uint32_t>(value("TileUnselectedRgb")),
            value("TileRandomRgb") != 0);
    }
    for (bool selected : {false, true}) {
        const std::string wall = selected ? "WallSelected" : "WallUnselected";
        applyColour(wall, [&]{ return IsWallCustomColourEnabled(selected); },
            [&](bool yes){ SetWallCustomColourEnabled(selected, yes); },
            [&](uint32_t rgb){ SetWallHexColour(selected, rgb); });
    }
    applyColour("Tube", IsTubeCustomColourEnabled,
        SetTubeCustomColourEnabled, SetTubeCustomColourHex);
    applyColour("Platform", IsPlatformCustomColourEnabled,
        SetPlatformCustomColourEnabled, SetPlatformCustomColourHex);
    applyColour("JumpPad", IsJumpPadCustomColourEnabled,
        SetJumpPadCustomColourEnabled, SetJumpPadCustomColourHex);
    applyColour("Pillar", IsPillarColourEnabled,
        SetPillarColourEnabled, SetPillarColourHex);
    if (changed("PillarRemoved")) SetPillarsRemoved(value("PillarRemoved") != 0);
    // Intensity and HEX setters implicitly enable custom water colours.
    // Restore the target toggle LAST, even if it was OFF before loading.
    if (changed("WaterRgb"))
        SetWaterCustomColourHex(static_cast<std::uint32_t>(value("WaterRgb")));
    if (changed("WaterIntensity")) SetWaterColourIntensityPercent(value("WaterIntensity"));
    if (IsWaterCustomColourEnabled() != (value("WaterEnabled") != 0))
        SetWaterCustomColourEnabled(value("WaterEnabled") != 0);
    if (changed("UnderwaterRgb"))
        SetUnderwaterCustomColourHex(static_cast<std::uint32_t>(value("UnderwaterRgb")));
    if (changed("UnderwaterIntensity"))
        SetUnderwaterColourIntensityPercent(value("UnderwaterIntensity"));
    if (IsUnderwaterCustomColourEnabled() != (value("UnderwaterEnabled") != 0))
        SetUnderwaterCustomColourEnabled(value("UnderwaterEnabled") != 0);
    if (changed("UnderwaterTintRemoved"))
        SetUnderwaterTintRemoved(value("UnderwaterTintRemoved") != 0);
    if (changed("WaterSplashesRemoved"))
        SetWaterSplashesRemoved(value("WaterSplashesRemoved") != 0);
    if (changed("WaterReflectionsVisible"))
        SetWaterReflectionsVisible(value("WaterReflectionsVisible") != 0);
    if (changed("WaterReflectionGeometryVisible"))
        SetWaterReflectedWallsVisible(value("WaterReflectionGeometryVisible") != 0);
    if (changed("WaterReflectionButtonsVisible"))
        SetWaterReflectedButtonsVisible(value("WaterReflectionButtonsVisible") != 0);
    if (changed("WaterReflectionPipesVisible"))
        SetWaterReflectedPipesVisible(value("WaterReflectionPipesVisible") != 0);
    if (changed("WaterReflectionBeaconsVisible"))
        SetWaterReflectedBeaconsVisible(value("WaterReflectionBeaconsVisible") != 0);
    if (changed("WaterReflectionJumpPadsVisible"))
        SetWaterReflectedJumpPadsVisible(value("WaterReflectionJumpPadsVisible") != 0);
    if (changed("WaterReflectionCubesVisible"))
        SetWaterReflectedCubesVisible(value("WaterReflectionCubesVisible") != 0);
    // Lock before anchoring the target minute: locking captures the currently
    // visible time, which would otherwise overwrite the desired saved minute.
    if (IsCustomTimeLocked() != (value("TimeLocked") != 0))
        SetCustomTimeLocked(value("TimeLocked") != 0);
    if (GetCustomTimeMinutes() != value("TimeMinutes"))
        SetCustomTimeMinutes(value("TimeMinutes"));
    if (GetCustomTimeSpeedPercent() != value("TimeSpeed"))
        SetCustomTimeSpeedPercent(value("TimeSpeed"));
    // Time setters can also implicitly enable the override; correct it last.
    if (IsCustomTimeOfDayEnabled() != (value("TimeEnabled") != 0))
        SetCustomTimeOfDayEnabled(value("TimeEnabled") != 0);
    if (changed("SkyDiscVisible")) SetSkyDiscVisible(value("SkyDiscVisible") != 0);
    for (int i = 0; i < 2; ++i) {
        const std::string sky = i == 0 ? "SkyCloudsVisible" : "SkyStarsVisible";
        if (changed(sky)) SetSkyEffectVisible(static_cast<SkyEffect>(i), value(sky) != 0);
    }
    for (int i = 0; i < 4; ++i) {
        const auto region = static_cast<SkyRegion>(i);
        const std::string sky = i == 0 ? "SkyZenith" : i == 1 ? "SkyHorizon" :
                                i == 2 ? "SkySun" : "SkyClouds";
        applyColour(sky,
            [&]{ return IsSkyColourEnabled(region); },
            [&](bool yes){ SetSkyColourEnabled(region, yes); },
            [&](uint32_t rgb){ SetSkyColourHex(region, rgb); });
    }
    for (int i = 0; i < 2; ++i) {
        const auto region = static_cast<FogRegion>(i);
        const std::string fog = i == 0 ? "FogGeneral" : "FogDirectional";
        applyColour(fog,
            [&]{ return IsFogColourEnabled(region); },
            [&](bool yes){ SetFogColourEnabled(region, yes); },
            [&](uint32_t rgb){ SetFogColourHex(region, rgb); });
        if (changed(fog + "Visible")) SetFogVisible(region, value(fog + "Visible") != 0);
    }
    if (changed("ShadowsVisible")) SetShadowsVisible(value("ShadowsVisible") != 0);
    applyColour("ShadowAmbient", IsShadowAmbientColourEnabled,
        SetShadowAmbientColourEnabled, SetShadowAmbientColourHex);
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
    QueueSettingsValue(L"CustomisationProfiles", L"ActiveProfile", s_active.c_str());
}
} // namespace

void InitialiseCustomisationProfiles() {
    if (s_ready || g_storage_dir.empty() || !EnsureDirectory(Root())) return;
    Scan();
    wchar_t saved[64]{};
    GetPrivateProfileStringW(L"CustomisationProfiles", L"ActiveProfile",
                             L"Default", saved, static_cast<DWORD>(std::size(saved)),
                             g_settings_path.c_str());
    if (ValidProfileName(saved) && Found(saved)) s_active = saved;
    else s_active = L"Default";
    // settings.ini is already the current live state at launch; absorb any
    // changes saved during the previous session, including pre-profile installs.
    if (!WriteProfile(s_active, Capture())) {
        Log(L"Customisation profiles: could not initialise active profile");
        return;
    }
    Scan();
    s_ready = true;
    PersistActive();
}
const std::vector<std::wstring>& CustomisationProfileNames() { return s_names; }
const std::wstring& ActiveCustomisationProfile() { return s_active; }
void RefreshCustomisationProfiles() {
    if (!s_ready) InitialiseCustomisationProfiles();
    if (s_ready) Scan();
}
bool SaveActiveCustomisationProfile() {
    if (!s_ready) return false;
    if (!WriteProfile(s_active, Capture())) {
        Notify(L"Could not save customisation profile");
        return false;
    }
    return true;
}
bool SelectCustomisationProfile(const std::wstring& name) {
    if (!s_ready || !ValidProfileName(name) || !Exists(name)) return false;
    if (_wcsicmp(name.c_str(), s_active.c_str()) == 0) return true;
    Values target;
    if (!ReadProfile(name, target)) {
        Notify(L"Customisation profile is incomplete or damaged");
        return false;
    }
    if (!SaveActiveCustomisationProfile()) return false;
    Apply(target);
    s_active = name;
    PersistActive();
    Notify(L"Customisation profile: " + name);
    return true;
}
bool CreateCustomisationProfile(const std::wstring& name) {
    if (!s_ready || !ValidProfileName(name) || Found(name) || Exists(name)) {
        Notify(L"Invalid or duplicate customisation profile name");
        return false;
    }
    if (!SaveActiveCustomisationProfile() || !WriteProfile(name, Capture())) {
        Notify(L"Could not create customisation profile");
        return false;
    }
    s_active = name;
    Scan();
    PersistActive();
    Notify(L"Created customisation profile: " + name);
    return true;
}
bool RenameActiveCustomisationProfile(const std::wstring& name) {
    if (!s_ready || !ValidProfileName(name) || Found(name) || Exists(name) ||
        _wcsicmp(s_active.c_str(), L"Default") == 0) {
        Notify(L"Cannot rename Default, or name is already taken/invalid");
        return false;
    }
    if (!SaveActiveCustomisationProfile() ||
        !MoveFileW(Path(s_active).c_str(), Path(name).c_str())) {
        Notify(L"Could not rename customisation profile");
        return false;
    }
    s_active = name;
    Scan();
    PersistActive();
    Notify(L"Renamed customisation profile: " + name);
    return true;
}
bool DeleteActiveCustomisationProfile() {
    if (!s_ready || _wcsicmp(s_active.c_str(), L"Default") == 0) {
        Notify(L"Default customisation profile cannot be deleted");
        return false;
    }
    Values defaults;
    if (!ReadProfile(L"Default", defaults)) {
        Notify(L"Default customisation profile is unavailable");
        return false;
    }
    const std::wstring remove = Path(s_active);
    if (!DeleteFileW(remove.c_str())) {
        Notify(L"Could not delete customisation profile");
        return false;
    }
    Apply(defaults);
    s_active = L"Default";
    Scan();
    PersistActive();
    Notify(L"Deleted profile; Default customisation active");
    return true;
}
bool ReloadActiveCustomisationProfile() {
    if (!s_ready) return false;
    Values target;
    if (!ReadProfile(s_active, target)) {
        Notify(L"Saved customisation profile is invalid");
        return false;
    }
    Apply(target);
    Notify(L"Reloaded customisation profile: " + s_active);
    return true;
}
} // namespace refunct

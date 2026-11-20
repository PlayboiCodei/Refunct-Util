#pragma once
// Pure path calculations for the per-profile save layout.
// Keep legacy paths here as read-only/migration fallbacks; never write new
// recordings outside Profiles/<name>.
#include <string>
#include <cwctype>

namespace refunct::ghost_data_paths {
inline bool EqualNoCase(const std::wstring& a, const wchar_t* b) {
    const std::wstring other(b);
    if (a.size() != other.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::towlower(a[i]) != std::towlower(other[i])) return false;
    return true;
}
inline std::wstring ProfileName(const std::wstring& split_ini_path) {
    const auto slash = split_ini_path.find_last_of(L"\\/");
    const std::wstring filename = split_ini_path.substr(slash == std::wstring::npos ? 0 : slash + 1);
    if (filename.size() <= 4 || !EqualNoCase(filename.substr(filename.size() - 4), L".ini")) return {};
    if (!EqualNoCase(filename, L"records.ini")) return filename.substr(0, filename.size() - 4);
    // In Profiles/<profile>/Splits/records.ini the identity comes from the
    // directory. Distinguish this from the valid legacy profile "records"
    // stored at old Splits/records.ini.
    if (slash == std::wstring::npos) return {};
    const auto parent_end = split_ini_path.find_last_of(L"\\/", slash == 0 ? 0 : slash - 1);
    if (parent_end == std::wstring::npos ||
        !EqualNoCase(split_ini_path.substr(parent_end + 1, slash - parent_end - 1), L"Splits")) return {};
    const auto profile_end = split_ini_path.find_last_of(L"\\/", parent_end == 0 ? 0 : parent_end - 1);
    if (profile_end == std::wstring::npos) return L"records"; // Old Splits/records.ini
    const auto above_profile = split_ini_path.find_last_of(L"\\/", profile_end == 0 ? 0 : profile_end - 1);
    const auto parent = split_ini_path.substr(
        above_profile == std::wstring::npos ? 0 : above_profile + 1,
        profile_end - (above_profile == std::wstring::npos ? 0 : above_profile + 1));
    if (EqualNoCase(parent, L"Profiles"))
        return split_ini_path.substr(profile_end + 1, parent_end - profile_end - 1);
    return L"records"; // Legacy Splits/records.ini in a deeper install path.

}
inline std::wstring ProfilesRoot(const std::wstring& root) {
    return root.empty() ? std::wstring{} : root + L"\\Profiles";
}
inline std::wstring ProfileDir(const std::wstring& root, const std::wstring& name) {
    const auto profiles = ProfilesRoot(root);
    return profiles.empty() || name.empty() ? std::wstring{} : profiles + L"\\" + name;
}
inline std::wstring SplitsDir(const std::wstring& root, const std::wstring& name) {
    const auto dir = ProfileDir(root, name);
    return dir.empty() ? std::wstring{} : dir + L"\\Splits";
}
inline std::wstring Records(const std::wstring& root, const std::wstring& name) {
    const auto dir = SplitsDir(root, name);
    return dir.empty() ? std::wstring{} : dir + L"\\records.ini";
}
inline std::wstring GhostDir(const std::wstring& root, const std::wstring& name) {
    const auto dir = ProfileDir(root, name);
    return dir.empty() ? std::wstring{} : dir + L"\\Ghosts";
}
inline std::wstring PB(const std::wstring& root, const std::wstring& ini) {
    const auto dir = GhostDir(root, ProfileName(ini));
    return dir.empty() ? std::wstring{} : dir + L"\\PB.ghost";
}
inline std::wstring Furthest(const std::wstring& root, const std::wstring& ini) {
    const auto dir = GhostDir(root, ProfileName(ini));
    return dir.empty() ? std::wstring{} : dir + L"\\furthest.ghost";
}
inline std::wstring ProfileSectionDir(const std::wstring& root, const std::wstring& ini) {
    const auto dir = GhostDir(root, ProfileName(ini));
    return dir.empty() ? std::wstring{} : dir + L"\\BestSegments";
}
inline std::wstring Gold(const std::wstring& root, const std::wstring& ini, int button) {
    if (button < 1 || button > 37) return {};
    const auto dir = ProfileSectionDir(root, ini);
    if (dir.empty()) return {};
    return dir + L"\\gold" + (button < 10 ? L"0" : L"") + std::to_wstring(button) + L".ghost";
}
// Previous layouts: RefunctUtility/Splits/<profile>.ini(.ghost) and
// RefunctUtility/Ghosts/<profile>.ghost or Sections/<profile>/goldNN.ghost.
inline std::wstring LegacySplit(const std::wstring& root, const std::wstring& name) {
    return root.empty() || name.empty() ? std::wstring{} : root + L"\\Splits\\" + name + L".ini";
}
inline std::wstring LegacyGhostRoot(const std::wstring& root) {
    return root.empty() ? std::wstring{} : root + L"\\Ghosts";
}
inline std::wstring LegacyPB(const std::wstring& root, const std::wstring& ini) {
    const auto name = ProfileName(ini);
    const auto dir = LegacyGhostRoot(root);
    return name.empty() || dir.empty() ? std::wstring{} : dir + L"\\" + name + L".ghost";
}
inline std::wstring LegacyFurthest(const std::wstring& root, const std::wstring& ini) {
    const auto name = ProfileName(ini);
    const auto dir = LegacyGhostRoot(root);
    return name.empty() || dir.empty() ? std::wstring{} : dir + L"\\" + name + L".furthest.ghost";
}
inline std::wstring LegacySectionDir(const std::wstring& root, const std::wstring& ini) {
    const auto name = ProfileName(ini);
    const auto dir = LegacyGhostRoot(root);
    return name.empty() || dir.empty() ? std::wstring{} : dir + L"\\Sections\\" + name;
}
inline std::wstring LegacyGold(const std::wstring& root, const std::wstring& ini, int button) {
    if (button < 1 || button > 37) return {};
    const auto dir = LegacySectionDir(root, ini);
    return dir.empty() ? std::wstring{} : dir + L"\\gold" + (button < 10 ? L"0" : L"") + std::to_wstring(button) + L".ghost";
}
inline std::wstring LegacyInline(const std::wstring& root, const std::wstring& ini,
                                const std::wstring& suffix) {
    const auto name = ProfileName(ini);
    const auto path = LegacySplit(root, name);
    return path.empty() ? std::wstring{} : path + suffix;
}
// Retained aliases exclusively for code/tests reading the old global folder.
inline std::wstring GhostRoot(const std::wstring& root) { return LegacyGhostRoot(root); }
inline std::wstring SectionRoot(const std::wstring& root) {
    const auto dir = LegacyGhostRoot(root);
    return dir.empty() ? std::wstring{} : dir + L"\\Sections";
}
} // namespace refunct::ghost_data_paths

#pragma once
#include <string>
#include <vector>

namespace refunct {
// Independent of split/PB profiles. File IO occurs only on menu actions/startup.
void InitialiseHotkeyProfiles();
const std::vector<std::wstring>& HotkeyProfileNames();
const std::wstring& ActiveHotkeyProfile();
bool SaveActiveHotkeyProfile();
bool SelectHotkeyProfile(const std::wstring& name);
bool CreateHotkeyProfile(const std::wstring& name);
bool RenameActiveHotkeyProfile(const std::wstring& name);
bool DeleteActiveHotkeyProfile();
bool ReloadActiveHotkeyProfile();
void RefreshHotkeyProfiles();
bool HotkeyProfilesReady();
} // namespace refunct

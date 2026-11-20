#pragma once
#include <string>
#include <vector>

namespace refunct {
// Presentation-only profiles. File IO occurs only on menu actions/startup.
void InitialiseHudProfiles();
bool HudProfilesReady();
const std::vector<std::wstring>& HudProfileNames();
const std::wstring& ActiveHudProfile();
bool SaveActiveHudProfile();
bool SelectHudProfile(const std::wstring& name);
bool CreateHudProfile(const std::wstring& name);
bool RenameActiveHudProfile(const std::wstring& name);
bool DeleteActiveHudProfile();
bool ReloadActiveHudProfile();
void RefreshHudProfiles();
} // namespace refunct

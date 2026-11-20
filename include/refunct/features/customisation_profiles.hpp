#pragma once
#include <string>
#include <vector>

namespace refunct {
// Independent of split/PB profiles. File IO occurs only on menu actions/startup.
void InitialiseCustomisationProfiles();
const std::vector<std::wstring>& CustomisationProfileNames();
const std::wstring& ActiveCustomisationProfile();
bool SaveActiveCustomisationProfile();
bool SelectCustomisationProfile(const std::wstring& name);
bool CreateCustomisationProfile(const std::wstring& name);
bool RenameActiveCustomisationProfile(const std::wstring& name);
bool DeleteActiveCustomisationProfile();
bool ReloadActiveCustomisationProfile();
void RefreshCustomisationProfiles();
} // namespace refunct

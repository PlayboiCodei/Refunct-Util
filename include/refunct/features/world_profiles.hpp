#pragma once
#include <string>
#include <vector>

namespace refunct {
// Independent of split/PB profiles. File IO occurs only on menu actions/startup.
void InitialiseWorldProfiles();
const std::vector<std::wstring>& WorldProfileNames();
const std::wstring& ActiveWorldProfile();
bool SaveActiveWorldProfile();
bool SelectWorldProfile(const std::wstring& name);
bool CreateWorldProfile(const std::wstring& name);
bool RenameActiveWorldProfile(const std::wstring& name);
bool DeleteActiveWorldProfile();
bool ReloadActiveWorldProfile();
void RefreshWorldProfiles();
void NoteWorldProfilePersistenceReset();
} // namespace refunct

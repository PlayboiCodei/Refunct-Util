#pragma once
// Internal functions owned by this feature's .cpp file.
#include "refunct/state.hpp"
namespace refunct {
bool IsRegularFile(const std::wstring& path);
bool EnsureDirectory(const std::wstring& path);
std::wstring ProfilePath(const std::wstring& name);
bool EnsureProfileDirectories(const std::wstring& name);
std::wstring LegacyDataFolder();
bool InitializeStorage();
// Ensure a profile-specific gold folder on profile load, never each game tick.
bool EnsureGhostSectionDirectory(const std::wstring& split_ini_path);
void SaveSettings();
void SaveMovementModeSettings();
bool QueueSettingsValue(const wchar_t* section, const wchar_t* key, const wchar_t* value);
bool DrainSettingsWriter(DWORD timeout_ms);
int ReadUncappedPercentSetting(const wchar_t* key, int minimum,
                               const wchar_t* section = L"Movement");
void LoadSettings();
} // namespace refunct

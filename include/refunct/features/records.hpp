#pragma once
// Internal functions owned by this feature's .cpp file.
#include "refunct/state.hpp"
namespace refunct {
// Single source of truth for the Cheats Active HUD and record eligibility.
// One-time actions remain visible as "Used this run" until a clean restart.
enum class CheatAction : unsigned char {
    Position, Buttons, Tiles, Sections, Checkpoint, Cubes
};
void MarkCheatAction(CheatAction action);
void MarkAutomaticButtonRepair(int one_based_button);
std::vector<std::wstring> ActiveCheatLabels();
// Purely visual overrides; never included in the cheats / PB eligibility mask.
std::vector<std::wstring> ActiveCustomisationLabels();
void UpdateCheatNotifications();
bool GameplayTuningActive();
bool CheatsCurrentlyActive();
bool RecordsAllowedNow();
// Cheat permissions only bypass the movement-cheat gate, never generated
// progress from preloaded / assisted restart methods.
bool RecordsAllowedForPB();
bool RecordsAllowedForSegments();
void ClearTaintOnFreshRun(int buttons);
std::wstring ButtonLabel(int internal_button);
bool SavePersonalBestFile(const PersonalBest& best);
void ScanProfiles(bool create_defaults);
bool SelectProfile(const std::wstring& name, bool reset_run);
void LoadPersonalBest();
// Loads only an external profile into a read-only cache; no per-frame disk I/O.
// Automatically chooses a different profile when the selected one is missing.
bool ReloadComparisonProfile();
bool CycleComparisonProfile(int direction);
double ComparisonReferenceSeconds(int button);
SumOfBest CalculateSumOfBest();
GoldUpdate UpdateBestSegment(int button, double segment, SegmentSource source, double observed_elapsed);
void FinishPersonalBestIfNeeded();
void ResetButtonHistory();
bool RefreshProfiles();
bool CreateProfile(const std::wstring& name);
bool RenameActiveProfile(const std::wstring& name);
void StartNameEdit(NameEditMode mode);
void ProcessProfileNameKey(int key);
// Called after a native button contact changes the game's progress counter.
// Captures real intermediate button timestamps that can otherwise disappear
// when multiple buttons activate between CharacterTick callbacks.
void CaptureNativeButtonSplit(MyCharacter* player, int before_buttons, int after_buttons, void* actor);
void UpdateButtonSplits();
} // namespace refunct

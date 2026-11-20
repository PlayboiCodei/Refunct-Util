#pragma once
// Internal functions owned by this feature's .cpp file.
#include "refunct/state.hpp"
namespace refunct {
bool ResolveButtonBoundsCenter(int button, Vec3& out_center);
// Replay-only live location; does not assume the static coordinate table order.
bool ResolveReplayButtonCenter(int button, Vec3& out_center);
bool DiscoverPracticeButtons();
bool ActivatePracticeButton(int zero_based, MyCharacter* player);
Vec3 PracticeSpawnForButton(int button);
bool SetPracticePosition(MyCharacter* player, const Vec3& destination);
const LevelState* PracticeLevelState();
void UpdateProgressCounters();
void SaveCurrentCheckpoint(MyCharacter* player);
void PracticeFailed(const wchar_t* reason);
std::wstring PracticeStatus();
void RequestPreparedPractice(int buttons_to_restore, Vec3 spawn,
                             const Rotator* rotation, PracticeOrigin origin,
                             int animate_button = 0);
void RequestPractice(int target_button,
                     PracticeOrigin origin = PracticeOrigin::Section);
void LoadSavedCheckpoint();
void LoadLastButtonCheckpoint();
// Teleport to the position of the latest real button press without replaying progress.
void TeleportToLastPressedButton(MyCharacter* player);
// Resolve queued global teleport only after the map and checkpoint loader settle.
void UpdateGlobalLastButtonTeleport(MyCharacter* player);
void LoadSecondLastButtonCheckpoint();
void ClearButtonHistoryOnRestart();
// Returns zero if fewer than two consecutive, real button presses were recorded.
int SecondLastButtonNumber();
void RequestProgressRestart();
void __fastcall NewGameEventHook(MyCharacter* player, void* /*edx*/);
void UpdatePreloadedRestart();
void TrackLastButton(MyCharacter* player);
// Immediately record a real native button press; runs before the next tick.
void CaptureGlobalLastButtonContact(MyCharacter* player,
                                    int before_buttons, int after_buttons);
bool IsSyntheticPracticeButtonContact();
void UpdatePractice(MyCharacter* player);
// Per-section world placement overrides. Session-only; never modifies the
// native level/button counter or the user's save data.
enum class SectionPlacement : uint8_t { Auto = 0, Raised = 1, Lowered = 2 };
SectionPlacement GetSectionPlacement(int one_based_section);
std::wstring SectionHeightStatus();
bool CycleSectionPlacement(int one_based_section, int direction);
void ResetAllSectionPlacements();
void ResetSectionPlacementInternals();
// Optional vertical offsets from Refunct's verified native section heights.
// All changes are session-only and reset on a new map/run.
float GetSectionHeightOffset(int one_based_section);
bool AdjustSectionHeightOffset(int one_based_section, int direction);
bool AdjustAllSectionHeightOffsets(int direction);
bool ResetSectionHeightOffset(int one_based_section);
bool ResetAllSectionHeightOffsets();
// World section translations: 0=X, 1=Y, 2=Z; +/-50 cm per step.
// Session-only. The native Auto/Raised/Lowered modes remain functional.
float GetSectionPositionOffset(int one_based_section, int axis);
bool AdjustSectionPositionOffset(int one_based_section, int axis, int direction);
bool AdjustAllSectionPositionOffsets(int axis, int direction);
bool ResetSectionPositionOffsets(int one_based_section);
bool ResetAllSectionPositionOffsets();
// Bulk edit all 31 levels without altering game progression.
bool SetAllSectionPlacements(SectionPlacement mode);
// Commit pending transitions when switching animation OFF.
void FinishSectionPlacementAnimations();
void UpdateSectionPlacements();
// Native platform rises and optional manual section placement animations.
int GetSectionSpeedPercent();
void SetSectionSpeedProfilePercent(int percent);
bool IsSectionSpeedModified();
void AdjustSectionSpeed(int direction);
void ResetSectionSpeed();
void UpdateNativeSectionSpeed();
bool SnapshotRaisedSections(std::array<bool, kPracticeClusterCount>& raised);
// Intercepts Refunct's real button-contact callback; used to suppress
// presses in lowered sections, while preserving synthetic practice replay.
void __fastcall SectionButtonContactHook(void* button, void* /*edx*/, MyCharacter* player);
// Native button contact sync and explicit button editing actions.
bool SetButtonsSyncWithSections(bool enabled);
std::wstring SectionButtonSyncStatus();
bool RestoreButtons(bool include_underwater);
// Reconstruct vanilla button availability from game progress and section height.
bool RestoreDefaultButtons();
// Continuous recovery of *currently usable* native buttons runs automatically.
// Explicit one-shot recovery is still available for difficult ghost buttons.
bool RepairGhostButtonsNow();
bool DisableUnderwaterButtons();
bool DisableAboveWaterButtons();
bool DisableAllButtons();
bool PressAllAvailableButtons();
} // namespace refunct

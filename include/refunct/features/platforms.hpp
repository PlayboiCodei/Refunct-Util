#pragma once
// World > Platforms: controls the dedicated BP_Lift_C and BP_Mover_C actors.
// This does not change BP_LevelRoot_C section rise speed or player movement.
#include "refunct/state.hpp"
namespace refunct {
bool IsPlatformCustomColourEnabled();
void SetPlatformCustomColourEnabled(bool enabled);
std::wstring GetPlatformCustomColourHex();
void SetPlatformCustomColourHex(uint32_t rgb);
void OpenPlatformColourPicker();
int GetPlatformSpeedPercent();
void SetPlatformSpeedProfilePercent(int percent);
bool IsPlatformSpeedModified();
void AdjustPlatformSpeed(int direction);
void ResetPlatformSpeed();
void UpdatePlatforms(MyCharacter* player);
// Replay-only virtual foot contacts with Refunct's BP_Lift/BP_Mover.
// Keeps a native ALiftBase based-character contact until the ghost steps off.
enum class GhostReplayLiftStatus { Pending, Ready, Unavailable };
GhostReplayLiftStatus PrepareGhostReplayLifts(MyCharacter* player);
void UpdateGhostReplayLifts(MyCharacter* player, const Vec3& previous,
                            const Vec3& current, bool continuous);
void ResetGhostReplayLifts();
bool GhostReplayLiftsChanged();
} // namespace refunct

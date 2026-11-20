#pragma once
// World > Tubes: scoped material colouring and travel-timeline speed tuning.
// All Unreal reads/writes happen on the game thread.
#include "refunct/state.hpp"
namespace refunct {
bool IsTubeCustomColourEnabled();
void SetTubeCustomColourEnabled(bool enabled);
std::wstring GetTubeCustomColourHex();
void SetTubeCustomColourHex(uint32_t rgb);
void OpenTubeColourPicker();
void UpdateTubeColours(MyCharacter* player);
// 100% preserves Refunct's native timeline rate. Speed changes affect only
// the BP_MyCharacter TravelMover timeline, not global game time.
int GetTubeTravelSpeedPercent();
void SetTubeTravelSpeedProfilePercent(int percent);
bool IsTubeTravelSpeedModified();
void AdjustTubeTravelSpeed(int direction);
void ResetTubeTravelSpeed();
void UpdateTubeTravelSpeed(MyCharacter* player);
} // namespace refunct

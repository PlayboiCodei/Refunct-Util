#pragma once
// Internal functions owned by this feature's .cpp file.
#include "refunct/state.hpp"
namespace refunct {
void RestorePracticeClusterSpeeds();
bool AcceleratePracticeClusters();
void CancelNormalFastRise();
bool BeginNormalFastRise();
void UpdateNormalFastRise();
void* GetGameInstance();
bool RestartRun(MyCharacter* player);
// Request a native UE4 map reload; game process and GameInstance remain alive.
bool RequestFullLevelReload();
// Execute only after the original character tick, because OpenLevel may destroy the player.
bool ExecuteFullLevelReload(MyCharacter* player);
void CheckFullLevelReloadCompletion();
} // namespace refunct

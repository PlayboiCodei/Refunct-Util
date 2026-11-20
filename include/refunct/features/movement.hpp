#pragma once
// Internal functions owned by this feature's .cpp file.
#include "refunct/state.hpp"
namespace refunct {
void ResetJumpAssistState();
void ResetAirMomentum();
void ResetSlideTuningState();
void UpdateSlideTuning(MyCharacter* player, float dt);
void ResetCharacter(MyCharacter* character);
void MaintainFlyingState(MyCharacter* player);
void SetFlying(MyCharacter* player, bool enabled);
void SetNoclip(MyCharacter* player, bool enabled);
void SavePosition(MyCharacter* player);
void RestorePosition(MyCharacter* player);
// Internal last-button teleport may run after a practice load reaches Ready.
bool TeleportToCoordinates(MyCharacter* player, const Vec3& destination,
                           bool allow_when_practice_ready = false);
void ApplyTunedFloat(TunedFloatProperty& field, float multiplier);
void* ReadReflectedObject(void* owner, const wchar_t* property);
void ApplyGameSpeed();
void ApplyPlayerFov(MyCharacter* player);
void ResetPlayerFovTracking();
void ApplyMovementTuning(MyCharacter* player);
void UpdateAirMomentum(MyCharacter* player);
int PercentageAdjustmentStep(int percentage, int direction, int small_step);
int AdjustUncappedPercent(int percentage, int direction, int minimum, int small_step);
void InvalidateAssistedTiming();
void AdjustMovementPercent(bool walking_speed, int direction);
void AdjustSwimSpeedPercent(int direction);
void AdjustClimbSpeedPercent(int direction);
void AdjustSlidePercent(bool adjust_speed, int direction);
void AdjustAccelerationPercent(int direction);
void TogglePreserveMomentum();
const wchar_t* JumpTargetsLabel(JumpTargets targets);
const wchar_t* JumpAssistModeLabel(bool scroll);
void CycleJumpAssistMode(bool scroll, int direction = 1);
void ToggleAirJump();
void ApplyPendingAirJump(MyCharacter* player);
void AdjustGameSpeedPercent(int direction);
bool ResetMovementConfiguration();
void ResetMovementOnNormalRestart(MyCharacter* player, bool force_defaults = false);
void RestorePendingMovementModes(MyCharacter* player);
void ToggleMovementPersistence(bool game_launch);
void ToggleWorldPersistence(bool game_launch);
void ResetWorldOnNormalRestart();
void ApplyWorldGameLaunchPersistence();
void ToggleRecordSaving(bool personal_best);
void RestoreMovementDefaults(MyCharacter* player);
void* FindReflectedFunction(void* object, const wchar_t* name);
void UpdateJumpAssist(MyCharacter* player);
} // namespace refunct

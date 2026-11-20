#pragma once
// World > Water and Customisation > Water: reversible native gameplay and appearance controls.
#include "refunct/state.hpp"
namespace refunct {
// Surface and underwater intensity have no user-configured ceiling. The int-based menu
// naturally stops only when its storage type can no longer represent a value.
// 100% is the original full-strength tint; larger values apply HDR boosting.
bool IsWaterCustomColourEnabled();
void SetWaterCustomColourEnabled(bool enabled);
std::wstring GetWaterCustomColourHex();
void SetWaterCustomColourHex(uint32_t rgb);
int GetWaterColourIntensityPercent();
void SetWaterColourIntensityPercent(int percent);
void OpenWaterColourPicker();
bool IsUnderwaterCustomColourEnabled();
void SetUnderwaterCustomColourEnabled(bool enabled);
std::wstring GetUnderwaterCustomColourHex();
void SetUnderwaterCustomColourHex(uint32_t rgb);
int GetUnderwaterColourIntensityPercent();
void SetUnderwaterColourIntensityPercent(int percent);
void OpenUnderwaterColourPicker();
// Visual-only effects. These do not modify swimming, physics or PB eligibility.
bool IsUnderwaterTintRemoved();
void SetUnderwaterTintRemoved(bool enabled);
bool AreWaterSplashesRemoved();
void SetWaterSplashesRemoved(bool enabled);
// WorldBase native planar-reflection capture (separate from directional shadows).
bool AreWaterReflectionsVisible();
// Capture-only visibility of concrete walls and lift platforms.
bool AreWaterReflectedWallsVisible();
void SetWaterReflectedWallsVisible(bool visible);
bool AreWaterReflectedButtonsVisible();
void SetWaterReflectedButtonsVisible(bool visible);
bool AreWaterReflectedPipesVisible();
void SetWaterReflectedPipesVisible(bool visible);
bool AreWaterReflectedBeaconsVisible();
void SetWaterReflectedBeaconsVisible(bool visible);
bool AreWaterReflectedJumpPadsVisible();
void SetWaterReflectedJumpPadsVisible(bool visible);
bool AreWaterReflectedCubesVisible();
void SetWaterReflectedCubesVisible(bool visible);
void SetWaterReflectionsVisible(bool visible);
// World gameplay controls: reversible surface/physics-volume height and swimming override.
// The master setting overrides all water effects without modifying individual settings.
bool IsAllWaterRemoved();
void SetAllWaterRemoved(bool enabled);
bool IsWaterRemoved();
void SetWaterRemoved(bool enabled);
bool IsWaterSwimmingDisabled();
void SetWaterSwimmingDisabled(bool enabled);
int GetWaterHeightOffset(); // Unreal units; synced to verified native water volumes when supported.
void SetWaterHeightOffset(int offset);
void AdjustWaterHeightOffset(int direction);
void UpdateWaterSwimming(MyCharacter* player);
void UpdateWaterColours(MyCharacter* player);
} // namespace refunct

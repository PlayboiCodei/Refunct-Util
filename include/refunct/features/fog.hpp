#pragma once
#include <cstdint>
#include <string>

namespace refunct {
struct MyCharacter;
// Colours of the native ExponentialHeightFogComponent in WorldBase.
// These are cosmetic; they do not change fog density, visibility or sun lighting.
enum class FogRegion : uint8_t { General = 0, Directional = 1 };
bool IsFogVisible(FogRegion region);
void SetFogVisible(FogRegion region, bool visible);
void ResetFogVisibility();
bool IsFogColourEnabled(FogRegion region);
void SetFogColourEnabled(FogRegion region, bool enabled);
std::wstring GetFogColourHex(FogRegion region);
void SetFogColourHex(FogRegion region, uint32_t rgb);
void OpenFogColourPicker(FogRegion region);
void ResetFogColours();
void UpdateFogColours(MyCharacter* player);
} // namespace refunct

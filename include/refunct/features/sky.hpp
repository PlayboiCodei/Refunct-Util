#pragma once
#include <cstdint>
#include <string>

namespace refunct {
struct MyCharacter;
// BP_DynamicSky's separate sky-material vector parameters. Visual only.
enum class SkyRegion : uint8_t { Zenith = 0, Horizon = 1, Sun = 2, Clouds = 3 };
// Controls Refunct's shared day/night sky disc without changing lighting.
bool IsSkyDiscVisible();
void SetSkyDiscVisible(bool visible);
void ResetSkyDiscVisibility();
// Native sky material scalars. Leave the daytime/nighttime system running.
enum class SkyEffect : uint8_t { Clouds = 0, Stars = 1 };
bool IsSkyEffectVisible(SkyEffect effect);
void SetSkyEffectVisible(SkyEffect effect, bool visible);
void ResetSkyEffectVisibility();
bool IsSkyColourEnabled(SkyRegion region);
void SetSkyColourEnabled(SkyRegion region, bool enabled);
std::wstring GetSkyColourHex(SkyRegion region);
void SetSkyColourHex(SkyRegion region, uint32_t rgb);
void ResetSkyColours();
void OpenSkyColourPicker(SkyRegion region);
void UpdateSkyColours(MyCharacter* player);
}

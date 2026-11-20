#pragma once
#include <cstdint>
#include <string>

namespace refunct {
struct MyCharacter;
// Cast shadows from Refunct's WorldBase directional light. This does not
// remove baked/ambient occlusion. The engine's original setting is restored.
bool AreShadowsVisible();
void SetShadowsVisible(bool visible);
// Tint the ambient cubemap filling shaded surfaces in PostNormal. This is NOT
// a cast-shadow-only colour shader; ambient highlights may also be affected.
bool IsShadowAmbientColourEnabled();
void SetShadowAmbientColourEnabled(bool enabled);
std::wstring GetShadowAmbientColourHex();
void SetShadowAmbientColourHex(uint32_t rgb);
void OpenShadowAmbientColourPicker();
void ResetShadowCustomisation();
void UpdateShadowCustomisation(MyCharacter* player);
} // namespace refunct

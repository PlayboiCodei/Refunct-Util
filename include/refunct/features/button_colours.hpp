#pragma once
#include <cstdint>
#include <string>
namespace refunct {
// Button colours are visual-only; native button actor materials and game
// progression fields are never modified by this feature.
bool IsButtonCustomColourEnabled(bool pressed);
void SetButtonCustomColourEnabled(bool pressed, bool enabled);
std::wstring GetButtonCustomColourHex(bool pressed);
void SetButtonCustomColourHex(bool pressed, uint32_t rgb);
void OpenButtonColourPicker(bool pressed);
// Independent colours for unpressed/pressed button borders ("Button base").
// pressed=false is the active/unpressed border; pressed=true is the pressed border.
bool IsButtonBorderCustomColourEnabled(bool pressed);
void SetButtonBorderCustomColourEnabled(bool pressed, bool enabled);
std::wstring GetButtonBorderColourHex(bool pressed);
void SetButtonBorderColourHex(bool pressed, uint32_t rgb);
void OpenButtonBorderColourPicker(bool pressed);
// "Beacon" is the independent vertical marker component on BP_Button_C.
// Visibility and colour are cosmetic; native press state is never modified.
bool IsButtonBeaconVisible();
void SetButtonBeaconVisible(bool visible);
bool IsButtonBeaconCustomColourEnabled();
void SetButtonBeaconCustomColourEnabled(bool enabled);
std::wstring GetButtonBeaconColourHex();
void SetButtonBeaconColourHex(uint32_t rgb);
void OpenButtonBeaconColourPicker();
void UpdateButtonColours();
} // namespace refunct

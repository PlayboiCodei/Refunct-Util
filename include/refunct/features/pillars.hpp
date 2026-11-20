#pragma once
// BP_BuildingDistributor background pillars (SM_Block_1000x1000x20000).
// Colours and removal are grouped in Customisation; removal also disables collision.
// The legacy visibility API remains available for safe internal restoration.
#include "refunct/state.hpp"

namespace refunct {
bool IsPillarColourEnabled();
bool ArePillarsRemoved();
bool ArePillarsVisible();
void SetPillarsVisible(bool visible);
void SetPillarsRemoved(bool removed);
void SetPillarColourEnabled(bool enabled);
std::wstring GetPillarColourHex();
void SetPillarColourHex(uint32_t rgb);
void OpenPillarColourPicker();
void UpdatePillarColours(MyCharacter* player);
} // namespace refunct

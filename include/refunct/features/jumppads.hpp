#pragma once
// World > Jump Pads. Cosmetic colour and launch-height multipliers are separate
// from normal Jump Height and the 31 rising sections.
#include "refunct/state.hpp"
namespace refunct {
bool IsJumpPadCustomColourEnabled();
void SetJumpPadCustomColourEnabled(bool enabled);
std::wstring GetJumpPadCustomColourHex();
void SetJumpPadCustomColourHex(uint32_t rgb);
void OpenJumpPadColourPicker();
int GetJumpPadHeightPercent();
void SetJumpPadHeightProfilePercent(int percent);
bool IsJumpPadHeightModified();
void AdjustJumpPadHeight(int direction);
void ResetJumpPadHeight();
void UpdateJumpPads(MyCharacter* player);
// Used only by the native event hook, which is optional outside full mode.
extern ProcessEventFn g_original_jump_pad_process_event;
extern bool g_jump_pad_launch_hook_available;
void __fastcall JumpPadProcessEventHook(void* object, void* edx,
                                        void* function, void* args);
} // namespace refunct

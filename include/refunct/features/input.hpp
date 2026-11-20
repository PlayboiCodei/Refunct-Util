#pragma once
// Internal functions owned by this feature's .cpp file.
#include "refunct/state.hpp"
namespace refunct {
bool HasFocusedGameWindow();
LRESULT CALLBACK MouseWheelInputHook(int code, WPARAM message, LPARAM details);
void UpdateMouseWheelHook();
bool IsBound(int key);
void __fastcall KeyDownHook(void* app, void* /*edx*/, int key, uint32_t code, bool repeat);
void __fastcall KeyUpHook(void* app, void* /*edx*/, int key, uint32_t code, bool repeat);
} // namespace refunct

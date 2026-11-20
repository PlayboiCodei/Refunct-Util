#pragma once
// Internal functions owned by this feature's .cpp file.
#include "refunct/state.hpp"
namespace refunct {
void DrawText(void* hud, const std::wstring& text, float x, float y, float scale = 1.0f,
              LinearColor color = {0.95f, 0.97f, 1.0f, 1.0f});
void DrawCenteredText(void* hud, const std::wstring& value, float center_x,
                      float y, float scale, LinearColor color);
void __fastcall HudDrawHook(void* hud, void* /*edx*/);
} // namespace refunct

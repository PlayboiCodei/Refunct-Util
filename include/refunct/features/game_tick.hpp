#pragma once
// Internal functions owned by this feature's .cpp file.
#include "refunct/state.hpp"
namespace refunct {
void ResetWorldSpeedSampling();
void __fastcall CharacterTickHook(MyCharacter* player, void* /*edx*/, float dt);
} // namespace refunct

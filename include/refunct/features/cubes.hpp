#pragma once
#include "refunct/state.hpp"

namespace refunct {
// Gameplay-affecting, session-local actions. Never edit LevelState counters.
enum class CubeWorldAction : unsigned char {
    DisableAll, CollectAll, ShowAll, ShowAboveWater, RestoreDefault
};
bool RequestCubeWorldAction(CubeWorldAction action, MyCharacter* player);
bool IsWorldCubeOverrideActive();
void UpdateWorldCubes(MyCharacter* player);
void ResetWorldCubeCache();
// Replay-owned cache; never mutates World > Cubes preferences or batch state.
enum class GhostReplayCubeStatus { Pending, Ready, Unavailable };
GhostReplayCubeStatus PrepareGhostReplayCubes(MyCharacter* player);
void TouchGhostReplayCubes(MyCharacter* player,const Vec3& previous,const Vec3& current,bool continuous);
bool GhostReplayCubesChanged();
void ResetGhostReplayCubes();
} // namespace refunct

#pragma once
#include "refunct/state.hpp"

namespace refunct {
// World-only gameplay controls; values are persisted by name, not enum ordinal.
// Actor UObjects are deliberately retained so normal restarts and restoring
// the toggles do not require destroying/reconstructing Blueprint actors.
enum class WorldObjectKind : uint8_t {
    Tubes, Buttons, JumpPads, Cubes, Platforms, Tiles, Count
};

bool IsWorldObjectRemoved(WorldObjectKind kind);
bool AreAllWorldObjectsRemoved();
void SetWorldObjectRemoved(WorldObjectKind kind, bool removed);
void SetAllWorldObjectsRemoved(bool removed);
void SetWorldObjectRemovalProfile(const std::array<bool, static_cast<size_t>(WorldObjectKind::Count)>& removed);
void ResetWorldObjectRemovals();
void UpdateWorldObjectRemovals(MyCharacter* player);
} // namespace refunct

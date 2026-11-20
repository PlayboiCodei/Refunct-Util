#pragma once
// Internal functions owned by this feature's .cpp file.
#include "refunct/state.hpp"
namespace refunct {
bool ValidCheckpoint(const Checkpoint& saved);
bool CaptureCheckpoint(MyCharacter* player, Checkpoint& result);
std::wstring CheckpointFloat(float number);
bool PersistCheckpoint(const Checkpoint& snapshot);
bool LoadPersistedCheckpoint();
} // namespace refunct

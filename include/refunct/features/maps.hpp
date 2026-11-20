#pragma once
#include "refunct/state.hpp"
namespace refunct {
// Read JSON map names only when the Maps page is opened or refreshed.
void RefreshCustomMapFiles();
size_t CustomMapFileCount();
std::wstring CustomMapFileLabel(size_t index);
std::wstring CustomMapStatus();
// Load a Refunct TAS map v1 by index, or restore actors from the first snapshot.
bool RequestCustomMapLoad(size_t index);
bool RequestCustomMapRestore();
// Process only a bounded amount of native actor work each game tick.
void UpdateCustomMapLoad();
bool CustomMapActive();
// True while the native world is being loaded, reapplied or restored.
bool CustomMapApplying();
// Called by the native level-reload system to invalidate world-specific
// actor handles and confirm (or reject) an original-map recovery.
void OnCustomMapFullLevelReloadStart();
void OnCustomMapFullLevelReloadResult(bool succeeded);
} // namespace refunct

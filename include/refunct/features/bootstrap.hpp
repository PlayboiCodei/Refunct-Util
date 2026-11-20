#pragma once
// Internal functions owned by this feature's .cpp file.
#include "refunct/state.hpp"
namespace refunct {
// Diagnostic startup trace shared by bootstrap and storage modules.
void StartupTrace(const char* message);
DWORD WINAPI Initialize(LPVOID);
} // namespace refunct

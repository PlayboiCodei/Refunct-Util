#pragma once
// Internal functions owned by this feature's .cpp file.
#include "refunct/state.hpp"
namespace refunct {
void Log(const wchar_t* message);
void ConfigureUtilLogDirectory(const std::wstring& storage_directory);
void SetUtilLogEnabled(bool enabled);
bool RequestClearRuntimeLogs();
const wchar_t* ClearLogsMenuLabel();
bool DrainLogWriter(DWORD timeout_ms);
bool IsReadable(const void* ptr, size_t size);
bool IsWritable(const void* ptr, size_t size);
bool IsRvaInSection(HMODULE module, uintptr_t rva, bool executable);
void Notify(const std::wstring& text);
int Hotkey(Action action);
std::wstring KeyName(int vk);
bool IsBindable(int key);
} // namespace refunct

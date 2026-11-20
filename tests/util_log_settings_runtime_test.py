"""Util-log integration checks. Native worker behaviour is exercised by async_io_runtime_test.py."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
core = (root / 'src/core.cpp').read_text(encoding='utf-8')
menu = (root / 'src/menu.cpp').read_text(encoding='utf-8')
storage = (root / 'src/storage.cpp').read_text(encoding='utf-8')
state_h = (root / 'include/refunct/state.hpp').read_text(encoding='utf-8')
state = (root / 'src/state.cpp').read_text(encoding='utf-8')
core_h = (root / 'include/refunct/features/core.hpp').read_text(encoding='utf-8')

assert 'extern std::atomic_bool g_util_log_enabled;' in state_h
assert 'std::atomic_bool g_util_log_enabled{true};' in state
assert 'void SetUtilLogEnabled(bool enabled);' in core_h
assert 'void ConfigureUtilLogDirectory(const std::wstring& storage_directory);' in core_h
assert 'L"\\\\Logs\\\\RefunctUtility.log"' in core
assert 'GetModuleFileNameW(g_module, path, MAX_PATH)' not in core.split('bool IsReadable(', 1)[0]
assert 'L"Settings", L"UtilLogEnabled"' in storage
assert 'GetPrivateProfileIntW(L"Settings", L"UtilLogEnabled", 1,' in storage
assert storage.index('ConfigureUtilLogDirectory(g_storage_dir);') > storage.index('if (!InitializeStorage())')
assert storage.index('ConfigureUtilLogDirectory(g_storage_dir);') < storage.index('GetPrivateProfileStringW(L"Splits", L"ActiveProfile"')
assert 'std::wstring(L"Save Logs: ")' in menu
assert 'SetUtilLogEnabled(!g_util_log_enabled.load(std::memory_order_relaxed));' in menu
util_toggle = menu.split('SetUtilLogEnabled(!g_util_log_enabled.load', 1)[1].split('} else if (i == 1)', 1)[0]
assert 'SetPerformanceMonitorEnabled(g_util_log_enabled.load(std::memory_order_relaxed));' in util_toggle
assert 'SaveSettings();' in util_toggle
assert 'worker->pending.clear();' in core
assert 'worker->generation.fetch_add(1, std::memory_order_acq_rel)' in core
assert 'if (!message || !g_util_log_enabled.load(std::memory_order_acquire)) return;' in core
assert 'WriteFile(file, message,' not in core, 'Fallback logging must not block game thread'
print('PASS: persisted Util Log setting, menu toggle, deferred directory and nonblocking OFF mode')

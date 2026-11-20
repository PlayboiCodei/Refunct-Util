"""Static integration and mock Windows C++ syntax test for Customisation Profiles.

Only actual Windows Win32 compilation and game testing prove injected ABI correctness.
"""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/customisation_profiles.cpp').read_text(encoding='utf8')
menu = (root / 'src/menu.cpp').read_text(encoding='utf8')
state = (root / 'include/refunct/state.hpp').read_text(encoding='utf8')
record = (root / 'src/records.cpp').read_text(encoding='utf8')
bootstrap = (root / 'src/bootstrap.cpp').read_text(encoding='utf8')
cmake = (root / 'CMakeLists.txt').read_text(encoding='utf8')

assert 'src/customisation_profiles.cpp' in cmake
assert 'InitialiseCustomisationProfiles();' in bootstrap.split('LoadSettings();', 1)[1]
assert 'enum class NameEditMode { None, Create, Rename, CustomCreate, CustomRename, HudCreate, HudRename, WorldCreate, WorldRename, HotkeyCreate, HotkeyRename };' in state
assert 'NameEditMode::CustomCreate' in record and 'NameEditMode::CustomRename' in record
for page in ('CustomisationProfiles', 'ConfirmDeleteCustomisationProfile'):
    assert f'case Page::{page}:' in menu
assert 'RefreshCustomisationProfiles();' in menu
assert 'SaveActiveCustomisationProfile()' in menu
assert 'SetPage(Page::CustomisationProfiles)' in menu
assert 'SetPage(Page::ConfirmDeleteCustomisationProfile)' in menu
assert 'DeleteActiveCustomisationProfile()' in menu
assert 'ApplyTileColourProfileState(' in source
assert 'InvalidateTileVisuals();' in (root/'src/tiles.cpp').read_text().split('void ApplyTileColourProfileState',1)[1].split('\n}',1)[0]

# These functions and data must never be reached by profile loading.
apply = source.split('void Apply(const Values& target)', 1)[1].split('\nvoid Scan()', 1)[0]
for forbidden in ('SetWorldObjectRemoved', 'SetPlatformSpeed', 'AdjustJumpPadHeight',
                  'SelectProfile(', 'SavePersonalBest', 'SetTubeTravelSpeed',
                  'g_cheat_record_mode =', 'g_keys[', 'g_pb ='):
    assert forbidden not in apply, forbidden
for token in ('CameraFov', 'ButtonPressed', 'BorderPressed', 'BeaconVisible',
              'TileRandomRgb', 'PillarRemoved', 'WaterIntensity', 'TimeLocked',
              'SkyCloudsVisible', 'FogDirectional', 'ShadowsVisible'):
    assert token in source, token
assert 'if (!ReadProfile(name, target))' in source
assert 'if (!SaveActiveCustomisationProfile()) return false;' in source
assert 'MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH' in source
assert 'Contents' not in source
assert 'if (!header || !version || !Validate(values)) return false;' in source
# Setters for water intensity and clock values implicitly turn overrides on.
# Their final states must always match the selected profile, including OFF.
assert apply.index('SetWaterColourIntensityPercent(value("WaterIntensity"))') < apply.index('if (IsWaterCustomColourEnabled() !=')
assert apply.index('SetUnderwaterColourIntensityPercent(value("UnderwaterIntensity"))') < apply.index('if (IsUnderwaterCustomColourEnabled() !=')
assert apply.index('SetCustomTimeLocked(value("TimeLocked")') < apply.index('SetCustomTimeMinutes(value("TimeMinutes")')
assert apply.index('SetCustomTimeMinutes(value("TimeMinutes")') < apply.index('if (IsCustomTimeOfDayEnabled() !=')
assert 'CustomisationProfile' not in (root/'src/game_tick.cpp').read_text()

compiler = shutil.which('clang++') or shutil.which('g++')
if not compiler:
    print('PASS: source integration (no C++ compiler available for mock syntax test)')
    raise SystemExit

with tempfile.TemporaryDirectory() as directory:
    mock = Path(directory) / 'refunct'
    mock.mkdir()
    (mock / 'state.hpp').write_text('''#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>
#include <iterator>
#include <utility>
#include <cwchar>
using DWORD = unsigned long;
namespace refunct { struct MyCharacter; struct Vec3; using ProcessEventFn = void(*)(void*, void*, void*);
extern std::wstring g_storage_dir, g_settings_path;
extern int g_player_fov_degrees;
}
''')
    (mock / 'internal_api.hpp').write_text('''#pragma once
#include "refunct/state.hpp"
#include "refunct/features/button_colours.hpp"
#include "refunct/features/tiles.hpp"
#include "refunct/features/tubes.hpp"
#include "refunct/features/platforms.hpp"
#include "refunct/features/jumppads.hpp"
#include "refunct/features/pillars.hpp"
#include "refunct/features/water.hpp"
#include "refunct/features/time_of_day.hpp"
#include "refunct/features/sky.hpp"
#include "refunct/features/fog.hpp"
#include "refunct/features/shadows.hpp"
#include "refunct/features/storage.hpp"
#include <string>
#include <vector>
#include <cstdint>
#include <string_view>
#include <iterator>
#include <cwchar>
using HANDLE = void*;
constexpr HANDLE INVALID_HANDLE_VALUE = nullptr;
constexpr DWORD FILE_ATTRIBUTE_DIRECTORY = 16;
constexpr DWORD FILE_ATTRIBUTE_NORMAL = 128;
constexpr DWORD GENERIC_READ = 0x80000000, GENERIC_WRITE = 0x40000000;
constexpr DWORD FILE_SHARE_READ = 1, OPEN_EXISTING = 3, CREATE_ALWAYS = 2;
constexpr DWORD MOVEFILE_REPLACE_EXISTING = 1, MOVEFILE_WRITE_THROUGH = 8;
constexpr DWORD INVALID_FILE_SIZE = 0xFFFFFFFF;
struct WIN32_FIND_DATAW { DWORD dwFileAttributes; wchar_t cFileName[260]; };
extern "C" int _wcsicmp(const wchar_t*, const wchar_t*);
HANDLE FindFirstFileW(const wchar_t*, WIN32_FIND_DATAW*);
int FindNextFileW(HANDLE, WIN32_FIND_DATAW*);
int FindClose(HANDLE);
HANDLE CreateFileW(const wchar_t*, DWORD, DWORD, void*, DWORD, DWORD, void*);
DWORD GetFileSize(HANDLE, void*);
int ReadFile(HANDLE, void*, DWORD, DWORD*, void*);
int WriteFile(HANDLE, const void*, DWORD, DWORD*, void*);
int FlushFileBuffers(HANDLE);
int CloseHandle(HANDLE);
int DeleteFileW(const wchar_t*);
int MoveFileW(const wchar_t*, const wchar_t*);
int MoveFileExW(const wchar_t*, const wchar_t*, DWORD);
DWORD GetPrivateProfileStringW(const wchar_t*, const wchar_t*, const wchar_t*, wchar_t*, DWORD, const wchar_t*);
namespace refunct {
bool ValidProfileName(const std::wstring&);
bool IsRegularFile(const std::wstring&);
bool EnsureDirectory(const std::wstring&);
bool QueueSettingsValue(const wchar_t*, const wchar_t*, const wchar_t*);
void SaveSettings();
void Log(const wchar_t*);
void Notify(const std::wstring&);
}
''')
    result = subprocess.run([compiler, '-std=c++20', '-fsyntax-only', '-Werror', '-Wno-ignored-attributes',
                             '-I', directory, '-I', str(root / 'include'),
                             str(root/'src/customisation_profiles.cpp')],
                            capture_output=True, text=True, timeout=50)
    if result.returncode:
        print(result.stdout, result.stderr)
    assert result.returncode == 0, 'Mock native C++ syntax check failed'

print('PASS: Customisation Profiles menu, persistence, policy, mock native C++ syntax')

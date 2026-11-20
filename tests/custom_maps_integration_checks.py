"""Static integration contract for Refunct TAS v1 maps and F5 menu.

No gameplay or native-ABI claim: Windows x86 in-game testing remains required.
"""
from pathlib import Path
r=Path(__file__).resolve().parents[1]
menu=(r/'src/menu.cpp').read_text()
state=(r/'include/refunct/state.hpp').read_text()
api=(r/'include/refunct/internal_api.hpp').read_text()
tick=(r/'src/game_tick.cpp').read_text()
native=(r/'src/maps.cpp').read_text()
cm=(r/'CMakeLists.txt').read_text()
records=(r/'src/records.cpp').read_text()
for path in ['src/maps.cpp','include/refunct/features/maps.hpp','include/refunct/custom_map_format.hpp']:
    assert (r/path).is_file()
assert 'CustomisationTime, Maps' in state
assert 'L"Maps  >"' in menu and 'case Page::Maps:' in menu
assert 'RefreshCustomMapFiles();' in menu
assert 'RequestCustomMapLoad(MapPageStart()' in menu
assert 'RequestCustomMapRestore()' in menu
assert 'SetPage(Page::Maps)' in menu
assert 'case Page::Maps: return L"Maps / Custom Worlds";' in menu
assert 'case Page::SaveStates: SetPage(Page::World); break;' in menu
assert 'UpdateCustomMapLoad();' in tick
assert '#include "refunct/features/maps.hpp"' in api
assert 'src/maps.cpp' in cm and 'Missing src/maps.cpp' in cm
assert 'g_recording_tainted = true;' in native
assert 'CheatBit::CustomMap, CustomMapActive()' in records
assert 'L"Custom map loaded"' in records
assert 'if (!g_pb_cheats_allowed_for_run) g_run_pb_eligible = false;' in native
assert 'WriteRoot(' in native and 'SetPose(' in native
assert 'BeginApplyRoots(true)' in native
assert 'QueryPerformanceCounter' in native
assert 'FILE_ATTRIBUTE_REPARSE_POINT' in native
assert 'EnsureDirectory(folder)' in native
assert 'g_storage_dir + L"\\\\Maps"' in native
assert 'L"\\\\*"' in native
assert 's_map.cursor < s_map.actors.size() && processed < 8' in native
assert 'if (!CurrentSnapshotValid())' in native
# Validate the shipped parser contract instead of requiring an optional
# Star World user map which is absent from source-only distributions.
format_header=(r/'include/refunct/custom_map_format.hpp').read_text(encoding='utf-8')
assert 'constexpr size_t kClusterCount = 31;' in format_header
assert 'constexpr std::array<size_t, 6> kExpectedTotals = {251, 18, 37, 3, 9, 4};' in format_header
assert '"platforms", "cubes", "buttons", "lifts", "pipes", "springpads"' in format_header
assert 'map must contain exactly 31 clusters' in format_header
assert 'kExpectedTotals' in format_header
print('PASS: 31-section schema, F5 Maps menu, CMake link, bounded loader, rollback and cheat tracking')

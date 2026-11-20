"""Real hot-path C++20 harness for Fly/Noclip anti-hitch maintenance.

This tests source-extracted functions with minimal Win32/game stubs. It does not
measure live UE4 render frametimes or compile the Windows x86 ASI.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
storage = (root / 'src/storage.cpp').read_text(encoding='utf-8')
movement = (root / 'src/movement.cpp').read_text(encoding='utf-8')
menu = (root / 'src/menu.cpp').read_text(encoding='utf-8')
tick = (root / 'src/game_tick.cpp').read_text(encoding='utf-8')
storage_api = (root / 'include/refunct/features/storage.hpp').read_text(encoding='utf-8')
movement_api = (root / 'include/refunct/features/movement.hpp').read_text(encoding='utf-8')

assert 'void SaveMovementModeSettings();' in storage_api
assert 'void MaintainFlyingState(MyCharacter* player);' in movement_api
modes = storage.split('void SaveMovementModeSettings() {', 1)[1].split('\n}', 1)[0]
assert 'EnqueueSettings(std::move(modes))' in modes
full_settings = storage.split('void SaveSettings() {', 1)[1].split('\n}\n', 1)[0]
assert 'write(L"Movement", L"Flying"' in full_settings
assert 'write(L"Movement", L"Noclip"' in full_settings
assert 'write(L"Movement", L"FlySpeed"' in full_settings
for name in ('Fly', 'Noclip'):
    action = menu.split(f'case Action::{name}:', 1)[1].split('break;', 1)[0]
    assert 'SaveMovementModeSettings();' in action
    assert 'SaveSettings();' not in action
assert 'SetNoclip(player, !g_noclip);' in menu
assert 'SetFlying(player, !g_flying);' in menu
assert tick.count('MaintainFlyingState(player);') == 2
assert 'player->movement->movement_mode = 5;' not in tick
assert 'player->movement->max_fly_speed = g_fly_speed;' not in tick

helper = movement.split('void MaintainFlyingState(MyCharacter* player) {', 1)[1].split('\n}\n\nvoid SetFlying', 1)[0]
compiler = shutil.which('clang++') or shutil.which('g++')
if compiler is None:
    raise SystemExit('SKIP: no portable C++ compiler')
source = r'''
#include <cassert>
#include <string>
#include <tuple>
#include <vector>
#include <cstdint>
namespace refunct {
struct MovementComponent { uint8_t movement_mode = 1; float max_fly_speed = 600; };
struct MyCharacter { MovementComponent* movement; };
bool g_flying = false, g_noclip = false;
bool g_restore_flying_pending = false, g_restore_noclip_pending = false;
float g_fly_speed = 1200;
std::wstring g_settings_path = L"settings.ini";
std::vector<std::tuple<std::wstring,std::wstring,std::wstring>> writes;
struct IniEntry { std::wstring section, key, value; };
int WritePrivateProfileStringW(const wchar_t* section, const wchar_t* key,
                               const wchar_t* value, const wchar_t*) {
    writes.emplace_back(section, key, value);
    return 1;
}
bool EnqueueSettings(std::vector<IniEntry>&& entries) {
    for (const auto& entry : entries)
        writes.emplace_back(entry.section, entry.key, entry.value);
    return true;
}
void SaveMovementModeSettings() {
''' + modes + r'''
}
void MaintainFlyingState(MyCharacter* player) {
''' + helper + r'''
}
}
int main() {
 using namespace refunct;
 MovementComponent movement{};
 MyCharacter character{&movement};
 MaintainFlyingState(&character);
 assert(movement.movement_mode == 1 && movement.max_fly_speed == 600);
 g_flying = true;
 MaintainFlyingState(&character);
 assert(movement.movement_mode == 5 && movement.max_fly_speed == 1200);
 // Native character tick may replace fields and must be corrected afterwards.
 movement.movement_mode = 4; movement.max_fly_speed = 600;
 MaintainFlyingState(&character);
 assert(movement.movement_mode == 5 && movement.max_fly_speed == 1200);
 g_fly_speed = 2500;
 MaintainFlyingState(&character);
 assert(movement.max_fly_speed == 2500);
 g_flying = false;
 movement.movement_mode = 1;
 MaintainFlyingState(&character);
 assert(movement.movement_mode == 1);
 g_flying = true; g_noclip = true;
 SaveMovementModeSettings();
 assert(writes.size() == 2);
 assert(std::get<1>(writes[0]) == L"Flying" && std::get<2>(writes[0]) == L"1");
 assert(std::get<1>(writes[1]) == L"Noclip" && std::get<2>(writes[1]) == L"1");
 writes.clear();
 g_flying = false; g_noclip = false;
 SaveMovementModeSettings();
 assert(writes.size() == 2);
 assert(std::get<2>(writes[0]) == L"0" && std::get<2>(writes[1]) == L"0");
 // Pending restore should remain persisted across actor handoff.
 writes.clear();
 g_restore_flying_pending = true; g_restore_noclip_pending = true;
 SaveMovementModeSettings();
 assert(std::get<2>(writes[0]) == L"1" && std::get<2>(writes[1]) == L"1");
 writes.clear();
 g_settings_path.clear();
 SaveMovementModeSettings();
 assert(writes.empty());
}
'''
with tempfile.TemporaryDirectory(prefix='refunct-fly-noclip-test-') as td:
    cpp = Path(td) / 'test.cpp'
    out = Path(td) / 'test'
    cpp.write_text(source, encoding='utf-8')
    r = subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror',
                        '-O2', str(cpp), '-o', str(out)],
                        capture_output=True, text=True, timeout=30)
    assert r.returncode == 0, r.stderr
    r = subprocess.run([str(out)], capture_output=True, text=True, timeout=20)
    assert r.returncode == 0, r.stderr
print('PASS: two-key movement persistence, pending restores, and fly state corrections')

"""Fast static integrity checks, not a replacement for an MSVC Win32 build."""
from pathlib import Path
import re
root = Path(__file__).resolve().parents[1]
baseline_path = root / 'reference/original_mod_v9.30.cpp'
original = baseline_path.read_text(encoding='utf-8') if baseline_path.exists() else ''
modules = {p.name: p.read_text(encoding='utf-8') for p in (root/'src').glob('*.cpp')}
combined = '\n'.join(modules.values())
pattern = re.compile(r'(?m)^((?:void|bool|int|int32_t|uint32_t|float|double|DWORD|LRESULT|std::wstring|std::vector<std::wstring>|TunedFloatProperty|SumOfBest|GoldUpdate|const LevelState\*|const wchar_t\*|Vec3|void\*|BOOL)\s+(?:(?:__fastcall|CALLBACK|WINAPI)\s+)?([A-Za-z_]\w*)\s*\()')

def definitions(text):
    result = []
    for match in pattern.finditer(text):
        i = match.end()
        depth = 1
        while i < len(text) and depth > 0:
            if text[i] == '(': depth += 1
            if text[i] == ')': depth -= 1
            i += 1
        brace = text.find('{', i)
        semi = text.find(';', i)
        if brace >= 0 and (semi < 0 or brace < semi):
            result.append(match.group(2))
    return result

original_functions = set(definitions(original))
new_functions = definitions(combined)
# Multiple translation units may have TU-local functions with matching names.
# Do not conflate that with duplicate symbols in one translation unit.
for module_name, module_text in modules.items():
    local_definitions = definitions(module_text)
    assert len(local_definitions) == len(set(local_definitions)), (module_name, 'duplicate definitions')
# Intentional post-refactor changes: direct jump was removed, and compact
# menu display selectors were introduced after the original v9.30 baseline.
removed_by_design = {'ToggleScrollJumpMethod', 'CallDirectAssistedJump',
                     'ReleaseDirectJumpIfPending', 'JumpMethodLabel',
                     'ToggleJumpAssist', 'ToggleJumpTargets'}
added_by_design = {'StartupTrace', 'DiagnosticMode', 'JumpAssistModeLabel', 'CycleJumpAssistMode',
                   'FastestDeltaMode', 'ChangeSplitRowMode', 'SplitRowModeLabel',
                   'CycleFastestDeltaMode', 'FastestDeltaModeLabel',
                   'ProgressDisplayModeLabel', 'CycleProgressDisplayMode',
                   'FormatMilliseconds', 'CheatRecordModeLabel',
                   'CycleCheatRecordMode', 'HudCornerLabel', 'CycleHudCorner', 'NoArtificialRunProgress',
                   'RecordsAllowedForPB', 'RecordsAllowedForSegments'}
assert not original or original_functions - removed_by_design == set(new_functions) - added_by_design, (
    f'missing={original_functions-set(new_functions)-removed_by_design}, '
    f'added={set(new_functions)-original_functions-added_by_design}')
api = '\n'.join(h.read_text() for h in (root/'include').rglob('*.hpp'))
pure = ''
for fn in original_functions - removed_by_design - {'DllMain'}:
    assert re.search(r'\b'+re.escape(fn)+r'\s*\(',api+'\n'+pure), 'missing prototype: '+fn
assert 'static_assert(sizeof(void*) == 4' in (root/'include/refunct/game_abi.hpp').read_text()
assert 'void StartupTrace(const char* message);' in (root/'include/refunct/features/bootstrap.hpp').read_text(), 'StartupTrace declaration unavailable to storage.cpp'
assert '#include <MinHook.h>' in modules['bootstrap.cpp']
assert 'SUFFIX ".asi"' in (root/'CMakeLists.txt').read_text()
print(f'PASS: retained functions accounted for; modular additions checked across {len(modules)} source files')

# Explicit checks for the jump/climb selector.
assert 'CycleJumpAssistMode(true,' in modules['menu.cpp']
assert 'CycleJumpAssistMode(false,' in modules['menu.cpp']
assert 'Hold targets:' not in modules['menu.cpp']
assert 'kScrollClimbHoldMs' in modules['movement.cpp']
assert 'JumpTargetsFromStoredValue' in modules['storage.cpp']
assert 'synthetic_climb_held' in api
if not original:
    print('NOTE: original baseline absent; baseline function inventory not checked')

# Cheat saving policy persists and only grants movement-cheat exceptions.
assert 'CheatRecordMode' in (root/'include/refunct/pure.hpp').read_text()
assert 'CheatRecordMode' in modules['storage.cpp']
assert 'Save with cheats:' in modules['menu.cpp']
assert 'RecordsAllowedForSegments()' in modules['records.cpp']
assert 'g_pb_cheats_allowed_for_run' in modules['movement.cpp']
assert 'NoArtificialRunProgress()' in modules['records.cpp']
assert 'case Page::RestartOptions:' in modules['menu.cpp']
print('PASS: recording policy and Settings restart navigation present')

# Restart with Progress is button-restoration-only; no soft map travel is reachable.
assert 'Progress restart method:' not in modules['menu.cpp']
assert 'ReloadCurrentLevelFromSave' not in combined
assert 'bool ExecuteFullLevelReload(MyCharacter* player)' in modules['restart.cpp']
assert 'OpenLevel' in modules['restart.cpp']
assert 'g_progress_restart_pending && !g_practice_restart_requested' not in modules['game_tick.cpp']
assert 'PracticeOrigin::ProgressRestart' in modules['practice.cpp']
print('PASS: progress restart is button-only, independent from full-level reload')

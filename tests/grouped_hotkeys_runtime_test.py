"""Check grouped hotkey routing with real C++ enum / category code.

Portable test of action-to-submenu mapping and row indexing. Cannot replace the
Windows x86 ASI build or an in-game test.
"""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
header = (root / 'include/refunct/state.hpp').read_text(encoding='utf8')
menu = (root / 'src/menu.cpp').read_text(encoding='utf8')
input_source = (root / 'src/input.cpp').read_text(encoding='utf8')
hud = (root / 'src/hud.cpp').read_text(encoding='utf8')

assert 'Ghost, GhostReplay,' in header, 'Legacy menu page ordering changed'
action_enum = 'enum class Action : size_t {' + header.split('enum class Action : size_t {', 1)[1].split('};', 1)[0] + '};'
page_enum = 'enum class Page {' + header.split('enum class Page {', 1)[1].split('};', 1)[0] + '};'
ids = re.findall(r'\b([a-zA-Z][a-zA-Z0-9_]*)\s*,',
    re.sub(r'//[^\n]*', '', action_enum.split('{', 1)[1].rsplit('}', 1)[0]))
assert ids[-1] != 'Count'  # Count is deliberately last, without a comma.
assert len(ids) == len(set(ids)), 'Action IDs duplicated'

# Recover exactly the production grouping code, rather than reimplementing it.
start = menu.index('namespace {\nstruct HotkeyCategory {')
end = menu.index('} // namespace\n\nstd::vector<std::wstring> BuildEntriesForPage', start)
helpers = menu[start + len('namespace {\n'):end]
assert len(re.findall(r'case Action::(\w+):', helpers)) == len(ids) + 1
assert 'HotkeyActionAtRow(g_page, i)' in menu
assert 'HotkeyActionAtRow(g_page, g_selection)' in menu
assert 'g_rebind_action = static_cast<int>(action)' in menu
assert 'g_keys[action].store(0)' in menu

capture = menu.split('if (g_rebinding.load()) {', 1)[1].split('// Search mode owns', 1)[0]
assert 'if (key == VK_BACK || key == VK_ESCAPE)' in capture
assert capture.index('g_keys[g_rebind_action].store(0)') < capture.index('if (!IsBindable(key))')
assert 'Key change cancelled' not in capture
assert 'ESC / BACKSPACE = NONE' in hud
assert 'if (key == VK_F5 && Hotkey(Action::Menu) == 0)' in input_source
assert 'if (key == VK_F5 && Hotkey(Action::Menu) == 0)' in menu
assert 'Hotkey(Action::Menu) == 0 ? L"F5 fallback"' in menu
assert 'raw <= static_cast<int>(Page::Crosshair)' in menu

# For each new action, defaults must be None to avoid clobbering user keys.
added = ['ToggleCubesHUD', 'ToggleTilesHUD', 'ToggleButtonNumberHUD',
    'ToggleKeyDisplayHUD', 'TogglePracticeStatusHUD', 'ToggleRecordStatusHUD',
    'ToggleComparisonHUD', 'ToggleBestSegmentHUD', 'TogglePBSplitHUD',
    'TogglePBGhost', 'ToggleGhostMode', 'ToggleGhostFastestSections',
    'ToggleGhostReplay', 'ToggleGhostReplayPause', 'SelectAllTiles']
for action in added:
    assert f'case Action::{action}:' in menu
    assert f'L"{action}", 0}}' in header
assert 'ToggleHudElement(HudElement::KeyDisplay)' in menu
assert 'SetPBGhostEnabled(!IsPBGhostEnabled())' in menu
assert 'SelectWorldTiles(true)' in menu

code = r'''
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <string>
#include <vector>
#include <iostream>
namespace refunct {
''' + action_enum + '\n' + page_enum + r'''
constexpr size_t kActionCount = static_cast<size_t>(Action::Count);
struct ActionInfo { const wchar_t* label; };
std::array<ActionInfo, kActionCount> kActions = [] {
    std::array<ActionInfo, kActionCount> all{};
    for (auto& item : all) item.label = L"Example";
    return all;
}();
std::array<std::atomic_int, kActionCount> g_keys{};
std::wstring KeyName(int v) { return std::to_wstring(v); }
''' + helpers + r'''
int Run() {
    size_t total = 0;
    for (const auto& category : kHotkeyCategories) {
        const auto entries = HotkeyCategoryEntries(category.page);
        assert(!entries.empty() && entries.back() == L"Back");
        assert(IsHotkeyCategoryPage(category.page));
        for (size_t row = 0; row + 1 < entries.size(); ++row) {
            const size_t id = HotkeyActionAtRow(category.page, static_cast<int>(row));
            assert(id < kActionCount);
            assert(HotkeyCategoryForAction(static_cast<Action>(id)) == category.page);
            ++total;
        }
        assert(HotkeyActionAtRow(category.page, static_cast<int>(entries.size() - 1)) == kActionCount);
        assert(HotkeyActionAtRow(category.page, -1) == kActionCount);
        assert(HotkeyActionAtRow(category.page, 9999) == kActionCount);
    }
    assert(total == kActionCount);
    assert(!IsHotkeyCategoryPage(Page::Hotkeys));
    std::cout << "PASS: " << total << " actions reachable in 6 categories; Back ignored\n";
    return 0;
}
}
int main() {return refunct::Run();}
'''
with tempfile.TemporaryDirectory() as temp:
    cpp = Path(temp) / 'hotkey_groups.cpp'
    exe = Path(temp) / 'hotkey_groups'
    cpp.write_text(code, encoding='utf-8')
    build = subprocess.run(['clang++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
        str(cpp), '-o', str(exe)], capture_output=True, text=True, timeout=30)
    assert build.returncode == 0, build.stderr
    run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
    assert run.returncode == 0, run.stderr
    print(run.stdout.strip())
print('PASS: Escape to None, safe F5 fallback, saved action IDs and 15 new shortcuts')

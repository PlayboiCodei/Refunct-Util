"""Split main-menu navigation and HUD-index integrity regression.

Uses the real C++ HUD enum and menu category arrays in a portable harness.
Windows ASI compilation and the in-game menu still require Windows testing.
"""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
header = (root / 'include/refunct/state.hpp').read_text(encoding='utf-8')
menu = (root / 'src/menu.cpp').read_text(encoding='utf-8')

def between(source, start, end):
    assert start in source and end in source, (start, end)
    return source.split(start, 1)[1].split(end, 1)[0]

# Never alter old page IDs or saved HUD indexes when rearranging menus.
page_names = re.sub(r'//[^\n]*', '', between(header, 'enum class Page {', '};'))
pages = [v.strip() for v in page_names.split(',') if v.strip()]
assert pages[pages.index('Splits'):pages.index('Splits') + 3] == ['Splits', 'SplitHud', 'SplitHudAppearance']
assert pages.index('HotkeysGhost') + 1 == pages.index('Splits')

entries = between(menu, 'std::vector<std::wstring> BuildEntriesForPage(Page page) {', 'void RefreshMenuSearch()')
actions = between(menu, 'void ActivateSelected(MyCharacter* player) {', 'namespace {\n// Left/Right settings')
back = between(menu, 'void GoBack() {', 'void ToggleProgressRiseAnimation()')
help_text = between(menu, 'std::wstring MenuSelectionDescription(', 'void RestoreHudDisplayDefaults() {')

def case(text, name):
    match = re.search(r'^\s{8}case Page::' + re.escape(name) + r':(?:(?!^\s{8}case Page::).)*', text, re.M | re.S)
    assert match, name
    return match.group(0)

assert 'L"Splits  >"' in case(entries, 'Main')
assert 'else if (i == 5) SetPage(Page::Splits)' in case(actions, 'Main')
assert 'L"Split Popups & Comparisons  >"' not in case(entries, 'Hud')
assert 'L"Split Popups & Comparisons  >"' in case(entries, 'Splits')
assert 'L"Saved Split Times  >"' in case(entries, 'Splits')
assert 'L"Split HUD Customisations  >"' in case(entries, 'Splits')
for i, page in enumerate(('SplitHud', 'HudTiming', 'HudRecordTimes', 'SplitHudAppearance')):
    assert f'else if (i == {i}) SetPage(Page::{page});' in case(actions, 'Splits') or (
        i == 0 and f'if (i == {i}) SetPage(Page::{page});' in case(actions, 'Splits'))
for name in ('SplitHud', 'HudTiming', 'HudRecordTimes', 'SplitHudAppearance'):
    assert f'case Page::{name}:' in back
assert 'SetPage(Page::Splits); break;' in back
assert 'SetPage(s_hud_style_return_page); break;' in back
assert 's_hud_style_return_page = g_page;' in case(actions, 'SplitHudAppearance')
assert 'case Page::SplitHud:' in help_text
assert 'case Page::SplitHudAppearance:' in help_text
assert 'raw <= static_cast<int>(Page::Crosshair); ++raw)' in menu

# Test the actual C++ arrays so no HUD ID is duplicated or lost during UI filtering.
hud_enum = 'enum class HudElement : size_t {' + between(header, 'enum class HudElement : size_t {', '};') + '};'
array_source = '// Menu-only ordering:' + between(menu, '// Menu-only ordering:', 'static_assert(kGeneralHudElements.size()')
array_source = 'constexpr size_t kSideHudElementCount = static_cast<size_t>(HudElement::KeyDisplay);\n' + array_source
code = '''
#include <array>
#include <cassert>
#include <cstddef>
#include <iostream>
''' + hud_enum + '\n' + array_source + '''
int main() {
    std::array<int, static_cast<size_t>(HudElement::Count)> counts{};
    for (auto element : kGeneralHudElements) ++counts[static_cast<size_t>(element)];
    for (auto element : kSplitHudElements) ++counts[static_cast<size_t>(element)];
    for (size_t i = 0; i < static_cast<size_t>(HudElement::KeyDisplay); ++i)
        assert(counts[i] == 1);
    assert(counts[static_cast<size_t>(HudElement::KeyDisplay)] == 0);
    assert(kSplitHudElements.size() == 7);
    assert(kGeneralHudElements.size() == 14);
    assert(kSplitHudElements[0] == HudElement::Timer);
    assert(kSplitHudElements[6] == HudElement::PBSplitTime);
    std::cout << "PASS: 7 split HUD + 14 general HUD, unique persistent indices\\n";
}
'''
with tempfile.TemporaryDirectory() as td:
    cpp = Path(td) / 'splits.cpp'
    exe = Path(td) / 'splits'
    cpp.write_text(code, encoding='utf-8')
    build = subprocess.run([shutil.which('clang++') or shutil.which('g++') or 'clang++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                            str(cpp), '-o', str(exe)], capture_output=True, text=True, timeout=20)
    assert build.returncode == 0, build.stderr
    run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=5)
    assert run.returncode == 0, run.stderr
    print(run.stdout.strip())
print('PASS: Splits main menu, descriptions, saved-time reachability, search and Back navigation')

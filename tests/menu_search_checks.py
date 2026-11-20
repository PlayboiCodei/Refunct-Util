"""Search navigation, input capture, legacy menu and pure matching regression."""
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
menu = (root / 'src/menu.cpp').read_text(encoding='utf-8')
input_cpp = (root / 'src/input.cpp').read_text(encoding='utf-8')
tick = (root / 'src/game_tick.cpp').read_text(encoding='utf-8')
movement = (root / 'src/movement.cpp').read_text(encoding='utf-8')
header = (root / 'include/refunct/state.hpp').read_text(encoding='utf-8')
menu_header = (root / 'include/refunct/features/menu.hpp').read_text(encoding='utf-8')
hud = (root / 'src/hud.cpp').read_text(encoding='utf-8')

assert 'HudAppearance, HudAppearanceEdit, HudKeyDisplay, Search' in header
assert 'case Page::Search:' in menu
assert 'L"Search settings  [/]  >"' in menu
assert 'void OpenMenuSearch()' in menu
assert 'void RefreshMenuSearch()' in menu
assert 'MenuSearchTerms query(s_menu_search_query)' in menu
assert 'query.matches(breadcrumb, label)' in menu
assert 's_menu_search_matches.push_back({page, static_cast<int>(row), label, breadcrumb})' in menu
assert 'SetPage(found.page);' in menu and 'g_selection = found.row;' in menu
assert 's_menu_search_return_selection = g_selection;' in menu
assert 'g_selection = s_menu_search_return_selection;' in menu
assert 'case VK_BACK:' in menu or 'if (key == VK_BACK)' in menu
assert 'key == VK_OEM_2' in menu
assert 'void QueueMenuSearchKey(int key)' in menu
assert 'SRWLOCK s_menu_search_key_lock' in menu
assert 's_menu_search_key_queue' in menu
assert 'std::mutex s_menu_search_key_mutex;' not in menu
assert 'menu_search_key' in input_cpp and 'QueueMenuSearchKey(key);' in input_cpp
assert 'DrainMenuSearchKeys(player);' in tick
assert 'void DrainMenuSearchKeys(MyCharacter* player)' in menu
assert 'IsMenuSearchTypingKey(VK_SHIFT)' in movement
assert 'IsMenuSearchTypingKey(VK_SPACE)' in movement
assert 'bool IsMenuSearchTypingKey(int key);' in menu_header
assert 'g_page == Page::Search' in hud
assert 'Search never changes the setting itself.' in menu
assert 'BuildEntriesForPage(page)' in menu

compiler = shutil.which('g++') or shutil.which('clang++')
if compiler:
    with tempfile.TemporaryDirectory() as td:
        binary = Path(td) / 'search_pure_test'
        subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror',
                        '-I', str(root / 'include'),
                        str(root / 'tests/menu_search_pure_test.cpp'),
                        '-o', str(binary)], check=True, timeout=20)
        subprocess.run([str(binary)], check=True, timeout=5)
    print('PASS: search source integration + portable C++ matcher')
else:
    print('PASS: search source integration (portable compiler unavailable)')

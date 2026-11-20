"""Compile the actual flat reflection menu rows and input routes with mock setters.
This validates navigation and setting selection, not native Unreal rendering.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
menu = (root / 'src/menu.cpp').read_text(encoding='utf-8')
state = (root / 'include/refunct/state.hpp').read_text(encoding='utf-8')

def between(source, start, end):
    return source.split(start, 1)[1].split(end, 1)[0]

page_enum = between(state, 'enum class Page {', '};')
render = between(menu, 'case Page::WaterReflections:\n            return {',
                 'case Page::WaterObjectReflections:')
help_rows = between(menu, 'case Page::WaterReflections:\n            return DescriptionAt(selected, {',
                    'case Page::WaterObjectReflections:')
helper = 'bool ToggleWaterReflectionRow(int row) {' + between(
    menu, 'bool ToggleWaterReflectionRow(int row) {', 'void ActivateSelected(')
activate = between(menu, 'void ActivateSelected(MyCharacter* player) {', 'void HandleHorizontalWorld(')
enter_prefix = activate.split('switch (g_page) {', 1)[0]
enter = 'case Page::WaterReflections:' + between(activate, 'case Page::WaterReflections:', 'case Page::WaterObjectReflections:')
horizontal_prefix = 'case Page::WaterReflections:\n            ToggleWaterReflectionRow'
horizontal = horizontal_prefix + between(menu, horizontal_prefix, 'case Page::WorldJumpPadColours:')
back = 'case Page::WaterReflections:' + between(between(menu,'void GoBack() {','void ToggleProgressRiseAnimation()'), 'case Page::WaterReflections:', 'case Page::WorldIndividualSections:')
setpage = 'void SetPage(Page page) {' + between(menu, 'void SetPage(Page page) {','void ExecuteAction(')
entries = 'std::vector<std::wstring> Entries() {' + between(menu,'std::vector<std::wstring> Entries() {','// A description belongs')
search = between(menu, 'void RefreshMenuSearch() {', 'void OpenMenuSearch() {')
assert 'page == Page::WaterObjectReflections || page == Page::WaterBeamReflections) continue;' in search
assert 'case Page::WaterReflections:\n        case Page::WorldJumpPadColours:' in menu
assert 'SetPage(Page::WaterObjectReflections);' not in menu
assert 'SetPage(Page::WaterBeamReflections);' not in menu
assert 'Water Object Reflections  >' not in menu and 'Water Beam Reflections  >' not in menu

names = ['WaterReflections', 'WaterReflectedWalls', 'WaterReflectedButtons',
         'WaterReflectedPipes', 'WaterReflectedJumpPads', 'WaterReflectedCubes', 'WaterReflectedBeacons']
getters = '\n'.join(f'bool Are{name}Visible() {{ return flags[{i}]; }}\n'
                    f'void Set{name}Visible(bool value) {{ flags[{i}]=value; ++calls[{i}]; }}'
                    for i, name in enumerate(names))
code = f'''#include <array>
#include <atomic>
#include <cassert>
#include <string>
#include <vector>
#include <initializer_list>
enum class Page {{{page_enum}}};
Page g_page=Page::WaterReflections;
int g_selection=0;
std::atomic_bool g_coordinate_editing{{false}}, s_menu_search_capture{{false}}, g_menu_open{{true}};
void FinishCoordinateEdit(bool) {{ g_coordinate_editing=false; }}
std::wstring MenuHotkeyDisplayName() {{ return L"F5"; }}
std::array<bool,7> flags{{}};
std::array<int,7> calls{{}};
{getters}
{setpage}
std::vector<std::wstring> BuildEntries() {{ return {{{render} }}
{entries}
std::wstring DescriptionAt(int i, std::initializer_list<const wchar_t*> rows) {{
    assert(i>=0 && static_cast<size_t>(i)<rows.size()); return *(rows.begin()+i);
}}
std::wstring Help(int selected) {{ return DescriptionAt(selected, {{{help_rows} }}
void GoBack() {{ switch(g_page) {{ {back} default: assert(false); }} }}
{helper}
void Enter() {{ {enter_prefix} switch(g_page) {{ {enter} default: assert(false); }} }}
void Horizontal(int key) {{ assert(key==37 || key==39); switch(g_page) {{ {horizontal} default: assert(false); }} }}
int main() {{
    const wchar_t* labels[]={{L"All water reflections: ", L"Reflect walls/platforms: ",
        L"Reflect buttons: ", L"Reflect pipes: ", L"Reflect jump pads: ", L"Reflect cubes: ", L"Reflect button beacon beams: "}};
    const wchar_t* help_keywords[]={{L"planar reflection",L"wall",L"button press",L"travel-pipe",L"jump pad",L"cube",L"Beacon"}};
    for(int mask=0;mask<128;++mask) {{
        for(int i=0;i<7;++i) flags[i]=(mask&(1<<i))!=0;
        auto rows=Entries(); assert(rows.size()==9);
        assert(rows[7]==L"Back" && rows[8]==L"Hide menu  [F5]");
        for(int row=0;row<7;++row) {{
            assert(rows[row]==std::wstring(labels[row])+(flags[row]?L"ON":L"OFF")+L"  < >");
            assert(Help(row).find(help_keywords[row])!=std::wstring::npos);
            for(int input=0;input<3;++input) {{
                auto old=flags; calls.fill(0); g_selection=row;
                if(input==0) Enter(); else Horizontal(input==1?37:39);
                for(int j=0;j<7;++j) {{
                    assert(flags[j]==(j==row?!old[j]:old[j])); assert(calls[j]==(j==row?1:0));
                }}
                assert(g_page==Page::WaterReflections && g_selection==row);
                flags=old;
            }}
        }}
    }}
    auto old=flags; calls.fill(0);
    for(int row:{{-1,7,8,999}}) {{g_selection=row; Horizontal(37); Horizontal(39);}}
    assert(flags==old && (calls==std::array<int,7>{{}}));
    g_selection=7; Enter(); assert(g_page==Page::Customisation && g_selection==0 && flags==old);
    SetPage(Page::WaterReflections); g_selection=8; Enter();
    assert(!g_menu_open && g_page==Page::WaterReflections && g_selection==8 && flags==old);
    SetPage(Page::WaterObjectReflections); assert(g_page==Page::WaterReflections && g_selection==0);
    SetPage(Page::WaterBeamReflections); assert(g_page==Page::WaterReflections && g_selection==0);
    GoBack(); assert(g_page==Page::Customisation);
}}
'''
compiler = shutil.which('g++') or shutil.which('clang++')
assert compiler, 'Portable C++ compiler required'
with tempfile.TemporaryDirectory() as td:
    source = Path(td) / 'test.cpp'
    exe = Path(td) / 'test'
    source.write_text(code, encoding='utf-8')
    subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', str(source), '-o', str(exe)], check=True, timeout=20)
    subprocess.run([str(exe)], check=True, timeout=5)
print('PASS: all seven reflection rows, 128 setting combinations, Enter/Left/Right, help, Back, Hide and retired pages')

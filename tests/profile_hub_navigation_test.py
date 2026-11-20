"""Compile the production hub routing and Back cases with minimal menu mocks."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
menu = (root / 'src/menu.cpp').read_text()
state = (root / 'include/refunct/state.hpp').read_text()
page_enum = state.split('enum class Page {', 1)[1].split('};', 1)[0]
actions = menu.split('void ActivateSelected(MyCharacter* player) {', 1)[1].split('namespace {\n// Left/Right settings', 1)[0]
back = menu.split('void GoBack() {', 1)[1].split('void ToggleProgressRiseAnimation()', 1)[0]

def case(code, name):
    return code.split(f'case Page::{name}:', 1)[1].split('        case Page::', 1)[0]

hub = case(actions, 'ProfilesHub')
assert re.findall(r'SetPage\(Page::(\w+)\)', hub) == ['Profiles', 'CustomisationProfiles', 'HudProfiles', 'WorldProfiles', 'HotkeyProfiles']
entries = menu.split('std::vector<std::wstring> BuildEntriesForPage(Page page) {', 1)[1].split('void RefreshMenuSearch()', 1)[0]
assert re.findall(r'L"([^"]+)"', case(entries,'ProfilesHub')) == ['Replay / Split Profiles  >', 'Customisation Profiles  >', 'HUD Profiles  >', 'World Profiles  >', 'Hotkey Profiles  >', 'Back']
assert 'else if (i == 9) SetPage(Page::ProfilesHub);' in case(actions,'Main')
for name, original, var in [('HudProfiles','Hud','s_hud_profiles_return_page'),('CustomisationProfiles','Customisation','s_customisation_profiles_return_page'),('WorldProfiles','World','s_world_profiles_return_page'),('HotkeyProfiles','Hotkeys','s_hotkey_profiles_return_page')]:
    assert f'{var} = Page::{original};' in case(actions,original)
    assert f'{var} = Page::ProfilesHub;' in hub
    assert f'SetPage({var});' in case(back,name)
    assert f'SetPage(Page::{name});' in case(back,'ConfirmDelete'+(name.removesuffix('s')))
    assert f'found.page == Page::{name} && s_menu_search_return_page != Page::{name}' in case(actions,'Search')
assert 'raw <= static_cast<int>(Page::Crosshair)' in menu
assert 'SetPage(Page::Profiles);' in case(back,'GameplayRecords')
assert 'SetPage(Page::ProfilesHub);' in case(back,'Profiles')
compiler = shutil.which('g++') or shutil.which('clang++')
assert compiler
# Extract the actual switch branches rather than recreating the hub implementation.
back_cases = ''.join(f'case Page::{n}:' + case(back,n) for n in ['HudProfiles','ConfirmDeleteHudProfile','CustomisationProfiles','ConfirmDeleteCustomisationProfile','WorldProfiles','ConfirmDeleteWorldProfile','HotkeyProfiles','ConfirmDeleteHotkeyProfile','Profiles','GameplayRecords','ProfilesHub'])
# ProfilesHub shares a grouped case with other pages; the extracted body is the same.
source = f'''#include <cassert>
enum class Page {{{page_enum}}};
Page g_page=Page::ProfilesHub;
Page s_hud_profiles_return_page=Page::Hud;
Page s_customisation_profiles_return_page=Page::Customisation;
Page s_world_profiles_return_page=Page::World;
Page s_hotkey_profiles_return_page=Page::Hotkeys;
int replay=0, custom=0, hud=0, world=0, hotkeys=0;
void SetPage(Page p) {{ g_page=p; }}
void RefreshProfiles() {{ ++replay; }}
void RefreshCustomisationProfiles() {{ ++custom; }}
void RefreshHudProfiles() {{ ++hud; }}
void RefreshWorldProfiles() {{ ++world; }}
void RefreshHotkeyProfiles() {{ ++hotkeys; }}
void GoBack() {{ switch(g_page) {{ {back_cases} default: assert(false); }} }}
void Activate(int i) {{ switch(g_page) {{ case Page::ProfilesHub: {hub} default: assert(false); }} }}
int main() {{
 Activate(0); assert(g_page==Page::Profiles && replay==1); SetPage(Page::GameplayRecords); GoBack(); assert(g_page==Page::Profiles); GoBack(); assert(g_page==Page::ProfilesHub);
 Activate(1); assert(g_page==Page::CustomisationProfiles && custom==1); SetPage(Page::ConfirmDeleteCustomisationProfile); GoBack(); assert(g_page==Page::CustomisationProfiles); GoBack(); assert(g_page==Page::ProfilesHub);
 Activate(2); assert(g_page==Page::HudProfiles && hud==1); SetPage(Page::ConfirmDeleteHudProfile); GoBack(); assert(g_page==Page::HudProfiles); GoBack(); assert(g_page==Page::ProfilesHub);
 Activate(3); assert(g_page==Page::WorldProfiles && world==1); SetPage(Page::ConfirmDeleteWorldProfile); GoBack(); assert(g_page==Page::WorldProfiles); GoBack(); assert(g_page==Page::ProfilesHub);
 Activate(4); assert(g_page==Page::HotkeyProfiles && hotkeys==1); SetPage(Page::ConfirmDeleteHotkeyProfile); GoBack(); assert(g_page==Page::HotkeyProfiles); GoBack(); assert(g_page==Page::ProfilesHub); GoBack(); assert(g_page==Page::Main);
 s_hotkey_profiles_return_page=Page::Hotkeys; SetPage(Page::HotkeyProfiles); GoBack(); assert(g_page==Page::Hotkeys);
 s_world_profiles_return_page=Page::World; SetPage(Page::WorldProfiles); GoBack(); assert(g_page==Page::World);
 s_hud_profiles_return_page=Page::Hud; SetPage(Page::HudProfiles); GoBack(); assert(g_page==Page::Hud);
 s_customisation_profiles_return_page=Page::Customisation; SetPage(Page::CustomisationProfiles); GoBack(); assert(g_page==Page::Customisation);
}}
'''
with tempfile.TemporaryDirectory() as td:
    src=Path(td)/'test.cpp'; exe=Path(td)/'test'; src.write_text(source)
    subprocess.run([compiler,'-std=c++20','-Wall','-Wextra','-Werror',str(src),'-o',str(exe)],check=True,timeout=20)
    subprocess.run([str(exe)],check=True,timeout=5)
print('PASS: shared profile hub routes, refresh, confirmation and Back navigation (compiled production branches)')

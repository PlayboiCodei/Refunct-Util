"""World gameplay controls remain separate from cosmetic controls (menu-level)."""
from pathlib import Path
root=Path(__file__).resolve().parents[1]
menu=(root/'src/menu.cpp').read_text()
state=(root/'include/refunct/state.hpp').read_text()
render=menu.split('std::vector<std::wstring> BuildEntries() {',1)[1].split('const wchar_t* MenuBreadcrumb(',1)[0]
activate=menu.split('void ActivateSelected(MyCharacter* player) {',1)[1].split('void ProcessKey(',1)[0]
back=menu.split('void GoBack() {',1)[1].split('void ToggleProgressRiseAnimation()',1)[0]
crumbs=menu.split('const wchar_t* MenuBreadcrumb(Page page) {',1)[1].split('std::vector<std::wstring> Entries()',1)[0]
def page(code,p):
    start=code.index(f'case Page::{p}:')
    end=code.find('case Page::',start+len(f'case Page::{p}:'))
    return code[start:end if end >= 0 else None]
world=page(render,'World'); custom=page(render,'Customisation')
world_actions=page(activate,'World'); custom_actions=page(activate,'Customisation')
assert 'CustomisationSky' in state
assert 'L"Sky  >"' in custom and 'L"Sky  >"' not in world
assert 'L"Water  >"' in world and 'L"Water  >"' in custom
assert 'else if (i == 8) SetPage(Page::WorldWater);' in world_actions
assert 'else if (i == 12) ResetWorldGameplayOptions();' in world_actions
assert 'else if (i == 9) SetPage(Page::CustomisationSky);' in custom_actions
assert 'else if (i == 10) SetPage(Page::CustomisationShadows);' in custom_actions
assert 'else if (i == 11) SetPage(Page::CustomisationPillars);' in custom_actions
assert 'else if (i == 16) ResetWorldCustomisation();' in custom_actions
assert 'SetPage(Page::WorldWaterColours);' in custom_actions
for page_name in ['CustomisationTime','CustomisationSky','CustomisationPillars','WorldWaterColours']:
    assert f'case Page::{page_name}:' in back
    assert f'case Page::{page_name}:' in crumbs
assert 'ResetSkyColours();' in menu.split('void ResetWorldCustomisation() {',1)[1].split('// Page through large',1)[0]
assert 'ResetWorldCustomisation()' not in world_actions
assert 'SetSkyColourHex(' in menu and 'OpenSkyColourPicker(' in menu
print('Customisation vs World menu separation passed')

# Keep all settings first, followed by profiles, reset, and Back.
import re
labels = re.findall(r'L"([^"\n]*)"', custom)
assert labels[-5:] == ['Water Reflections  >', 'Crosshair  >', 'Customisation Profiles  >', 'Restore default customisations', 'Back'], labels
profile_index = labels.index('Customisation Profiles  >')
reset_index = labels.index('Restore default customisations')
reflection_index = labels.index('Water Reflections  >')
assert f'else if (i == {reflection_index}) SetPage(Page::WaterReflections);' in custom_actions
assert re.search(rf'else if \(i == {profile_index}\) \{{\s*s_customisation_profiles_return_page = Page::Customisation;\s*RefreshCustomisationProfiles\(\);\s*SetPage\(Page::CustomisationProfiles\);\s*\}}', custom_actions)
help_section = menu.split('std::wstring MenuSelectionDescription(', 1)[1].split('void GoBack()', 1)[0]
help_rows = re.findall(r'L"([^"\n]*)"', page(help_section, 'Customisation'))
assert len(help_rows) == len(labels) - 1
assert help_rows[reflection_index].startswith('Control water reflections')
assert help_rows[profile_index].startswith('Create and switch between saved sets')
assert help_rows[reset_index].startswith('Restore default camera FOV')
assert f'else if (i == {reset_index}) ResetWorldCustomisation();' in custom_actions
print('PASS: settings, profiles, reset and Back have matching actions and help')

# Compile the actual row list and activation branch, not a handwritten replica.
import shutil
import subprocess
import tempfile
compiler = shutil.which('g++') or shutil.which('clang++')
assert compiler, 'Portable C++ compiler required'
rows_body = custom.split('case Page::Customisation:', 1)[1]
action_body = custom_actions.split('case Page::Customisation:', 1)[1]
help_body = page(help_section, 'Customisation').split('case Page::Customisation:', 1)[1]
expected_pages = [
    'WorldButtonColours', 'WorldButtonBeacons', 'WorldTileColours', 'WorldWalls',
    'WorldTubeColours', 'WorldPlatformColours', 'WorldJumpPadColours', 'WorldWaterColours',
    'CustomisationTime', 'CustomisationSky', 'CustomisationShadows', 'CustomisationPillars',
    'WorldPlayer', 'WaterReflections', 'Crosshair', 'CustomisationProfiles'
]
enum = (root/'include/refunct/state.hpp').read_text().split('enum class Page {', 1)[1].split('};', 1)[0]
expected = ', '.join('Page::'+name for name in expected_pages)
code = f"""#include <cassert>
#include <string>
#include <vector>
enum class Page {{{enum}}};
Page current = Page::Customisation;
Page s_water_colour_return_page = Page::Main;
Page s_customisation_profiles_return_page = Page::Main;
int resets = 0, refreshes = 0, backs = 0;
void SetPage(Page p) {{ current = p; }}
void ResetWorldCustomisation() {{ ++resets; }}
void RefreshCustomisationProfiles() {{ ++refreshes; }}
void GoBack() {{ ++backs; }}
std::wstring DescriptionAt(int i, std::vector<std::wstring> descriptions) {{
    return descriptions.at(i);
}}
std::vector<std::wstring> Rows() {{ {rows_body} }}
std::wstring Help(int selected) {{ {help_body} }}
void Activate(int i) {{ switch(current) {{ case Page::Customisation: {action_body} default: assert(false); }} }}
int main() {{
    const auto rows = Rows();
    assert(rows.size()==18);
    assert(rows[13]==L"Water Reflections  >");
    assert(rows[14]==L"Crosshair  >");
    assert(rows[15]==L"Customisation Profiles  >");
    assert(rows[16]==L"Restore default customisations");
    assert(rows[17]==L"Back");
    const Page destinations[] = {{{expected}}};
    for (int i=0; i<16; ++i) {{
        current=Page::Customisation;
        Activate(i);
        assert(current==destinations[i] && resets==0 && backs==0);
    }}
    assert(refreshes==1 && s_customisation_profiles_return_page==Page::Customisation);
    assert(s_water_colour_return_page==Page::Customisation);
    assert(Help(13).starts_with(L"Control water reflections"));
    assert(Help(15).starts_with(L"Create and switch"));
    assert(Help(16).starts_with(L"Restore default camera FOV"));
    current=Page::Customisation;
    Activate(16); assert(resets==1 && current==Page::Customisation && backs==0);
    Activate(17); assert(backs==1 && resets==1);
}}
"""
with tempfile.TemporaryDirectory() as directory:
    source = Path(directory)/'menu.cpp'
    executable = Path(directory)/'menu_test'
    source.write_text(code)
    subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', str(source), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
print('PASS: compiled production rows, all 18 selections, and reordered help')

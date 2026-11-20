"""Regression checks for Sky/Fog submenu grouping.

Verifies row/index contracts and routing without launching Win32 Refunct.
"""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
menu = (root / 'src/menu.cpp').read_text(encoding='utf-8')
state = (root / 'include/refunct/state.hpp').read_text(encoding='utf-8')

pages = ('CustomisationSky', 'CustomisationSkyColours', 'CustomisationSkyVisibility',
         'CustomisationFogColours', 'CustomisationFogVisibility')

def page_cases(section, name):
    label = f'        case Page::{name}:'
    assert section.count(label) == 1, (name, section.count(label))
    return section.split(label, 1)[1].split('        case Page::', 1)[0]

build = menu.split('std::vector<std::wstring> BuildEntriesForPage(Page page) {', 1)[1].split(
    '\nvoid RefreshMenuSearch()', 1)[0] if '\nvoid RefreshMenuSearch()' in menu.split('std::vector<std::wstring> BuildEntriesForPage(Page page) {', 1)[1] else menu.split('std::vector<std::wstring> BuildEntriesForPage(Page page) {', 1)[1].split('void RefreshMenuSearch() {',1)[0]
actions = menu.split('void ActivateSelected(MyCharacter* player) {', 1)[1].split('namespace {\n// Left/Right settings', 1)[0]
arrows = menu.split('void HandleHorizontalMovement(int key, MyCharacter* player) {', 1)[1].split('} // namespace\n\nvoid ProcessKey(int key, MyCharacter* player) {', 1)[0]
back = menu.split('void GoBack() {', 1)[1].split('void ToggleProgressRiseAnimation()', 1)[0]
crumbs = menu.split('const wchar_t* MenuBreadcrumb(Page page) {', 1)[1].split('std::vector<std::wstring> Entries()', 1)[0]

# Appending instead of inserting preserves historical page IDs.
page_enum = state.split('enum class Page {', 1)[1].split('};', 1)[0]
assert page_enum.index('CustomisationPillars, WorldObjectRemoval') < page_enum.index('CustomisationSkyColours')
assert page_enum.index('CustomisationSkyColours') < page_enum.index('CustomisationFogVisibility')
assert 'raw <= static_cast<int>(Page::ConfirmDeleteCustomisationProfile); ++raw)' in menu

expected_rows = {
    'CustomisationSky': ['Sky Colours', 'Sky Visibility', 'Fog Colours', 'Fog Visibility',
                         'Restore default sky and atmosphere'],
    'CustomisationSkyColours': ['Sky zenith:', 'Sky horizon:', 'Sky sun:', 'Cloud colour:'],
    'CustomisationSkyVisibility': ['Moon/Sun visible:', 'Clouds visible:', 'Stars visible:'],
    'CustomisationFogColours': ['Fog haze:', 'Directional fog:'],
    'CustomisationFogVisibility': ['Fog visible:', 'Directional fog visible:'],
}
for page in pages:
    render = page_cases(build, page)
    action = page_cases(actions, page)
    assert 'L"Back"' in render, page
    assert 'else GoBack();' in action, page
    assert f'case Page::{page}:' in crumbs
    for label in expected_rows[page]:
        assert label in render, (page, label)

parent = page_cases(actions, 'CustomisationSky')
for row, target in enumerate(pages[1:]):
    assert f'i == {row}) SetPage(Page::{target});' in parent, target
assert 'i == 4)' in parent
for fn in ('ResetSkyDiscVisibility();', 'ResetSkyEffectVisibility();',
           'ResetSkyColours();', 'ResetFogColours();', 'ResetFogVisibility();'):
    assert fn in parent, fn

# Exactly 17 original controls, each in its expected page and no duplicates.
colours = page_cases(actions, 'CustomisationSkyColours')
sky_visible = page_cases(actions, 'CustomisationSkyVisibility')
fog_colours = page_cases(actions, 'CustomisationFogColours')
fog_visible = page_cases(actions, 'CustomisationFogVisibility')
for page_action, ordered in [
    (colours, ['SetSkyColourEnabled(SkyRegion::Zenith', 'OpenSkyColourMethod(SkyRegion::Zenith)',
               'SetSkyColourEnabled(SkyRegion::Horizon', 'OpenSkyColourMethod(SkyRegion::Horizon)',
               'SetSkyColourEnabled(SkyRegion::Sun', 'OpenSkyColourMethod(SkyRegion::Sun)',
               'SetSkyColourEnabled(SkyRegion::Clouds', 'OpenSkyColourMethod(SkyRegion::Clouds)']),
    (sky_visible, ['SetSkyDiscVisible(', 'SetSkyEffectVisible(SkyEffect::Clouds,',
                   'SetSkyEffectVisible(SkyEffect::Stars,']),
    (fog_colours, ['SetFogColourEnabled(FogRegion::General', 'OpenFogColourMethod(FogRegion::General)',
                   'SetFogColourEnabled(FogRegion::Directional', 'OpenFogColourMethod(FogRegion::Directional)']),
    (fog_visible, ['SetFogVisible(FogRegion::General', 'SetFogVisible(FogRegion::Directional']),
]:
    indices = [int(i) for i in re.findall(r'(?:if|else if) \(i == (\d+)\)', page_action)]
    assert indices == list(range(len(ordered))), indices
    offsets = [page_action.index(needle) for needle in ordered]
    assert offsets == sorted(offsets), ordered

# All four new pages are reversible via Back to Sky.
assert 'case Page::CustomisationSkyColours: case Page::CustomisationSkyVisibility:' in back
assert 'case Page::CustomisationFogColours: case Page::CustomisationFogVisibility:' in back
assert 'SetPage(Page::CustomisationSky); break;' in back

# Left/right switching follows the same page+row contract as Enter.
for page, count in [('CustomisationSkyColours', 8), ('CustomisationSkyVisibility', 3),
                    ('CustomisationFogColours', 4), ('CustomisationFogVisibility', 2)]:
    for i in range(count):
        assert f'g_page == Page::{page} && g_selection == {i})' in arrows, (page, i)

print('PASS: Sky/Fog page organisation, row actions, arrows, back, search and reset')

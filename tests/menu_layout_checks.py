"""Static menu-page coverage and remapped navigation/action regression checks.

These checks verify source plumbing only. Actual UI interaction needs Windows
x86 gameplay testing.
"""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
menu = (root / 'src/menu.cpp').read_text(encoding='utf-8')
hud = (root / 'src/hud.cpp').read_text(encoding='utf-8')
header = (root / 'include/refunct/state.hpp').read_text(encoding='utf-8')
api = (root / 'include/refunct/features/menu.hpp').read_text(encoding='utf-8')

pages_text = header.split('enum class Page {', 1)[1].split('};', 1)[0]
pages_text = re.sub(r'//[^\n]*', '', pages_text)
pages = [p.strip() for p in pages_text.split(',') if p.strip()]

render = menu.split('std::vector<std::wstring> BuildEntries() {', 1)[1].split('// Show the current location', 1)[0]
breadcrumbs = menu.split('const wchar_t* MenuBreadcrumb(Page page)', 1)[1].split('std::vector<std::wstring> Entries()', 1)[0]
back = menu.split('void GoBack() {', 1)[1].split('void ToggleProgressRiseAnimation()', 1)[0]
activate = menu.split('void ActivateSelected(MyCharacter* player) {', 1)[1].split('namespace {\n// Left/Right settings', 1)[0]
arrow = menu.split('void HandleHorizontalMovement(int key, MyCharacter* player) {', 1)[1].split('} // namespace\n\nvoid ProcessKey(int key, MyCharacter* player) {', 1)[0]

for page in pages:
    needle = f'case Page::{page}:'
    assert render.count(needle) == 1, f'{page}: missing/duplicate BuildEntries case'
    assert breadcrumbs.count(needle) == 1, f'{page}: missing/duplicate breadcrumb'
    assert back.count(needle) == 1, f'{page}: missing/duplicate GoBack case'
    assert activate.count(needle) == 1, f'{page}: missing/duplicate action case'

assert 'const wchar_t* MenuBreadcrumb(Page page);' in api
assert 'std::wstring(MenuBreadcrumb(g_page))' in hud
assert 'g_page != Page::Main' in menu and 'Hide menu  [' in menu

# Helper for the final action switch only.
def selected(page):
    start = activate.index(f'case Page::{page}:')
    end = activate.find('        case Page::', start + 15)
    return activate[start:end if end != -1 else None]

for page, targets in {
    'Main': ['Page::Movement', 'Page::World', 'Page::Maps', 'Page::Customisation', 'Page::Hud', 'Page::Splits', 'Page::HudKeyDisplay', 'Page::Ghost', 'Page::GhostReplay', 'Page::ProfilesHub', 'Page::Settings'],
    'SaveStates': ['Page::RestartOptions', 'Page::SavePositions', 'Page::SavedCheckpoints',
                   'Page::ButtonCheckpoints', 'Page::Practice'],
    'World': ['Page::WorldSections', 'Page::SaveStates', 'Page::WorldButtonActions',
              'Page::WorldTileActions', 'Page::WorldCubes', 'Page::WorldTubeSpeed', 'Page::WorldPlatformSpeed',
              'Page::WorldJumpPadHeight'],
    'Customisation': ['Page::WorldButtonColours', 'Page::WorldButtonBeacons',
                      'Page::WorldTileColours', 'Page::WorldWalls', 'Page::WorldTubeColours',
                      'Page::WorldPlatformColours', 'Page::WorldJumpPadColours',
                      'Page::WorldWaterColours', 'Page::WorldPlayer'],
    'Hud': ['Page::HudGameplay', 'Page::HudAppearance'],
    'Splits': ['Page::SplitHud', 'Page::HudTiming', 'Page::HudRecordTimes', 'Page::SplitHudAppearance'],
    'HudTiming': ['Page::SplitDisplay', 'Page::HudComparisons'],
    'Settings': ['Page::Hotkeys', 'Page::Persistence', 'Page::Logs'],
}.items():
    snippet = selected(page)
    for i, target in enumerate(targets):
        if page == 'HudTiming':
            i += 1  # Split popup visibility is the first row.
        statement = f'SetPage({target});'
        assert ((page == 'Main' and target == 'Page::Maps' and 'SetPage(Page::Maps);' in snippet) or
                f'if (i == {i}) {statement}' in snippet or
                f'else if (i == {i}) {statement}' in snippet or
                f'else if (i == {i}) {{ {statement} break; }}' in snippet or
                (page == 'Customisation' and target == 'Page::WorldWaterColours' and
                 'else if (i == 7) {' in snippet and
                 's_water_colour_return_page = Page::Customisation;' in snippet and
                 'SetPage(Page::WorldWaterColours);' in snippet) or
                (page == 'Customisation' and target == 'Page::WorldPlayer' and
                 'else if (i == 12) SetPage(Page::WorldPlayer);' in snippet) or
                (page == 'Main' and target == 'Page::ProfilesHub' and
                 'SetPage(Page::ProfilesHub);' in snippet)), (page, i, target)

# New pages must keep their corresponding native actions and arrow controls.
for page, statements in {
    'WorldButtonActions': ['RestoreDefaultButtons()', 'RestoreButtons(false)', 'RestoreButtons(true)',
                           'DisableUnderwaterButtons()', 'DisableAboveWaterButtons()',
                           'DisableAllButtons()', 'PressAllAvailableButtons()', 'RepairGhostButtonsNow()'],
    'WorldTileActions': ['SelectWorldTiles(false)', 'SelectWorldTiles(true)',
                         'DeselectWorldTiles(false)', 'DeselectWorldTiles(true)',
                         'ToggleTileSelectionOnContact()'],
    'WorldTileColours': ['CycleTileColourPreset(true, +1)', 'OpenSelectedTileColourMethod(true)',
                         'ToggleRandomTileRGB()', 'CycleTileColourPreset(false, +1)',
                         'OpenSelectedTileColourMethod(false)'],
    'SplitDisplay': ['CycleComparePopupDisplayMode(+1)', 'ChangeSplitRowMode(g_pb_row_mode, +1)',
                     'ChangeSectionCompareMode(g_section_vs_pb_display, +1)',
                     'ChangeSectionCompareMode(g_section_vs_file_display, +1)'],
    'HudComparisons': ['CycleCompareMode()',
                       'CycleComparisonProfile(+1)', 'ReloadComparisonProfile()'],
    'Persistence': ['ToggleMovementPersistence(true)', 'ToggleMovementPersistence(false)', 'ToggleWorldPersistence(i == 2)'],
    'GameplayRecords': ['ToggleRecordSaving(true)', 'ToggleRecordSaving(false)',
                         'CycleCheatRecordMode(+1)'],
    'Ghost': ['SetPBGhostEnabled(!IsPBGhostEnabled())', 'SetPBGhostFastestSectionsEnabled(!IsPBGhostFastestSectionsEnabled())', 'else GoBack();'],
}.items():
    snippet = selected(page)
    assert [s in snippet for s in statements] == [True] * len(statements), page

for page in ['WorldButtonActions', 'WorldTileActions', 'WorldTileColours',
             'HudComparisons', 'Persistence', 'Ghost']:
    assert f'g_page == Page::{page}' in arrow, f'{page}: missing arrow navigation'

assert 'g_page == Page::MovementPhysics && g_selection == 8' in arrow, 'Game speed arrow mapping'
assert 'case Page::RestartOptions:\n            SetPage(Page::SaveStates); break;' in back
assert 'case Page::SaveStates: SetPage(Page::World); break;' in back
print(f'PASS: {len(pages)} menu pages, navigation, action wiring and breadcrumbs')

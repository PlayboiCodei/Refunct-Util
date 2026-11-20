"""HUD layout and navigation regression tests for consolidated side elements.

Source-level checks only: validate that every per-element setting is reachable,
there are no duplicate live HUD toggles, and the existing split popup remains.
"""
from pathlib import Path
import re

root=Path(__file__).resolve().parents[1]
menu=(root/'src/menu.cpp').read_text('utf-8')
state=(root/'include/refunct/state.hpp').read_text('utf-8')
storage=(root/'src/storage.cpp').read_text('utf-8')


def area(start,end):
    return menu.split(start,1)[1].split(end,1)[0]


def page(body,name):
    match=re.search(r'^\s{8}case Page::'+re.escape(name)+r':(?:(?!^\s{8}case Page::).)*',body,re.M|re.S)
    assert match,name
    return match.group(0)

entries=area('std::vector<std::wstring> BuildEntries() {','// Show the current location')
actions=area('void ActivateSelected(MyCharacter* player) {','void ProcessKey(int key, MyCharacter* player) {')
arrows=menu.split('void HandleHorizontalMovement(int key, MyCharacter* player) {', 1)[1].split('} // namespace\n\nvoid ProcessKey(int key, MyCharacter* player) {', 1)[0]
reset=area('void RestoreHudDisplayDefaults() {','void GoBack() {')

home=page(entries,'Hud')
side=page(entries,'SplitHud')
keys_page=page(entries,'HudKeyDisplay')
custom=page(entries,'SplitHudAppearance')
edit=page(entries,'HudAppearanceEdit')
shift=page(entries,'HudTiming')
popup=page(entries,'SplitDisplay')
assert 'L"Side HUD Elements  >"' in home
assert 'L"Split Popups & Comparisons  >"' not in home
assert 'L"Split Popups & Comparisons  >"' in page(entries,'Splits')
assert 'L"Key Display  >"' not in home
assert 'L"Inputs  >"' in page(entries,'Main')
assert 'L"Customisations  >"' in home
assert 'L"HUD Positions  >"' not in home
assert 'L"HUD Colours & Sizes  >"' not in home
assert 'L"Saved Time Displays  >"' not in shift
assert 'L"Show button split popups: "' in shift
assert 'L"Configure Popup Rows  >"' in shift
assert 'L"Compare Profile & Mode  >"' in shift
assert 'std::wstring(kHudPositionLabels[index]) + L": "' in side
assert 'kGeneralHudElements' in side and 'KeyDisplay' not in side
assert 'Show key display: ' in keys_page
assert 'Customise key display  >' in keys_page
assert 'HudElementIsVisible(element) ? L"ON" : L"OFF"' in side
assert 'entries.reserve(count + 1)' in side
assert 'entries.reserve(count + (splits ? 1 : 2))' in custom
assert 'F5 utility menu position: ' in custom
for part in ('L"Position: "', 'L"Colour: "', 'HudCustomColourMethodLabel()',
             'L"Text size: "', 'L"Show header: "', 'HudElementHasView(element)',
             'L"Reset this element customisations"'):
    assert part in edit,part

home_action=page(actions,'Hud')
assert 'if (i == 0) SetPage(Page::HudGameplay)' in home_action
assert 'else if (i == 1) SetPage(Page::HudTiming)' in page(actions,'Splits')
assert 'SetPage(Page::HudKeyDisplay)' not in home_action
assert 'else if (i == 6) SetPage(Page::HudKeyDisplay)' in page(actions,'Main')
assert 'else if (i == 1) SetPage(Page::HudAppearance)' in home_action
assert 'else if (i == 3) RestoreHudDisplayDefaults()' in home_action
assert 'ToggleHudElement(splits ? kSplitHudElements[i] : kGeneralHudElements[i]);' in page(actions,'SplitHud')
assert 'ToggleHudElement(HudElement::KeyDisplay);' in page(actions,'HudKeyDisplay')
assert 's_hud_style_return_page = Page::HudKeyDisplay;' in page(actions,'HudKeyDisplay')
assert 's_hud_style_return_page = g_page;' in page(actions,'SplitHudAppearance')
assert 'CycleMenuPosition(+1)' in page(actions,'SplitHudAppearance')
for text in ('CycleHudCorner(s_hud_style_element, +1)', 'CycleHudColour(+1)',
             'OpenHudCustomColourMethod()', 'AdjustHudTextSize(+1)',
             'ToggleHudHeader()', 'ToggleHudElementView(element)',
             'ResetHudElementStyle()'):
    assert text in page(actions,'HudAppearanceEdit'),text
for row in range(5):
    assert f'g_page == Page::HudAppearanceEdit && g_selection == {row}' in arrows
assert 'case Page::HudAppearance: {' in arrows
assert 'HudElementHasView(static_cast<HudElement>(s_hud_style_element))' in arrows
assert 'case Page::SplitHud: {' in arrows
assert 'g_page == Page::HudGameplay && g_selection >= 0' not in arrows
assert 'g_page == Page::HudKeyDisplay && g_selection == 0' in arrows
assert 'L"Total vs other profile: "' in popup
assert 'g_pb_gain_loss_display' in popup
assert 'g_section_vs_pb_display' in popup

# All 22 distinct saved slots and all display flags still exist.
keys=re.search(r'kHudPositionKeys\{\{(.*?)\}\};',state,re.S).group(1)
assert len(re.findall(r'L"',keys))==22
for name in ('Timer','PBRun','SumOfBest','LastButtonSplit','Coordinates','Cubes',
             'Tiles','FPS','EnabledModules','PracticeStatus','RecordStatus','Speed',
             'CompareTime','BestSegmentTime','PBSplitTime','CompletionPercent',
             'ActiveCheats','ActiveCustomisations','GameTime','LocalTime','ButtonNumber','KeyDisplay'):
    assert f'case HudElement::{name}:' in menu,name
assert 'kDefaultHudShowHeaders' in state
assert 'ShowHeader' in storage
assert 'ShowPracticeStatus' in storage and 'ShowRecordStatus' in storage
assert 'g_hud_show_headers = kDefaultHudShowHeaders' in reset
assert 'g_hud_corners = kDefaultHudCorners' in reset
assert 'g_show_timer = true' in reset
assert 'g_show_splits = true' in reset
assert 'g_show_button_number = false' in reset
assert 'g_show_key_display = false' in reset
assert 'g_show_record_status_hud = true' in reset
assert 'SaveSettings()' in reset
print('PASS: general and split HUD controls separated; 22 settings and editor persistence preserved')

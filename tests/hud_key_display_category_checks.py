"""Main-menu Inputs module: visibility, customisation, Back paths."""
from pathlib import Path
import re
root = Path(__file__).resolve().parents[1]
m = (root/'src/menu.cpp').read_text(encoding='utf-8')
h = (root/'include/refunct/state.hpp').read_text(encoding='utf-8')
def in_block(begin, end):
    return m.split(begin, 1)[1].split(end, 1)[0]
entries = in_block('std::vector<std::wstring> BuildEntries() {', '// Show the current location')
actions = in_block('void ActivateSelected(MyCharacter* player) {', 'void ProcessKey(int key, MyCharacter* player) {')
descriptions = in_block('std::wstring MenuDescription(Page page, int selected)', 'void RestoreHudDisplayDefaults() {') if 'std::wstring MenuDescription(Page page, int selected)' in m else m
assert 'HudAppearance, HudAppearanceEdit, HudKeyDisplay' in h
assert 'L"Inputs  >"' in entries
assert 'L"Key Display  >"' not in entries
assert 'case Page::HudKeyDisplay:' in entries
assert 'L"Customise key display  >"' in entries
side = entries.split('case Page::HudGameplay:',1)[1].split('case Page::HudKeyDisplay:',1)[0]
assert 'kGeneralHudElements' in side
assert 'KeyDisplay' not in side
assert 'kSideHudElementCount = static_cast<size_t>(HudElement::KeyDisplay)' in m
hud = actions.split('case Page::Hud:',1)[1].split('case Page::HudTiming:',1)[0]
main = actions.split('case Page::Main:',1)[1].split('case Page::Maps:',1)[0]
assert 'else if (i == 6) SetPage(Page::HudKeyDisplay)' in main
assert 'SetPage(Page::HudKeyDisplay)' not in hud
assert 'else if (i == 1) SetPage(Page::HudAppearance)' in hud
key = actions.split('case Page::HudKeyDisplay:',1)[1].split('case Page::HudAppearance:',1)[0]
assert 'if (i == 0) ToggleHudElement(HudElement::KeyDisplay)' in key
assert 's_hud_style_element = static_cast<size_t>(HudElement::KeyDisplay)' in key
assert 's_hud_style_return_page = Page::HudKeyDisplay' in key
assert 'SetPage(Page::HudAppearanceEdit)' in key
back = in_block('void GoBack() {', 'void ToggleProgressRiseAnimation()')
assert 'case Page::HudKeyDisplay:' in back
assert 'SetPage(s_hud_style_return_page)' in back
assert 'case Page::HudKeyDisplay: return L"Inputs";' in m
assert 'L"Inputs / Customisations"' in m
assert 'case Page::HudKeyDisplay:' in descriptions
assert 'g_page == Page::HudKeyDisplay && g_selection == 0' in m
assert 'case Page::Maps: case Page::Hud: case Page::Splits: case Page::HudKeyDisplay:' in back
print('PASS: Inputs main module, no duplicate HUD link, preserved editor and correct Back navigation')

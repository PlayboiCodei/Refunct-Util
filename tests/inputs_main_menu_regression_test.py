"""Check Inputs main-menu routing while preserving saved WASD HUD behaviour.

This validates source navigation and settings wiring. It is not an in-game test.
"""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
menu = (root / 'src/menu.cpp').read_text(encoding='utf-8')
state = (root / 'include/refunct/state.hpp').read_text(encoding='utf-8')
hud = (root / 'src/hud.cpp').read_text(encoding='utf-8')
storage = (root / 'src/storage.cpp').read_text(encoding='utf-8')


def case(section: str, page: str) -> str:
    result = re.search(r'^\s{8}case Page::' + re.escape(page) +
                       r':(?:(?!^\s{8}case Page::).)*', section, re.M | re.S)
    assert result, f'Missing page: {page}'
    return result.group(0)


entries = menu.split('std::vector<std::wstring> BuildEntriesForPage(Page page) {', 1)[1].split('void RefreshMenuSearch()', 1)[0]
actions = menu.split('void ActivateSelected(MyCharacter* player) {', 1)[1].split('namespace {\n// Left/Right settings', 1)[0]
back = menu.split('void GoBack() {', 1)[1].split('void ToggleProgressRiseAnimation()', 1)[0]
breadcrumb = menu.split('const wchar_t* MenuBreadcrumb(Page page)', 1)[1].split('std::vector<std::wstring> Entries()', 1)[0]

main_rows = re.findall(r'L"([^"\n]*\s+>)"', case(entries, 'Main'))
assert main_rows == [
    'Movement  >', 'World  >', 'Maps  >', 'Customisation  >',
    'HUD  >', 'Splits  >', 'Inputs  >', 'Ghost  >',
    'Replay  >', 'Profiles  >', 'Settings  >', 'Search settings  [/]  >'
]
main_actions = case(actions, 'Main')
assert 'else if (i == 6) SetPage(Page::HudKeyDisplay);' in main_actions
assert 'else if (i == 7) SetPage(Page::Ghost);' in main_actions
assert 'else if (i == 9) SetPage(Page::ProfilesHub);' in main_actions
assert 'else if (i == 10) SetPage(Page::Settings);' in main_actions
assert 'else if (i == 11) OpenMenuSearch();' in main_actions

hud_rows = case(entries, 'Hud')
hud_actions = case(actions, 'Hud')
assert 'Key Display  >' not in hud_rows
assert 'HudKeyDisplay' not in hud_actions
assert 'else if (i == 1) SetPage(Page::HudAppearance);' in hud_actions
assert 'else if (i == 2) {' in hud_actions
assert 'else if (i == 3) RestoreHudDisplayDefaults();' in hud_actions

inputs_rows = case(entries, 'HudKeyDisplay')
inputs_actions = case(actions, 'HudKeyDisplay')
assert 'Show key display:' in inputs_rows
assert 'Customise key display  >' in inputs_rows
assert 'ToggleHudElement(HudElement::KeyDisplay);' in inputs_actions
assert 's_hud_style_element = static_cast<size_t>(HudElement::KeyDisplay);' in inputs_actions
assert 's_hud_style_return_page = Page::HudKeyDisplay;' in inputs_actions
assert 'SetPage(Page::HudAppearanceEdit);' in inputs_actions
assert 'case Page::HudKeyDisplay: return L"Inputs";' in breadcrumb
assert 'L"Inputs / Customisations"' in breadcrumb
assert 'case Page::HudKeyDisplay:' in back.split('SetPage(Page::Main);', 1)[0]
assert 'SetPage(s_hud_style_return_page); break;' in back

# This is a relocation, not a new saved option/visibility state or hotkey.
assert 'HudAppearance, HudAppearanceEdit, HudKeyDisplay, Search' in state
assert 'ToggleKeyDisplayHUD' in state
assert 'KeyDisplayPosition' in state
assert 'ShowKeyDisplay' in storage
assert 'g_show_key_display' in hud
assert 'ToggleHudElement(HudElement::KeyDisplay)' in menu
print('PASS: Inputs module reachable with preserved hotkey, HUD drawing, style and settings')

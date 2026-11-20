"""Regression for promoting Profiles to main menu without changing saved data.

Checks the dynamic profile-list rows, Back navigation and Record Saving
selection. Source-level checks are not a substitute for Windows in-game UI.
"""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
menu = (root / 'src/menu.cpp').read_text(encoding='utf-8')
state = (root / 'include/refunct/state.hpp').read_text(encoding='utf-8')

def between(text: str, start: str, end: str) -> str:
    assert start in text and end in text, (start, end)
    return text.split(start, 1)[1].split(end, 1)[0]

render = between(menu, 'std::vector<std::wstring> BuildEntriesForPage(Page page) {', 'void RefreshMenuSearch()')
menu_actions = between(menu, 'void ActivateSelected(MyCharacter* player) {', 'namespace {\n// Left/Right settings')
back = between(menu, 'void GoBack() {', 'void ToggleProgressRiseAnimation()')
descriptions = between(menu, 'std::wstring MenuSelectionDescription(', '// HUD reset remains display-only')
main = between(render, 'case Page::Main:', 'case Page::RestartOptions:')
profiles = between(render, 'case Page::Profiles:', 'case Page::Settings:')
settings = between(render, 'case Page::Settings:', 'case Page::Hotkeys:')
profile_actions = between(menu_actions, 'case Page::Profiles:', 'case Page::Settings:')
settings_actions = between(menu_actions, 'case Page::Settings:', 'case Page::Hotkeys:')
main_actions = between(menu_actions, 'case Page::Main:', 'case Page::Maps:')

assert main.count('L"Profiles  >"') == 1
assert main.index('L"Ghost  >"') < main.index('L"Profiles  >"') < main.index('L"Settings  >"')
assert 'else if (i == 9) SetPage(Page::ProfilesHub);' in main_actions
assert 'else if (i == 10) SetPage(Page::Settings);' in main_actions
assert 'else if (i == 11) OpenMenuSearch();' in main_actions

assert 'for (const auto& name : g_profiles)' in profiles
assert '(name == g_active_profile ? L"[ACTIVE] "' in profiles
for label in ('Create new profile...', 'Rename active profile...', 'Refresh files from disk',
              'Reload active split file', 'Record Saving  >', 'Back'):
    assert profiles.count(f'L"{label}"') == 1, label
assert profiles.index('L"Record Saving  >"') < profiles.index('L"Back"')
assert 'i == static_cast<int>(g_profiles.size()) + 4' in profile_actions
assert 'SetPage(Page::GameplayRecords);' in profile_actions
assert 'SelectProfile(chosen, true)' in profile_actions
assert 'LoadPersonalBest();' in profile_actions

assert 'Record Saving' not in settings and 'Split Profiles' not in settings
assert 'SetPage(Page::GameplayRecords);' not in settings_actions
assert 'SetPage(Page::Profiles);' not in settings_actions
assert 'case Page::Profiles:\n            SetPage(Page::ProfilesHub); break;' in back
assert 'case Page::GameplayRecords: SetPage(Page::Profiles); break;' in back
assert 'case Page::GameplayRecords: return L"Profiles / Replay & Splits / Record Saving";' in menu
assert 'case Page::Profiles: return L"Profiles / Replay & Splits";' in menu
assert 'if (selected == static_cast<int>(count) + 4)' in descriptions

# Existing page IDs and record-saving state/data paths are preserved.
assert 'Ghost, GhostReplay,' in state  # category pages appended without changing older IDs
assert 'case Page::GameplayRecords:' in menu_actions
assert 'CycleCheatRecordMode(' in menu
print('PASS: Profiles main-menu routing, record-saving submenu, descriptions and Back navigation')

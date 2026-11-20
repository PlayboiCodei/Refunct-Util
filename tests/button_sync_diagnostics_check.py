"""Static integration guard for the single button-sync mode."""
from pathlib import Path
r = Path(__file__).resolve().parents[1]
paths = (
    'include/refunct/state.hpp', 'include/refunct/features/practice.hpp',
    'src/state.cpp', 'src/sections.cpp', 'src/storage.cpp',
    'src/menu.cpp', 'src/hud.cpp', 'src/records.cpp',
)
text = {p: (r / p).read_text(encoding='utf-8') for p in paths}
merged = '\n'.join(text.values())
sections = text['src/sections.cpp']
menu = text['src/menu.cpp']
assert menu.count('SectionButtonSyncStatus()') == 1
assert sections.count('Buttons sync with sections:') == 3
assert menu.count('SetButtonsSyncWithSections(!g_buttons_sync_with_sections)') == 2
assert 'SetButtonsSyncWithSections(bool enabled)' in sections
assert 'SetButtonsSyncWithSections(bool enabled)' in text['include/refunct/features/practice.hpp']
assert 'IsSectionPhysicallyRaised(matched->section, allowed)' in sections
assert 'SyncSectionButtons(true)' in sections
assert '!g_buttons_sync_with_sections || IsSyntheticPracticeButtonContact()' in sections
assert 'g_original_add_based_character(actor, player)' in sections
assert 'g_recording_tainted = true;' in sections
assert 'ButtonsSyncWithSections' in text['src/storage.cpp']
assert 'g_buttons_sync_with_sections' in text['src/records.cpp']
assert 'const auto cheats = ActiveCheatLabels();' in text['src/hud.cpp']
assert 'set(CheatBit::ButtonSync, g_buttons_sync_with_sections);' in text['src/records.cpp']
for legacy in ('g_section_buttons_follow_height', 'g_buttons_match_raised_platforms',
               'SetSectionButtonsFollowHeight', 'SetButtonsMatchRaisedPlatforms'):
    assert legacy not in merged, f'old duplicate mode remains: {legacy}'
assert 'g_add_based_character(actor, player)' not in sections, 'do not fake native presses'
print('PASS: only one button-sync setting; safe native progress gating')

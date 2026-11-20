"""Regression: World Buttons uses the single native physical-height button mode.

This replaces the obsolete pre-consolidation dual-toggle checks.
"""
from pathlib import Path
root = Path(__file__).resolve().parents[1]
sections = (root / 'src/sections.cpp').read_text(encoding='utf-8')
menu = (root / 'src/menu.cpp').read_text(encoding='utf-8')
state = (root / 'include/refunct/state.hpp').read_text(encoding='utf-8')
assert 'SectionButtonContactHook' in sections
assert 'IsSectionPhysicallyRaised' in sections
assert 'g_section_button_contact_hook_available' in sections
assert 'IsSyntheticPracticeButtonContact()' in sections
assert 'g_original_add_based_character(actor, player)' in sections
assert 'SetButtonsSyncWithSections(bool enabled)' in sections
assert 'g_buttons_sync_with_sections' in state
assert 'SectionButtonSyncStatus()' in menu
assert 'SetButtonsSyncWithSections(!g_buttons_sync_with_sections)' in menu
assert 'g_buttons_match_raised_platforms' not in state
assert 'ChangeSectionButtonRules(bool follow, bool physical)' not in sections
print('PASS: single native button height-sync module remains intact')

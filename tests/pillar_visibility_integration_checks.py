"""Remove Pillars replaces Pillars Visible and is classified as cosmetic.

Tests menu routing, reset ownership, persistence migration, record mask, and
World-only scan gating. No native Refunct process is launched.
"""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
source = (root / 'src/pillars.cpp').read_text(encoding='utf-8')
menu = (root / 'src/menu.cpp').read_text(encoding='utf-8')
records = (root / 'src/records.cpp').read_text(encoding='utf-8')
sections = (root / 'src/sections.cpp').read_text(encoding='utf-8')
api = (root / 'include/refunct/features/pillars.hpp').read_text(encoding='utf-8')

world_rows = menu.split('case Page::World:\n            return {', 1)[1].split('};', 1)[0]
custom_rows = menu.split('case Page::CustomisationPillars:\n            return {', 1)[1].split('};', 1)[0]
actions = menu.split('void ActivateSelected(MyCharacter* player) {', 1)[1]
world_actions = actions.split('case Page::World:', 1)[1].split('case Page::WorldCubes:', 1)[0]
pillar_actions = actions.split('case Page::CustomisationPillars:', 1)[1].split('case Page::CustomisationTime:', 1)[0]
world_reset = menu.split('void ResetWorldGameplayOptions() {', 1)[1].split('void ResetWorldCustomisation() {', 1)[0]
custom_reset = menu.split('void ResetWorldCustomisation() {', 1)[1].split('// Page through large map folders', 1)[0]
arrows = menu.split('void HandleHorizontalMovement(int key, MyCharacter* player) {', 1)[1].split('} // namespace\n\nvoid ProcessKey(int key, MyCharacter* player) {', 1)[0]
mask = records.split('constexpr uint64_t kCustomisationMask =', 1)[1].split(';', 1)[0]
settings = records.split('uint64_t AllSettingMask() {', 1)[1].split('uint64_t SettingCheatMask()', 1)[0]
labels = records.split('std::vector<std::wstring> LabelsForMask(uint64_t active)', 1)[1].split('} // namespace', 1)[0]

assert 'L"Pillars  >"' in menu
assert 'L"Customisation / Pillars"' in menu
assert 'L"Remove pillars: "' not in world_rows
assert 'L"Pillars visible: "' not in world_rows
assert 'L"Remove pillars: "' in custom_rows
assert 'L"Pillars visible: "' not in custom_rows
assert 'else if (i == 12) ResetWorldGameplayOptions();' in world_actions
assert 'SetPillarsRemoved(' not in world_actions
assert 'else if (i == 2) SetPillarsRemoved(!ArePillarsRemoved());' in pillar_actions
assert 'SetPillarsVisible(' not in pillar_actions
assert 'g_page == Page::CustomisationPillars && g_selection == 2' in arrows
assert 'g_page == Page::World && g_selection == 9' in arrows  # Remove All Objects toggle
assert 'SetPillarsRemoved(' not in world_reset
assert 'if (ArePillarsRemoved()) SetPillarsRemoved(false);' in custom_reset
assert 'SetPillarsVisible(' not in custom_reset

assert 'bool ArePillarsRemoved();' in api
assert 'void SetPillarsRemoved(bool removed);' in api
assert 'L"PillarColours", L"Removed", 0' in source
assert 'QueueSettingsValue(L"PillarColours", L"Removed"' in source
# Previous Visible=0 setting must not survive behind a menu that can no
# longer turn it back on. Legacy code remains for native restoration only.
assert 's_config.visible = true;' in source
assert 's_config.visible = GetPrivateProfileIntW(' not in source
assert 'IsDistributorComponent(target.mesh)' in source
assert 'objects->objects[target.index].serial_number == target.serial' in source
assert 'FindReflectedFunction(component, L"SetStaticMesh")' in source
assert 'FindReflectedFunction(component, L"SetCollisionEnabled")' in source
assert 'if (removed || target.removal_saved || target.mesh_detached ||' in source
assert 'return t.mid != nullptr || t.removal_saved || t.visual_saved;' in source

assert 'set(CheatBit::PillarRemoved, ArePillarsRemoved());' in settings
assert 'set(CheatBit::PillarVisibility, ' not in settings
assert 'CheatMask(CheatBit::PillarRemoved)' in mask
assert 'CheatMask(CheatBit::PillarVisibility)' not in mask
assert 'add(CheatBit::PillarRemoved, L"Pillars removed")' in labels
assert 'add(CheatBit::PillarVisibility, ' not in labels
assert 'uint64_t SettingCheatMask() { return AllSettingMask() & ~kCustomisationMask; }' in records
assert 'uint64_t ActiveCustomisationMask() { return AllSettingMask() & kCustomisationMask; }' in records
assert 'ArePillarsRemoved() ||' not in sections
assert 'g_run_pb_eligible' not in source

print('PASS: Remove Pillars cosmetic menu, legacy setting migration, reset, saved state, HUD and PB mask')

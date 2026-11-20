"""World page organisation and object-removal routing regression checks.

Uses source contracts; this is not an in-game Unreal Engine test.
"""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
menu = (root / "src/menu.cpp").read_text(encoding="utf-8")
state = (root / "include/refunct/state.hpp").read_text(encoding="utf-8")
removal = (root / "src/world_object_removal.cpp").read_text(encoding="utf-8")


def page_body(source: str, section: str, page: str) -> str:
    part = source.split(section, 1)[1]
    return part.split(f"case Page::{page}:", 1)[1].split("case Page::", 1)[0]


build = "std::vector<std::wstring> BuildEntriesForPage(Page page) {"
actions = "void ActivateSelected(MyCharacter* player) {"
arrow = menu.split('void HandleHorizontalMovement(int key, MyCharacter* player) {', 1)[1].split('} // namespace\n\nvoid ProcessKey(int key, MyCharacter* player) {', 1)[0]
world = page_body(menu, build, "World")
world_actions = page_body(menu, actions, "World")

# Main World menu titles must be category names rather than implementation names.
names = ["Buttons", "Tiles", "Cubes", "Tubes", "Platforms", "Jump Pads"]
for category in names:
    assert f'L"{category}  >"' in world, category
for old in ["Button Controls", "Tile Controls", "Cube Controls", "Tube Travel Speed",
            "Platform Speed", "Jump Pad Launch Height", "Remove Objects"]:
    assert f'L"{old}  >"' not in world, old

# Keep global actions at World root; make legacy removal page inaccessible.
assert 'L"Restore all removed objects"' in world
assert 'L"Remove all objects: "' in world
assert 'SetAllWorldObjectsRemoved(!AreAllWorldObjectsRemoved())' in world_actions
assert 'ResetWorldObjectRemovals()' in world_actions
assert 'SetPage(Page::WorldObjectRemoval)' not in world_actions
assert 'page == Page::WorldObjectRemoval) continue;' in menu
for orphan in ("WorldButtons", "WorldTiles", "WorldTubes", "WorldPlatforms", "WorldJumpPads"):
    assert f"page == Page::{orphan}" in menu.split("void RefreshMenuSearch()", 1)[1].split("const auto rows =", 1)[0]

assert 'CustomisationPillars, WorldObjectRemoval' in state  # Never renumber page enum.

categories = {
    # Page, native kind, user text, row index (existing commands unaffected).
    "WorldButtonActions": ("Buttons", "buttons", 9),
    "WorldTileActions": ("Tiles", "tiles", 5),
    "WorldCubes": ("Cubes", "cubes", 5),
    "WorldTubeSpeed": ("Tubes", "tubes", 2),
    "WorldPlatformSpeed": ("Platforms", "moving platforms", 2),
    "WorldJumpPadHeight": ("JumpPads", "jump pads", 2),
}

for page, (kind, label, index) in categories.items():
    rows = page_body(menu, build, page)
    click = page_body(menu, actions, page)
    assert f'L"Remove {label}: "' in rows, page
    assert f'IsWorldObjectRemoved(WorldObjectKind::{kind})' in rows, page
    assert rows.index(f'L"Remove {label}: "') < rows.rindex('L"Back"'), page
    assert f'else if (i == {index})' in click, page
    assert f'SetWorldObjectRemoved(WorldObjectKind::{kind},' in click, page
    assert f'!IsWorldObjectRemoved(WorldObjectKind::{kind})' in click, page
    assert f'g_page == Page::{page} && g_selection == {index}' in arrow, page

for index, page in enumerate([
    "WorldSections", "SaveStates", "WorldButtonActions",
    "WorldTileActions", "WorldCubes", "WorldTubeSpeed", "WorldPlatformSpeed",
    "WorldJumpPadHeight", "WorldWater"
]):
    assert re.search(rf'(?:if|else if) \(i == {index}\) SetPage\(Page::{page}\);', world_actions), page

# Removal is still gameplay-affecting and remains independent of visual customisation.
assert 'QueueSettingsValue(L"WorldObjectRemoval"' in removal
assert 'GetPrivateProfileIntW' in removal
assert 'SetAllWorldObjectsRemoved(false)' in removal
assert 'SetPillarsRemoved(' not in world_actions
assert 'g_page == Page::World && g_selection == 9' in arrow
print("PASS: World category names, six relocated toggles, search, click, arrows, bulk controls")

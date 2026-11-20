"""Regression: popup rows have one obvious visibility control in Popup Layout.

Source-level check only; live Win32 menu rendering still needs in-game testing.
"""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
menu = (root / "src/menu.cpp").read_text(encoding="utf-8")
hud = (root / "src/hud.cpp").read_text(encoding="utf-8")


def part(source: str, start: str, end: str) -> str:
    return source.split(start, 1)[1].split(end, 1)[0]


def case(source: str, name: str) -> str:
    match = re.search(
        r"^        case Page::" + re.escape(name) +
        r":(?:(?!^        case Page::).)*", source, re.M | re.S
    )
    assert match, f"Missing {name}"
    return match.group()


entries = part(menu, "std::vector<std::wstring> BuildEntries() {", "// Show the current location")
actions = part(menu, "void ActivateSelected(MyCharacter* player) {", "void ProcessKey(int key")
arrows = menu.split('void HandleHorizontalMovement(int key, MyCharacter* player) {', 1)[1].split('} // namespace\n\nvoid ProcessKey(int key, MyCharacter* player) {', 1)[0]
popup = case(entries, "SplitDisplay")
references = case(entries, "HudComparisons")
popup_actions = case(actions, "SplitDisplay")
reference_actions = case(actions, "HudComparisons")

# All popup rows are controlled by the same page, including the previously
# hidden section comparison rows. References choose sources, not visibility.
for label in (
    "Show button number:", "Elapsed run time at button:", "Time since previous button:",
    "Total ahead/behind PB:", "Button time vs fastest:",
    "Total vs other profile:", "Section time vs PB:", "Section time vs other profile:",
):
    assert label in popup, label
    assert label not in references, label

for setting, row in (("g_section_vs_pb_display", 6), ("g_section_vs_file_display", 7)):
    assert setting in popup, setting
    assert setting not in references, setting
    assert f"else if (i == {row}) {{ ChangeSectionCompareMode({setting}, +1); break; }}" in popup_actions
    assert f"g_page == Page::SplitDisplay && g_selection == {row}" in arrows
    assert f"ChangeSectionCompareMode({setting}, key == VK_RIGHT ? +1 : -1);" in arrows
    assert setting not in reference_actions
assert "if (i == 0) { CycleCompareMode(); break; }" in reference_actions
assert "else if (i == 1) {" in reference_actions
assert "else if (i == 2) {" in reference_actions

# Hiding split popups must also hide independent section comparison rows.
assert "if (g_show_splits && !g_button_splits.empty() &&" in hud
assert "g_show_splits ||\n         g_section_vs_pb_display" not in hud

# Preserve existing persisted comparison settings (migration-free change).
storage = (root / "src/storage.cpp").read_text(encoding="utf-8")
assert 'L"SectionVsPBDisplay"' in storage
assert 'L"SectionVsFileDisplay"' in storage
print("PASS: all popup rows have visible controls and respect the master switch")

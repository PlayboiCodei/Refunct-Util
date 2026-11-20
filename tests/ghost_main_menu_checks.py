"""PB ghost menu isolation and navigation checks. Native Win32 HUD unverified."""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
menu = (root / "src/menu.cpp").read_text(encoding="utf-8")
state = (root / "include/refunct/state.hpp").read_text(encoding="utf-8")
ghost = (root / "src/ghost.cpp").read_text(encoding="utf-8")

def switch_case(section, page):
    match = re.search(r"case Page::" + page + r":(.*?)(?=\n\s*case Page::|\n\s*default:)", section, re.S)
    assert match, f"Page {page} not found"
    return match.group(1)

render = menu.split("std::vector<std::wstring> BuildEntriesForPage(Page page) {", 1)[1].split("void RefreshMenuSearch()", 1)[0]
activate = menu.split("void ActivateSelected(MyCharacter* player) {", 1)[1].split("namespace {\n// Left/Right settings", 1)[0]
arrow = menu.split('void HandleHorizontalMovement(int key, MyCharacter* player) {', 1)[1].split('} // namespace\n\nvoid ProcessKey(int key, MyCharacter* player) {', 1)[0]
breadcrumbs = menu.split("const wchar_t* MenuBreadcrumb(Page page)", 1)[1].split("std::vector<std::wstring> Entries()", 1)[0]
back = menu.split("void GoBack() {", 1)[1].split("void ToggleProgressRiseAnimation()", 1)[0]

assert "CustomisationShadows,\n    // PB ghost controls" in state
assert "Ghost, GhostReplay," in state
main = switch_case(render, "Main")
assert main.index('L"HUD  >"') < main.index('L"Ghost  >"') < main.index('L"Profiles  >"') < main.index('L"Settings  >"')
assert main.index('L"Settings  >"') < main.index('L"Search settings  [/]  >"')

record_rows = switch_case(render, "GameplayRecords")
ghost_rows = switch_case(render, "Ghost")
assert "Show ghost" not in record_rows and "Ghost recordings" not in record_rows
assert "Show ghost" in ghost_rows
assert "Ghost recordings" not in ghost_rows and "PBGhostRecordingStatus()" not in ghost_rows
assert ghost_rows.count('L"Back"') == 1

main_actions = switch_case(activate, "Main")
assert "else if (i == 7) SetPage(Page::Ghost);" in main_actions
assert "else if (i == 9) SetPage(Page::ProfilesHub);" in main_actions
assert "else if (i == 10) SetPage(Page::Settings);" in main_actions
assert "else if (i == 11) OpenMenuSearch();" in main_actions

record_actions = switch_case(activate, "GameplayRecords")
ghost_actions = switch_case(activate, "Ghost")
assert "SetPBGhostEnabled(" not in record_actions
assert "if (i == 0) SetPBGhostEnabled(!IsPBGhostEnabled());" in ghost_actions
assert "GhostColourMethodLabel()" in ghost_rows
assert "Start next ghost on button" in ghost_rows
assert "Ghost mode:" in ghost_rows
assert "2D transparency:" in ghost_rows and "3D transparency:" in ghost_rows
assert "else if (i == 1) SetPBGhostPassThroughEnabled(!IsPBGhostPassThroughEnabled());" in ghost_actions
assert "else if (i == 2) SetPBGhost2DTransparencyEnabled(!IsPBGhost2DTransparencyEnabled());" in ghost_actions
assert "else if (i == 3) SetPBGhost3DTransparencyEnabled(!IsPBGhost3DTransparencyEnabled());" in ghost_actions
assert "else if (i == 4) OpenGhostColourMethod();" in ghost_actions
assert "else if (i == 5) SetPBGhostButtonSyncEnabled(!IsPBGhostButtonSyncEnabled());" in ghost_actions
assert "Use fastest sections" in ghost_rows
assert "else if (i == 6) SetPBGhostFastestSectionsEnabled(!IsPBGhostFastestSectionsEnabled());" in ghost_actions
assert "else if (i == 6) Notify(HasPBGhostForActiveProfile()" not in ghost_actions
assert "else if (i == 8) SetPage(Page::GhostReplay);" in main_actions
assert "SetPage(Page::GhostReplay)" not in ghost_actions
for index in range(7):
    assert f"case {index}:" in arrow
assert "g_page == Page::Ghost" in arrow
assert "g_selection == 5" in arrow
assert "case Page::Ghost: return L\"Ghost\";" in breadcrumbs
assert "case Page::GhostReplay: return L\"Replay\";" in breadcrumbs
assert "case Page::Maps: case Page::Hud: case Page::Splits: case Page::HudKeyDisplay:" in back
assert "raw <= static_cast<int>(Page::Crosshair); ++raw" in menu
assert "IsPBGhostEnabled()" in ghost and "HasPBGhostForActiveProfile()" in ghost
print("PASS: Ghost main-module routing, status, settings isolation, keyboard navigation, search and back")

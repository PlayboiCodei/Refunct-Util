"""Static integration checks for World -> Tiles (no game EXE required)."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
menu = (root / 'src/menu.cpp').read_text()
tiles = (root / 'src/tiles.cpp').read_text()
sections = (root / 'src/sections.cpp').read_text()
state = (root / 'include/refunct/state.hpp').read_text()
practice = (root / 'include/refunct/features/practice.hpp').read_text()
tick = (root / 'src/game_tick.cpp').read_text()
cmake = (root / 'CMakeLists.txt').read_text()
api = (root / 'include/refunct/internal_api.hpp').read_text()

checks = {
    'world submenu entry': 'L"Tile Controls  >"' in menu,
    'page navigation': 'case Page::WorldTileActions' in menu and 'SetPage(Page::WorldTileActions)' in menu,
    'both labels': 'Select tiles (above water)' in menu and 'Select all tiles (including underwater)' in menu,
    'raised selection action': 'if (i == 0) SelectWorldTiles(false);' in menu,
    'all selection action': 'else if (i == 1) SelectWorldTiles(true);' in menu,
    'back to World': 'case Page::WorldTileActions:' in menu[menu.index('void GoBack()'):menu.index('void ToggleProgressRiseAnimation()')],
    'page enum': 'WorldTiles' in state,
    'native array lookup': 'L"FertileLands"' in tiles and 'L"BP_IslandChunk_C"' in tiles,
    'exact 251 actor preflight': 'enumerated != kTotalColourableTiles' in tiles,
    'raised height used': 'SnapshotRaisedSections(raised)' in tiles and '(all_tiles || raised[section])' in tiles,
    'native activation': 'add_contact(actor, player);' in tiles and 'g_remove_based_character(actor, player);' in tiles and 'g_original_add_based_character' in tiles,
    'native counter untouched': 'const LevelState* level = PracticeLevelState();' in tiles and 'updated->platforms < before.platforms' in tiles and 'state->platforms =' not in tiles.split('bool SelectWorldTiles(bool all_tiles) {', 1)[1],
    'records tainted': 'g_recording_tainted = true;' in tiles,
    'tile actor cache guard': 'g_practice_level_actors[i] != s_tiles.level_roots[i]' in tiles,
    'height snapshot implemented': 'bool SnapshotRaisedSections(' in sections and 'bool SnapshotRaisedSections(' in practice,
    'tick processes selection': 'UpdateWorldTileSelection(player);' in tick,
    'new source in build': 'src/tiles.cpp' in cmake,
    'header aggregator': '#include "refunct/features/tiles.hpp"' in api,
}
for name, ok in checks.items():
    print(f'{"PASS" if ok else "FAIL"}: {name}')
assert all(checks.values()), 'World Tiles integration check failed'
print(f'Passed {len(checks)} static checks')

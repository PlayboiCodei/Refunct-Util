"""World -> Tiles deselection feature integration and failure guards."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
menu = (root / "src/menu.cpp").read_text()
tiles = (root / "src/tiles.cpp").read_text()
header = (root / "include/refunct/features/tiles.hpp").read_text()
cmake = (root / "CMakeLists.txt").read_text()

checks = {
    "available deselect item": 'L"Deselect tiles (above water)"' in menu,
    "all deselect item": 'L"Deselect all tiles (including underwater)"' in menu,
    "available deselect action": 'else if (i == 2) DeselectWorldTiles(false);' in menu,
    "all deselect action": 'else if (i == 3) DeselectWorldTiles(true);' in menu,
    "public declaration": 'bool DeselectWorldTiles(bool all_tiles);' in header,
    "implemented": 'bool DeselectWorldTiles(bool all_tiles) {' in tiles,
    "validates entire map": tiles.count('enumerated != kTotalColourableTiles') >= 2,
    "available uses raised geometry": tiles.count('(all_tiles || raised[section])') >= 2,
    "verified native material API": 'ResolveTileDeselectAbi(candidate)' in tiles and 'SetMeshMaterialVerified(mesh, 0, normal[0])' in tiles and 'SetMeshMaterialVerified(mesh, 1, normal[1])' in tiles,
    "dispatch only game thread": 'UpdateWorldTileSelection(MyCharacter* player)' in tiles and 'UpdateWorldTileSelection(player);' in (root / "src/game_tick.cpp").read_text(),
    "reversal verifies native state": 'ReadTileColor(actor, batch.color_offset, confirmed)' in tiles and 'confirmed != 0' in tiles and 'ReadMeshMaterial(mesh, index) == material' in tiles,
    "validates live progress": 'actual_coloured != level->platforms' in tiles and 'state->platforms = before.platforms - 1;' in tiles,
    "counter updated only after material and native Color": tiles.index('state->platforms = before.platforms - 1;') > tiles.index('confirmed != 0') > tiles.index('SetMeshMaterialVerified(mesh, 0, normal[0])'),
    "restart safety": 'g_full_level_reload_requested || g_restart_requested' in tiles,
    "shared batch prevents overlap": 'if (s_tiles.active)' in tiles,
    "preserves existing select": 'add_contact(actor, player);' in tiles and 'g_remove_based_character(actor, player);' in tiles,
    "build retains tiles source": 'src/tiles.cpp' in cmake,
    "one frame each": 'UpdateWorldTileSelection(MyCharacter* player)' in tiles,
    "write access preflight": 'WritableTileField(&writable_state->platforms' in tiles and 'WritableTileField(static_cast<const uint8_t*>(actor) + batch.color_offset' in tiles,
}
for label, ok in checks.items():
    print(("PASS" if ok else "FAIL") + ": " + label)
assert all(checks.values()), "Tile deselect checks failed"
print(f"Passed {len(checks)} static checks")

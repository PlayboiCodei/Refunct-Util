"""Tile batch height fallback / underwater-agnostic actions."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
src = (root / "src/tiles.cpp").read_text()
helper = src.split("bool SnapshotRaisedTilesForWorldOperation(", 1)[1].split("bool DeselectWorldTiles(bool all_tiles)", 1)[0]
deselect = src.split("bool DeselectWorldTiles(bool all_tiles) {", 1)[1].split("bool IsRandomTileRGBEnabled()", 1)[0]
select = src.split("bool SelectWorldTiles(bool all_tiles) {", 1)[1].split("void UpdateWorldTileSelection(", 1)[0]

checks = {
    "read-only fallback uses native actor position": "FindReflectedFunction(first, L\"K2_GetActorLocation\")" in helper and "g_process_event(actor, get_fn, args.data())" in helper,
    "fallback checks all sections": "for (int section = 0; section < kPracticeClusterCount; ++section)" in helper,
    "fallback checks native actor class": "class_ptr != g_practice_level_class" in helper,
    "fallback requires finite coordinates": "std::isfinite(actual.z)" in helper,
    "fallback supports section height offsets": "GetSectionHeightOffset(section + 1)" in helper,
    "fallback only includes fully raised sections": "std::abs(actual.z - raised_z) <= 8.0f" in helper,
    "fallback keeps failed partial snapshot private": "raised = snapshot;" in helper,
    "deselect all does not require section heights": "if (!all_tiles && !SnapshotRaisedTilesForWorldOperation(raised))" in deselect,
    "select all does not require section heights": "if (!all_tiles && !SnapshotRaisedTilesForWorldOperation(raised))" in select,
    "deselect above water filters by raised": "if (all_tiles || raised[section])" in deselect,
    "select above water filters by raised": "if (all_tiles || raised[section])" in select,
    "native tile / colour / counter preflights preserved": "ResolveTileDeselectAbi(candidate)" in deselect and "actual_coloured != level->platforms" in deselect,
}
for name, ok in checks.items():
    print(("PASS" if ok else "FAIL") + ": " + name)
assert all(checks.values()), "Tile height fallback integration checks failed"
print(f"Passed {len(checks)} height fallback checks")

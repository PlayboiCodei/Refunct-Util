"""World object toggles: menu, serialization, lifecycle, and PB registry contracts."""
from pathlib import Path
root = Path(__file__).resolve().parents[1]
read = lambda n: (root / n).read_text(encoding='utf-8')
module = read('src/world_object_removal.cpp')
header = read('include/refunct/features/world_object_removal.hpp')
menu = read('src/menu.cpp')
state = read('include/refunct/state.hpp')
records = read('src/records.cpp')
sections = read('src/sections.cpp')
tick = read('src/game_tick.cpp')
cmake = read('CMakeLists.txt')
assert 'src/world_object_removal.cpp' in cmake
assert '#include "refunct/features/world_object_removal.hpp"' in read('include/refunct/internal_api.hpp')
assert 'CustomisationPillars, WorldObjectRemoval' in state  # preserve old page IDs
assert 'case Page::WorldObjectRemoval:' in menu
assert 'L"Remove Objects  >"' not in menu
assert 'page == Page::WorldObjectRemoval) continue;' in menu
assert 'else if (i == 9) SetAllWorldObjectsRemoved(!AreAllWorldObjectsRemoved());' in menu
assert 'else if (i == 10) ResetWorldObjectRemovals();' in menu
assert 'else if (i == 12) ResetWorldGameplayOptions();' in menu
for item in ('Tubes','Buttons','JumpPads','Cubes','Platforms','Tiles'):
    assert 'WorldObjectKind::'+item in module or item == 'Tiles'
    assert item in header or item == 'Tiles'
    assert 'WorldObjectKind::'+item in menu
assert 'SetAllWorldObjectsRemoved(!AreAllWorldObjectsRemoved())' in menu
assert 'ResetWorldObjectRemovals();' in menu
assert 'ResetWorldObjectRemovals()' in menu.split('void ResetWorldGameplayOptions() {',1)[1].split('void ResetWorldCustomisation() {',1)[0]
assert 'ResetWorldObjectRemovals()' not in menu.split('void ResetWorldCustomisation() {',1)[1].split('// Page through large',1)[0]
assert 'UpdateCustomMapLoad();' in tick and 'UpdateWorldObjectRemovals(player);' in tick
assert tick.index('UpdateCustomMapLoad();') < tick.index('UpdateWorldObjectRemovals(player);')
assert 'WorldObjectRemoval' in records and 'CheatMask(CheatBit::WorldObjectRemoval)' in records
mask = records.split('constexpr uint64_t kCustomisationMask =',1)[1].split('static_assert(',1)[0]
assert 'CheatBit::WorldObjectRemoval' not in mask
assert 'L"World objects removed"' in records
assert 'if (IsWorldObjectRemoved(WorldObjectKind::Buttons)) return;' in sections
assert 'for (unsigned i = 0; i < static_cast<unsigned>(WorldObjectKind::Count); ++i)' in sections
for native_name in ('BP_TravelPipe_C', 'BP_Button_C', 'BP_Jumppad_C',
                    'BP_PowerCore_C', 'BP_Lift_C', 'BP_Mover_C', 'BP_IslandChunk_C'):
    assert native_name in module
for api in ('SetActorHiddenInGame', 'IsHidden', 'SetActorEnableCollision', 'GetActorEnableCollision'):
    assert api in module
assert 'serial_number == actor.serial' in module
assert 'g_full_level_reload_requested || CustomMapApplying()' in module
assert 'ColourWorkBudget budget(850)' in module
assert 'kRescanMs = 15000' in module
for forbidden in ('DestroyActor', 'DestroyComponent', 'SpawnActor', 'TerminateProcess'):
    assert forbidden not in module
print('PASS: menu, native classes, lifecycle, restart, PB/cheats, and no actor destruction')

"""Guard post-native-tick placement, delayed map/checkpoint loads, and diagnostics."""
from pathlib import Path
root = Path(__file__).resolve().parents[1]
practice = (root / 'src/practice.cpp').read_text(encoding='utf8')
tick = (root / 'src/game_tick.cpp').read_text(encoding='utf8')
map_cpp = (root / 'src/maps.cpp').read_text(encoding='utf8')
map_h = (root / 'include/refunct/features/maps.hpp').read_text(encoding='utf8')
practice_h = (root / 'include/refunct/features/practice.hpp').read_text(encoding='utf8')
assert 'bool CustomMapApplying();' in map_h
assert 'bool CustomMapApplying() { return s_map.applying; }' in map_cpp
assert 'void UpdateGlobalLastButtonTeleport(MyCharacter* player);' in practice_h
assert 'void UpdateGlobalLastButtonTeleport(MyCharacter* player) {' in practice
assert 'CustomMapApplying()' in practice
assert 'g_practice_phase != PracticePhase::Ready' in practice
assert 'Button teleport: REQUEST global button' in practice
assert 'Button teleport: APPLY position-only teleport after native tick' in practice
assert 'Button teleport: COMPLETE button' in practice
assert 'Button teleport: FAILED' in practice
assert 'if (!TeleportToCoordinates(player, request.destination, true))' in practice
assert tick.index('if (g_original_tick) g_original_tick(player, dt);') < tick.index('UpdateGlobalLastButtonTeleport(player);')
assert tick.index('UpdateCustomMapLoad();') < tick.index('UpdateGlobalLastButtonTeleport(player);')
assert tick.index('UpdatePractice(player);') < tick.index('UpdateGlobalLastButtonTeleport(player);')
assert tick.index('UpdateGlobalLastButtonTeleport(player);') < tick.index('UpdateWorldTileSelection(player);')
print('PASS: global teleport deferred until native movement/map/practice finish; logged request, placement and result')

"""Ensure replay-only tile and lift actions force a clean world reset on stop."""
from pathlib import Path
root=Path(__file__).resolve().parents[1]
source=(root/'src/ghost.cpp').read_text(encoding='utf8')
tile=(root/'src/tiles.cpp').read_text(encoding='utf8')
lift=(root/'src/platforms.cpp').read_text(encoding='utf8')
stop=source[source.index('void StopGhostReplayInternal() {'):source.index('// Spawn the camera using',source.index('void StopGhostReplayInternal() {'))]
assert 's_replay.changed_native_progress ||' in stop
assert 'GhostReplayTilesChanged() || GhostReplayLiftsChanged()' in stop
assert stop.index('GhostReplayLiftsChanged()') < stop.index('ResetGhostReplayLifts()')
assert 'if (replay_changed_progress && native &&\n' in stop
assert 'native->buttons > 0' not in stop
assert 'bool GhostReplayTilesChanged()' in tile
assert 'bool GhostReplayLiftsChanged()' in lift
print('PASS: replay-only tile/lift changes trigger cleanup alongside native buttons')

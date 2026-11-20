"""Verify the real pillar scan lifecycle has no periodic rediscovery.

These static integration checks do not run the game; the pillar C++ syntax
is independently checked by pillar_colour_syntax_checks.py.
"""
from pathlib import Path
import re

src = (Path(__file__).resolve().parents[1] / 'src' / 'pillars.cpp').read_text(encoding='utf-8')

def body(begin: str, end: str) -> str:
    return src.split(begin, 1)[1].split(end, 1)[0]

state = body('std::vector<Target> s_targets{};', 'std::atomic<uint32_t>')
assert 'bool s_discovery_complete = false;' in state
assert not re.search(r'kRescanMs|s_next_scan', src)

dirty = body('void Dirty() {', 'DWORD WINAPI PickerThread(')
assert '!s_discovery_complete && s_scan_cursor < 0' in dirty
assert 's_targets.clear()' not in dirty

discover = body('void Discover(const GlobalObjectArray* objects) {', '} // namespace')
assert 's_scan_cursor = -1;' in discover
assert 's_discovery_complete = true;' in discover
assert 'GetTickCount64() + ' not in discover

update = body('void UpdatePillarColours(MyCharacter* player) {', '} // namespace refunct')
assert 'if (s_player != player)' in update
assert 's_targets.clear();' in update  # only when player/world changes
assert 's_discovery_complete = false;' in update
assert 'if (need_discovery && !s_discovery_complete && s_scan_cursor < 0)' in update
assert 'if (!need_discovery) s_scan_cursor = -1;' in update
assert 'if (!pending_restore) return;' in update
assert 'if (!Valid(target, objects))' in update
assert '// The cached UObject was destroyed/replaced.' in update
assert 's_discovery_complete = false;' in update.split('if (!Valid(target, objects))', 1)[1]
assert 's_targets.clear();' not in update.split('const ULONGLONG now = GetTickCount64();', 1)[1]

# While all cosmetics are OFF and restoration is complete, the cache stays
# in memory; a subsequent toggle can re-use it and apply changes immediately.
assert 's_pending = s_targets.size();' in dirty
assert 's_next_update = 0;' in dirty

print('PASS: no periodic pillar scans; one-time cache, settings reuse and invalidation')

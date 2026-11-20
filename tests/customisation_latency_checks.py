"""Source-level invariants for immediate, bounded cosmetic customisation updates.

No source inspection substitutes for Windows x86 compilation or GPU frametime
measurements in Refunct.
"""
from pathlib import Path
root = Path(__file__).resolve().parents[1]
source = {name: (root / 'src' / f'{name}.cpp').read_text(encoding='utf-8')
          for name in ('tubes', 'platforms', 'jumppads', 'sections', 'tiles', 'water')}

# Tube changes update the existing material before optional UObject discovery.
tubes = source['tubes']
hex_set = tubes.split('void SetTubeCustomColourHex(', 1)[1].split('\n}', 1)[0]
assert 'QueueTubeColourRefresh()' in hex_set
assert 's_next_scan = 0' not in hex_set  # no forced global rescan
assert 'if (s_tubes.empty() && s_scan_cursor < 0) s_scan_cursor = 0;' in hex_set
assert 'QueueTubeSave()' in hex_set
assert 'FlushTubeSave();' in tubes.split('void UpdateTubeColours(', 1)[1]
assert 'const size_t batch = s_pending_apply ? 5 : 4;' in tubes
assert 'ColourWorkBudget scan_budget(800)' in tubes
assert 'ColourWorkBudget apply_budget(1000)' in tubes
assert 'TubeMethod(mesh, false)' in tubes and 'TubeMethod(mesh, true)' in tubes
assert 's_pending_apply' in tubes

# Known actors are recoloured without invalidating a completed global scan.
for name, setter in (('platforms', 'Changed'), ('jumppads', 'JumpPadChanged')):
    code = source[name]
    fn = code.split('void ' + setter + '(', 1)[1].split('\n}', 1)[0]
    assert 'if (s_targets.empty() && s_scan_cursor < 0)' in fn
    assert 's_pending_colour = s_targets.size();' in fn
    assert 's_save_due = GetTickCount64() + 350;' in fn
    assert 'if (!affects_gameplay)' in fn
    assert 's_save_dirty = false;' in fn  # immediate gameplay persistence
    assert 'ColourWorkBudget budget(700)' in code
    assert 'ColourWorkBudget apply_budget(1100)' in code
    assert 'FindReflectedFunction(object, method)' in code
    assert 'cache[cls] = result' in code
    assert 'const bool urgent = s_pending_colour != 0;' in code
    assert 'serial_number == entry.serial' in code
    assert 's_targets.clear();' in code  # stale object references on level rebind
assert 'const bool check_speed = now >= s_next_speed_check;' in source['platforms']
assert 'if (check_speed && (settings.speed_percent != 100 || target.rate_modified))' in source['platforms']
assert 'if (needs_height && now >= s_next_height_check)' in source['jumppads']
assert 'ObserveNearbyJumpPadImpulse(player)' in source['jumppads']

# Buttons preserve native property/ABI validation, cache verified signatures,
# and avoid 37 simultaneous material creations.
buttons = source['sections']
assert 'struct ButtonPropertyCache' in buttons
assert 'FindClassPropertyOffset(cls, property)' in buttons
assert 'right_type = IsReadable' in buttons
assert 'cached = {cls, actual_offset};' in buttons
assert 'ButtonCall(mesh, 0)' in buttons and 'ButtonCall(mesh, 1)' in buttons
assert 'ButtonCall(mid, 2)' in buttons
assert 'RequestButtonColourRefresh();' in buttons
assert 'ColourWorkBudget tint_budget(1000)' in buttons
assert 's_button_colour_dirty_remaining' in buttons

# Tiles and walls use the existing event path, with a fast edit pass and a
# fallback rate cap. Do not build MIDs inside the native contact callback.
tiles = source['tiles']
assert 's_tile_colour_dirty_remaining = kTotalColourableTiles;' in tiles
assert 'ColourWorkBudget palette_budget(1100)' in tiles
assert 'if (now >= next_random_fallback_ms)' in tiles
assert 'if (!urgent && s_tile_palette.cached && s_tile_palette.count > 0)' in tiles
contact = tiles.split('void NotifyNativeTileContactCompleted(', 1)[1].split('\n}', 1)[0]
assert 'g_process_event(' not in contact

# The already-optimised surface and underwater feature must remain independent.
water = source['water']
assert 'UpdateUnderwaterColours(player);' in water
assert 'if (!discovered_this_tick && !s_targets.empty()' in water
assert 's_surface_edit_quiet_until' in water
print('PASS: cosmetic discovery/updates cached and bounded; gameplay paths preserved')

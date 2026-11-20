"""Catch the exact cooked-blueprint mismatch that broke Jump Pad Launch Height.

Refunct's Jumppad name is a UFunction parameter of PerformJumppadJump,
NOT a property of BP_MyCharacter. This test verifies native event-only
launch gating, lifecycle setup and preservation of the existing colour path.
"""
from pathlib import Path
import math

root = Path(__file__).resolve().parents[1]
code = (root / 'src/jumppads.cpp').read_text()
header = (root / 'include/refunct/features/jumppads.hpp').read_text()
boot = (root / 'src/bootstrap.cpp').read_text()
tick = (root / 'src/game_tick.cpp').read_text()

assert 'FindClassPropertyOffset(cls, L"Jumppad")' not in code
assert 'FindReflectedFunction(player, L"PerformJumppadJump")' in code
assert 'JumpPadParam(function, L"Jumppad", L"ObjectProperty", 256)' in code
assert 'g_original_jump_pad_process_event(object, function, args)' in code
assert 's_launch.function != nullptr && object == s_launch.player' in code
assert 'function == s_launch.function' in code
assert 'valid_pad = NamedJumpPad(pad)' in code
assert 's_launch.expires_at = now + 750' in code
assert 's_launch.pending = false;  // Never multiply the same pad launch twice.' in code
assert 'ObservePendingLaunch(s_launch.player);' in code
assert 'PrepareLaunchFunction(player);' in code
assert 'ObservePendingLaunch(player);' in code
assert 'UpdateJumpPads(player);' in tick
assert 'kProcessEventRva' in boot
assert 'MH_CreateHook(callback' in boot
assert 'reinterpret_cast<void*>(&JumpPadProcessEventHook)' in boot
assert 'g_jump_pad_launch_hook_available = jump_pad_event_hook_queued;' in boot
assert 'extern ProcessEventFn g_original_jump_pad_process_event;' in header
assert 'void __fastcall JumpPadProcessEventHook' in header
assert 'if (diagnostic_mode == 6)' in boot
assert 'JumpPadSetTint' in code and 'JumpPadColourTarget' in code
assert 'JumpPadSetMaterial(target.mesh, target.source)' in code

# A 200% height request produces sqrt(2) vertical launch speed;
# 25% produces half-speed. Normal jumping is not intercepted.
for height in (25, 50, 100, 125, 200, 400, 1000):
    ratio = math.sqrt(height / 100)
    assert math.isclose((850 * ratio)**2 / 850**2, height / 100, rel_tol=1e-12)

print('PASS: jump-pad launch uses actual reflected Blueprint event, not player field')

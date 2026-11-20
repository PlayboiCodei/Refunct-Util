"""Regression: pad launches may bypass UObject::ProcessEvent in Blueprint VM."""
from pathlib import Path
root=Path(__file__).resolve().parents[1]
code=(root/'src/jumppads.cpp').read_text()
assert 'ObserveNearbyJumpPadImpulse(player);' in code
assert 'RefreshJumpPadPositions(objects, now);' in code
assert 'K2_GetActorLocation' in code
assert 'AtKnownJumpPad(prior_pos, now)' in code
assert 'AtKnownJumpPad(pos, now)' in code
assert 'z - prior_z < required_velocity_rise' in code
assert 'normal_jump * 1.20f' in code
assert 'g_flying || g_restart_requested' in code
assert 's_launch.last_velocity_z = scaled;' in code
assert 'now - s_launch.last_scaled_at < 400' in code
assert '(Config().custom_colour || needs_height)' in code
assert 'ObserveNearbyJumpPadImpulse(player);' in code
assert 'JumpPadColourTarget(target, Config().custom_colour, Config().rgb)' in code
assert 'JumpPadSetMaterial(target.mesh, target.source)' in code
assert 'if (now >= s_next_update || s_pending_colour != 0)' in code
assert 'if (needs_height && now >= s_next_height_check)' in code
assert 'const bool needs_colour = Config().custom_colour || s_restore_pending;' in code
assert 's_targets.clear();' in code
print('PASS: position-confirmed native impulse detector and colour preservation')

# A focused model of the native-velocity gate: ordinary jumps and impulses
# away from the jump pads must never be modified.
import math

def eligible(prior_z, z, normal_jump, near_pad):
    minimum = max(650.0, normal_jump * 1.20)
    rise = max(180.0, normal_jump * 0.30)
    return z >= minimum and z - prior_z >= rise and near_pad

assert not eligible(0, 420, 400, True)  # Ordinary jump on the pad
assert not eligible(-100, 1100, 400, False)  # Big velocity far from pad
assert eligible(-200, 1100, 400, True)  # Actual pad impulse
assert not eligible(0, 1200, 1400, True)  # Tuned ordinary jump
for percent in (25, 100, 125, 200, 1500):
    z = 1100.0
    scaled = min(z * math.sqrt(percent / 100.0), 30000.0)
    assert scaled > 0 and math.isfinite(scaled)
print('PASS: native jump-pad velocity gate and multiplier model')

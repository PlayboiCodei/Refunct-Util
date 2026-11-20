"""Prevent silent false negatives and ambiguous diagnostics in jump-pad launch scaling."""
from pathlib import Path
root = Path(__file__).resolve().parents[1]
code = (root / 'src/jumppads.cpp').read_text()
boot = (root / 'src/bootstrap.cpp').read_text()
assert 'Jump pad launch height: ProcessEvent hook ready' in boot
assert 'Jump pad launch height: ProcessEvent hook unavailable' in code
assert 'Jump pad launch height: tracking PerformJumppadJump' in code
assert 'Jump pad launch height: PerformJumppadJump event observed' in code
assert 'Jump pad launch height: applied ' in code
assert 'Jump pad launch height: pad argument unverified; using matched launch event' in code
assert 'const bool has_velocity = ReadLaunchVelocity(s_launch.player, before);' in code
assert 'const bool has_velocity = valid_pad && ReadLaunchVelocity' not in code
# It must still only arm on the exact player and named UFunction.
assert 's_launch.function != nullptr && object == s_launch.player' in code
assert 'function == s_launch.function' in code
assert 's_launch.pending = true' in code
assert 's_launch.pending = false;  // Never multiply the same pad launch twice.' in code
assert 'JumpPadColourTarget' in code and 'JumpPadSetTint' in code
print('PASS: jump pad launch fallbacks and event diagnostics')

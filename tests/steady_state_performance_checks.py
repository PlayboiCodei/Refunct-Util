"""Regression checks for slow-burn game-thread work in live Refunct worlds."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
pillars = (root / "src/pillars.cpp").read_text(encoding="utf-8")
sections = (root / "src/sections.cpp").read_text(encoding="utf-8")

# Native name lookup and global registry traversal are expensive.
# One completed sweep remains complete until a cached object is invalidated
# or the player/world is replaced. Settings toggles reuse the same cache.
assert "bool s_discovery_complete = false;" in pillars
assert "s_discovery_complete = true;" in pillars
assert "if (s_player != player)" in pillars
assert "s_scan_cursor = need_discovery ? 0 : -1;" in pillars
assert "if (need_discovery && !s_discovery_complete && s_scan_cursor < 0)" in pillars
assert "s_discovery_complete = false;" in pillars
assert "kRescanMs" not in pillars and "s_next_scan" not in pillars

# The entry was verified to be a real pillar at discovery time; on each
# sweep just verify the index and serial instead of allocating Unreal names.
assert "if (!Valid(target, objects))" in pillars
assert "if (!Valid(target, objects) || !IsDistributorOwnedMesh(target.mesh))" not in pillars
assert "// The cached UObject was destroyed/replaced." in pillars
assert "// Keep the cache even when pillar customisation is fully OFF." in pillars
assert "if (enabled || target.mid)" in pillars
assert "if (removed || target.removal_saved)" in pillars
assert "if (!visible || target.visual_saved)" in pillars

# Manual overrides need periodic consistency checks, but native animations
# must not be throttled. World restarts clear this timer with s_sections = {}.
assert "ULONGLONG next_override_check_ms = 0;" in sections
assert "const bool check_override = now >= s_sections.next_override_check_ms;" in sections
assert "if (check_override) s_sections.next_override_check_ms = now + 50;" in sections
assert "if (s_sections.transitions[i].active)" in sections
assert "AdvanceSectionTransition(i, now);" in sections
assert "} else if (check_override &&" in sections
assert "void ResetSectionPlacementInternals() {" in sections
assert "    s_sections = {};" in sections
assert "MoveSectionToMode(i, s_sections.modes[i], false, preserve_native_z);" in sections

# Timing model: at 120 FPS, the actual animations still advance 120x/s,
# but persistent override validation runs 20x/s (not 120x/s).
checks, transitions = 0, 0
next_check = 0
for now in range(0, 1000, 1000 // 120):
    transitions += 1
    if now >= next_check:
        checks += 1
        next_check = now + 50
assert 10 <= checks <= 22, checks
assert transitions >= 100, transitions
print("Steady-state performance checks passed")

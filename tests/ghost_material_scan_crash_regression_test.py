"""Guard against the unsafe broad UE4 material introspection that crashed Refunct.

Keeps the known-working Shipping compatible six-part no-collision ghost
without enumerating reflected material parameter arrays on the game tick.
"""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
source = (root / "src" / "ghost.cpp").read_text(encoding="utf-8")
assert "GhostPrepareDeferredMobility(component)" in source
assert "BeginDeferredActorSpawnFromClass" in source
assert "SetCollisionEnabled" in source
assert "void DrawWorldPBGhost(" in source
assert "kWorldGhostLimbs" in source
# Do not revive unverified material-array strides or unbounded material probes
# without real engine-side ABI validation and an in-game safety test.
for unsafe in (
    "DiscoverGhostAppearance(",
    "GhostMaterialParameter(",
    "ApplyWorldGhostAppearance(",
    "GhostMaterialBlendMode(",
    "ScalarParameterValues",
    "VectorParameterValues",
):
    assert unsafe not in source, f"Unsafe material probe reintroduced: {unsafe}"
print("PASS: world-space ghost retained; unsafe material scanning disabled")

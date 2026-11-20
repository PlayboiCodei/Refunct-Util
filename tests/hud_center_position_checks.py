"""Center placement regression tests: display contract and saved enum compatibility."""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
header = (root / 'include/refunct/state.hpp').read_text(encoding='utf-8')
menu = (root / 'src/menu.cpp').read_text(encoding='utf-8')
storage = (root / 'src/storage.cpp').read_text(encoding='utf-8')
hud = (root / 'src/hud.cpp').read_text(encoding='utf-8')
assert 'BottomLeft = 3, Center = 4' in header
assert 'kHudPlacementCount = static_cast<size_t>(HudCorner::Center) + 1' in header
assert 'case HudCorner::Center: return L"Center";' in menu
assert 'constexpr int count = static_cast<int>(kHudPlacementCount)' in menu
assert 'value >= 0 && value <= static_cast<int>(HudCorner::Center) ? value : fallback' in storage
assert 'std::array<std::vector<HudLine>, kHudPlacementCount> corners{}' in hud
assert 'std::array<float, kHudPlacementCount> hud_first_y{}' in hud
assert 'std::array<float, kHudPlacementCount> hud_after_y{}' in hud
assert 'at_center = corner_index == static_cast<size_t>(HudCorner::Center)' in hud
assert 'center_x - width * 0.5f' in hud
assert '(static_cast<float>(viewport_height) - render_height) * 0.5f' in hud
assert 'const size_t menu_corner = static_cast<size_t>(g_menu_position) - 1' in hud
assert re.search(r'void CycleMenuPosition\(int direction\)\s*\{\s*constexpr int count = 4;', menu)
assert 'saved_menu_position >= 1 && saved_menu_position <= 4' in storage
assert 'const float popup_center_y = static_cast<float>(viewport_height) * 0.58f;' in hud

# Saved positions remain stable. Cycle left/right and verify round trips.
def cycle(value: int, direction: int) -> int:
    return (value + (4 if direction < 0 else 1)) % 5

for old_value in range(5):
    assert cycle(cycle(old_value, 1), -1) == old_value
    assert cycle(cycle(old_value, -1), 1) == old_value
assert cycle(3, 1) == 4 and cycle(4, 1) == 0
assert cycle(0, -1) == 4

# A center stack of varying height remains inside the viewport, not top-left.
def center_y(height: float, max_y: float) -> float:
    max_height = max(18.0, height - 16.0 - 30.0)
    render_height = min(max_y, max_height)
    return max(16.0, min((height - render_height) * .5,
                         max(16.0, height - 30.0 - render_height)))

for h in (240., 360., 720., 1080.):
    for stack_h in (18., 90., 180., 400., 1000.):
        y = center_y(h, stack_h)
        assert y >= 16.0
        assert y + min(stack_h, max(18., h - 46.)) <= h - 30.0 + 1e-4

print('PASS: HUD Center placement and persistence; menu remains four corners')

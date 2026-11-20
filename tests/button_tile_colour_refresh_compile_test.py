"""Compile and execute the actual bounded post-button scan with test doubles."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/tiles.cpp').read_text(encoding='utf-8')
start = source.index('void RefreshTilesAfterButtonProgress() {')
end = source.index('\n}\n\n} // namespace', start) + 2
actual_function = source[start:end]

stub = r'''
#include <array>
#include <cstdint>
#include <cassert>
#include <vector>
#include <cstddef>
struct MyCharacter {};
struct ColourWorkBudget { explicit ColourWorkBudget(int) {} bool expired() const { return false; } };
constexpr int kCount = 251;
struct Runtime {
    bool cached = true;
    int button_refresh_cursor = 0;
    int button_refresh_remaining = kCount;
    int button_refresh_passes = 3;
    int count = kCount;
    void* tile_class = nullptr;
    MyCharacter* player = nullptr;
    struct State { bool observed = true; bool was_selected = false; };
    std::array<State, kCount> random_colours{};
    std::array<int32_t, kCount> applied_native_colours{};
    std::array<void*, kCount> actors{};
};
Runtime s_tile_palette;
std::array<int32_t, kCount> native_colours{};
std::vector<size_t> notifications;
bool RandomTileRGBEnabled() { return true; }
bool ReadTileColor(void* actor, int32_t, int32_t& out, void*) {
    size_t index = reinterpret_cast<uintptr_t>(actor) - 1;
    if (index >= kCount) return false;
    out = native_colours[index];
    return true;
}
void NotifyNativeTileContactCompleted(void* actor, MyCharacter*) {
    notifications.push_back(reinterpret_cast<uintptr_t>(actor) - 1);
}
'''
main = r'''
int main() {
    for (int i = 0; i < kCount; ++i) {
        s_tile_palette.actors[i] = reinterpret_cast<void*>(uintptr_t(i + 1));
        s_tile_palette.applied_native_colours[i] = 0;
    }
    native_colours[200] = 3; // Selected by Blueprint, no tile contact.
    // The cached-state event is detected within a few ticks and never applies
    // materials inside the scan itself.
    for (int tick = 0; tick < 4 && notifications.empty(); ++tick)
        RefreshTilesAfterButtonProgress();
    assert(notifications.size() == 1 && notifications[0] == 200);
    assert(s_tile_palette.button_refresh_passes >= 1);
    // No native state change means no notifications.
    notifications.clear();
    native_colours[200] = 0;
    s_tile_palette.random_colours[200].was_selected = false;
    for (int tick = 0; tick < 12; ++tick) RefreshTilesAfterButtonProgress();
    assert(notifications.empty());
    assert(s_tile_palette.button_refresh_remaining == 0);
    return 0;
}
'''
with tempfile.TemporaryDirectory() as tmp:
    path = Path(tmp) / 'button_colour_scan_test.cpp'
    binary = Path(tmp) / 'button_colour_scan_test'
    path.write_text(stub + actual_function + main, encoding='utf-8')
    subprocess.run(['g++', '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror',
                    str(path), '-o', str(binary)], check=True, timeout=30)
    subprocess.run([str(binary)], check=True, timeout=10)
print('PASS: actual button-triggered scanner compiles and detects native transitions')

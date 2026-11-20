"""Verify that the production ghost renderer anchors to the standing capsule.

Compiles real ghost.cpp with Win32/UE4 mocks. Confirms render-only vertical
alignment on the ground and while jumping without changing ghost samples.
Does not verify actual in-game rendering.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
compiler = shutil.which('g++') or shutil.which('clang++')
if compiler is None:
    print('SKIP: no portable C++20 compiler')
    raise SystemExit(0)

source = (root / 'src/ghost.cpp').read_text(encoding='utf-8')
source = 'namespace refunct {' + source.split('namespace refunct {', 1)[1]
preamble = (root / 'tests/ghost_native_mock_preamble.hpp').read_text(encoding='utf-8')
main = r'''
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

std::vector<float> projected_z;
int drawn_rectangles = 0;

int main() {
    using namespace refunct;
    SceneComponent root{{40.f, 80.f, 188.f}}; // Foot level = 100
    struct FakeController {void* cls; unsigned char reserved[96]{};};
    int fake_class = 0, fake_method = 0;
    FakeController camera{&fake_class};
    MyCharacter player{&root, reinterpret_cast<PlayerController*>(&camera)};
    g_player = &player;
    g_pb.total_seconds = 1.0;
    s_loaded_profile = g_pb_path;
    s_loaded_settings = true;
    s_show = true;
    s_button_sync = false;
    s_use_fastest_sections = false;
    // This test checks the original standing/jumping geometry, not proximity
    // fading; the separate pass-through test covers the new default mode.
    SetPBGhostPassThroughEnabled(false);
    g_elapsed_seconds = 0.0;
    s_playback = {{0,40.f,80.f,188.f,0.f}, {1000,40.f,80.f,288.f,0.f}};
    const auto original_samples = s_playback;

    s_projection = {};
    s_projection.controller_class = &fake_class;
    s_projection.method = &fake_method;
    s_projection.world = 0;
    s_projection.screen = 32;
    s_projection.result = 48;
    g_process_event = +[](void*, void*, void* context) {
        auto* bytes = static_cast<std::uint8_t*>(context);
        Vec3 world{};
        std::memcpy(&world, bytes, sizeof(world));
        projected_z.push_back(world.z);
        const float screen[] = {400.f, 700.f - world.z};
        std::memcpy(bytes + 32, screen, sizeof(screen));
        bytes[48] = 1;
    };
    g_draw_rect = +[](void*, LinearColor, float, float, float, float) {
        ++drawn_rectangles;
    };

    // Standing: draw feet at ground Z=100, top at Z=276.
    g_mock_tick_ms = 1000;
    DrawPBGhost(nullptr, 1920, 1080);
    assert(projected_z.size() == 2 && drawn_rectangles == 6);
    assert(std::abs(projected_z[0] - 100.f) < .001f);
    assert(std::abs(projected_z[1] - 276.f) < .001f);
    assert(std::abs(projected_z[1] - projected_z[0] - 176.f) < .001f);

    // Ghost is 100 units above that ground during a recorded jump.
    projected_z.clear();
    g_mock_tick_ms = 1010;
    g_elapsed_seconds = 1.0;
    DrawPBGhost(nullptr, 1920, 1080);
    assert(projected_z.size() == 2 && drawn_rectangles == 12);
    assert(std::abs(projected_z[0] - 200.f) < .001f);
    assert(std::abs(projected_z[1] - 376.f) < .001f);

    // Applying the visual offset must not change the original track.
    assert(s_playback.size() == original_samples.size());
    for (std::size_t i = 0; i < s_playback.size(); ++i) {
        assert(s_playback[i].z == original_samples[i].z);
    }
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='ghost_ground_') as directory:
    unit = Path(directory) / 'ghost_ground.cpp'
    unit.write_text(preamble + source + main, encoding='utf-8')
    executable = Path(directory) / 'ghost_ground'
    subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', '-O2',
                    '-I', str(root / 'include'), str(unit), '-o', str(executable)],
                   check=True)
    subprocess.run([str(executable)], check=True)
print('PASS: ghost standing feet, head, and jumping heights align with player capsule')

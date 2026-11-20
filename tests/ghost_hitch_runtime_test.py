"""Compile production ghost.cpp against inert test mocks.

Prove capture-buffer reuse and that expensive UE4 projections are capped
without throttling rectangle drawing. No Win32/in-game claims.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
compiler = shutil.which('g++') or shutil.which('clang++')
if not compiler:
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
#include <cstdint>
#include <cstring>
struct FakeController {
    void* class_ptr = nullptr;
    unsigned char extra[96]{};
};
int main() {
    using namespace refunct;
    SceneComponent component{{20.f, 30.f, 40.f}};
    PlayerController regular_controller{{0.f, 180.f, 0.f}};
    MyCharacter pawn{&component, &regular_controller};
    g_player = &pawn;

    // The full two-hour capacity is allocated once, not during the run.
    UpdatePBGhostRecording(&pawn);
    assert(s_capture.started && s_capture.usable);
    assert(s_capture.samples.capacity() >= ghost_data::kMaxSamples);
    assert(s_capture.samples.size() == 1);
    auto* storage = s_capture.samples.data();
    const auto first_checks = g_mock_readability_checks;
    g_elapsed_seconds = 0.010;
    UpdatePBGhostRecording(&pawn);
    assert(s_capture.samples.size() == 1);  // still below 50 ms
    assert(g_mock_readability_checks == first_checks); // no VirtualQuery calls
    g_elapsed_seconds = 0.050;
    UpdatePBGhostRecording(&pawn);
    assert(s_capture.samples.size() == 2);
    assert(g_mock_readability_checks == first_checks + 2);
    ResetPBGhostCapture();
    assert(!s_capture.started && s_capture.samples.empty());
    assert(s_capture.samples.data() == storage); // reset reused buffer
    g_last_run_start = 200.0;
    g_elapsed_seconds = 0.0;
    UpdatePBGhostRecording(&pawn);
    assert(s_capture.samples.data() == storage);

    // Synthetic reflected projection, with a persistent controller class.
    FakeController fake_controller{};
    int dummy_class = 0, dummy_method = 0;
    fake_controller.class_ptr = &dummy_class;
    pawn.controller = reinterpret_cast<PlayerController*>(&fake_controller);
    s_projection = {};
    s_projection.controller_class = &dummy_class;
    s_projection.method = &dummy_method;
    s_projection.world = 0;
    s_projection.screen = 32;
    s_projection.result = 48;
    s_playback = {{0,20.f,30.f,40.f,0.f}, {1000,120.f,30.f,40.f,0.f}};
    g_pb.total_seconds = 1.0;
    s_loaded_profile = g_pb_path;
    s_draw_cache = {};
    s_loaded_settings = true; // Keep mocked INI loading from restoring 3D default.
    s_pass_through = false; // Projection benchmark deliberately uses 2D mode.
    g_elapsed_seconds = 0.10;
    g_timer_complete = false;
    // C++ function pointer mocks need capture-free lambdas, so use globals.
    extern int mock_projection_calls, mock_draw_calls;
    mock_projection_calls = 0;
    mock_draw_calls = 0;
    g_process_event = +[](void*, void*, void* context) {
        ++mock_projection_calls;
        auto* bytes = static_cast<std::uint8_t*>(context);
        Vec3 world{};
        std::memcpy(&world, bytes, sizeof(world));
        const float screen[] = {300.f + world.x * .01f, 600.f - world.z * .08f};
        std::memcpy(bytes + 32, screen, sizeof(screen));
        bytes[48] = 1;
    };
    g_draw_rect = +[](void*, LinearColor, float, float, float, float) {
        ++mock_draw_calls;
    };
    g_mock_tick_ms = 1000;
    DrawPBGhost(nullptr, 1920, 1080);
    assert(mock_projection_calls == 2 && mock_draw_calls == 6);
    g_mock_tick_ms = 1004;
    g_elapsed_seconds = 0.104;
    DrawPBGhost(nullptr, 1920, 1080);
    assert(mock_projection_calls == 2 && mock_draw_calls == 12);
    g_mock_tick_ms = 1009;
    g_elapsed_seconds = 0.109;
    DrawPBGhost(nullptr, 1920, 1080);
    assert(mock_projection_calls == 4 && mock_draw_calls == 18);
    g_mock_tick_ms = 1010;
    DrawPBGhost(nullptr, 1280, 720); // viewport changed: cache invalid
    assert(mock_projection_calls == 6);
    s_draw_cache = {};
    g_elapsed_seconds = 0.01; // backward time: immediate invalidation
    DrawPBGhost(nullptr, 1280, 720);
    assert(mock_projection_calls == 8);
    // Turning visibility OFF must stop all draw/projection calls.
    s_show = false;
    g_mock_tick_ms = 1020;
    DrawPBGhost(nullptr, 1280, 720);
    assert(mock_projection_calls == 8 && mock_draw_calls == 30);
    return 0;
}
int mock_projection_calls = 0;
int mock_draw_calls = 0;
'''
# Declarations must be visible to lambdas before their uses; main source defines
# the externs at global scope for this test translation unit.
with tempfile.TemporaryDirectory() as directory:
    src = Path(directory) / 'ghost_perf.cpp'
    src.write_text(preamble + 'extern int mock_projection_calls;\nextern int mock_draw_calls;\n' + source + main, encoding='utf-8')
    binary = Path(directory) / 'ghost_perf'
    subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', '-O2',
                    '-I', str(root / 'include'), str(src), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('PASS: ghost capture buffer reused and projection cache reduces native calls')

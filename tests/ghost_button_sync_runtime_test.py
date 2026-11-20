"""Build production ghost.cpp against inert UE4/Win32 mocks and exercise button clock.

No game process or native Win32 runtime is involved.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
compiler = shutil.which("g++") or shutil.which("clang++")
if not compiler:
    print("SKIP: C++20 compiler unavailable")
    raise SystemExit(0)
source = (root / "src/ghost.cpp").read_text(encoding="utf-8")
assert "namespace refunct {" in source
source = "namespace refunct {" + source.split("namespace refunct {", 1)[1]
preamble = (root / "tests/ghost_native_mock_preamble.hpp").read_text(encoding="utf-8")
main = r'''
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdint>
#include <cstring>
int main() {
    using namespace refunct;
    SceneComponent component{{20.f, 30.f, 40.f}};
    struct FakeController {void* cls; unsigned char reserved[96]{};};
    int fake_class = 0;
    int fake_method = 0;
    FakeController camera{&fake_class};
    MyCharacter player{&component, reinterpret_cast<PlayerController*>(&camera)};
    g_player = &player;
    g_pb.total_seconds = 10.0;
    g_pb.button_seconds[0] = 2.0;
    g_pb.button_seconds[1] = 4.0;
    g_pb.button_seconds[2] = 6.0;
    s_loaded_profile = g_pb_path;
    s_loaded_settings = true;
    s_show = true;
    s_pass_through = false; // This runtime test exercises the 2D HUD projection.
    s_button_sync = true;
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
        float screen[] = {400.f + world.x * .01f, 600.f - world.z * .07f};
        std::memcpy(bytes + 32, screen, sizeof(screen));
        bytes[48] = 1;
    };
    g_draw_rect = +[](void*, LinearColor, float, float, float, float) {};
    s_playback = {{0,0,0,0,0}, {10000,100,0,0,0}};

    // Ordinary first segment, time relative to the run start.
    g_last_button_count = 0;
    g_elapsed_seconds = 1.0;
    g_mock_tick_ms = 1000;
    DrawPBGhost(nullptr, 1920, 1080);
    assert(s_draw_cache.populated && s_draw_cache.elapsed_ms == 1000);

    // Button 1: jump to the PB's first split at exactly t=2s.
    g_last_button_count = 1;
    g_button_splits = {{1, 3.0}};
    g_elapsed_seconds = 3.0;
    g_mock_tick_ms = 1000; // Same HUD tick: button-count must invalidate cache.
    DrawPBGhost(nullptr, 1920, 1080);
    assert(s_draw_cache.populated && s_draw_cache.elapsed_ms == 2000);
    g_elapsed_seconds = 4.5;
    g_mock_tick_ms = 1010;
    DrawPBGhost(nullptr, 1920, 1080);
    assert(s_draw_cache.elapsed_ms == 3500);

    // Ghost reaches button 2 first; stays still until the player arrives.
    g_elapsed_seconds = 8.0;
    g_mock_tick_ms = 1020;
    DrawPBGhost(nullptr, 1920, 1080);
    assert(s_draw_cache.elapsed_ms == 4000);
    g_elapsed_seconds = 9.0;
    g_mock_tick_ms = 1030;
    DrawPBGhost(nullptr, 1920, 1080);
    assert(s_draw_cache.elapsed_ms == 4000);

    g_last_button_count = 2;
    g_button_splits.push_back({2, 9.0});
    g_mock_tick_ms = 1030;
    DrawPBGhost(nullptr, 1920, 1080);
    assert(s_draw_cache.populated && s_draw_cache.elapsed_ms == 4000);
    g_elapsed_seconds = 9.5;
    g_mock_tick_ms = 1040;
    DrawPBGhost(nullptr, 1920, 1080);
    assert(s_draw_cache.elapsed_ms == 4500);

    // Incomplete PB splits: hide rather than play the wrong segment.
    g_pb.button_seconds[2] = -1;
    g_mock_tick_ms = 1050;
    DrawPBGhost(nullptr, 1920, 1080);
    assert(!s_draw_cache.populated);
    g_pb.button_seconds[2] = 6;
    g_button_splits.clear(); // Button event absent.
    g_mock_tick_ms = 1060;
    DrawPBGhost(nullptr, 1920, 1080);
    assert(!s_draw_cache.populated);

    // Practice / synthetic checkpoints must not masquerade as native contacts.
    g_button_splits = {{2, 9.0}};
    g_practice_active = true;
    g_mock_tick_ms = 1070;
    DrawPBGhost(nullptr, 1920, 1080);
    assert(!s_draw_cache.populated);
    g_practice_active = false;

    // Legacy full-run mode uses elapsed time without splitting it.
    SetPBGhostButtonSyncEnabled(false);
    g_elapsed_seconds = 9.5;
    g_mock_tick_ms = 1080;
    DrawPBGhost(nullptr, 1920, 1080);
    assert(s_draw_cache.populated && s_draw_cache.elapsed_ms == 9500);
    SetPBGhostButtonSyncEnabled(true);
    g_last_button_count = 0;
    g_button_splits.clear();
    g_elapsed_seconds = 0;
    g_mock_tick_ms = 1090;
    DrawPBGhost(nullptr, 1920, 1080);
    assert(s_draw_cache.populated && s_draw_cache.elapsed_ms == 0);
    return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    unit = Path(directory) / "ghost_button_sync_mock.cpp"
    unit.write_text(preamble + source + main, encoding="utf-8")
    binary = Path(directory) / "ghost_button_sync_mock"
    subprocess.run([compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-O2",
                    "-I", str(root / "include"), str(unit), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("PASS: per-button PB ghost clocks, waiting, cache invalidation, and legacy playback")

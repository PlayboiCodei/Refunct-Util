"""Compile production Ghost playback against inert Win32/UE mocks.
Tests fastest/full preference, continuous and button-gated clocks, and
incomplete-run fallback without touching real game memory.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
compiler = shutil.which('g++') or shutil.which('clang++')
if not compiler:
    raise SystemExit('SKIP: C++20 compiler unavailable')
source = (root / 'src/ghost.cpp').read_text(encoding='utf-8')
source = 'namespace refunct {' + source.split('namespace refunct {', 1)[1]
preamble = (root / 'tests/ghost_native_mock_preamble.hpp').read_text(encoding='utf-8')
main = r'''
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
int main() {
    using namespace refunct;
    s_loaded_settings = true;
    s_loaded_profile = g_pb_path;
    s_show = true;
    g_pb.total_seconds = 7000.0 / 1000.0;
    g_pb.button_seconds[0] = 3.0;
    g_pb.button_seconds[1] = 7.0;
    g_pb.best_segment_seconds[0] = 2.0;
    g_pb.best_segment_seconds[1] = 3.0;
    s_playback = {{0, 10, 0, 0, 0}, {7000, 70, 0, 0, 0}};
    s_fastest[0] = {{0, 100, 0, 0, 0}, {2000, 200, 0, 0, 0}};
    s_fastest[1] = {{0, 300, 0, 0, 0}, {3000, 400, 0, 0, 0}};
    const std::vector<ghost_data::Sample>* track = nullptr;
    std::uint32_t clock = 0;
    bool fastest = false, furthest = false;

    // Continuous gold sections from separate incomplete runs: 2 + 3 secs.
    s_button_sync = false;
    SetPBGhostFastestSectionsEnabled(true);
    assert(IsPBGhostFastestSectionsEnabled());
    g_elapsed_seconds = 1.0;
    assert(ContinuousFastestSectionsPlaybackMs(clock, track, fastest, furthest));
    assert(track == &s_fastest[0] && clock == 1000 && fastest && !furthest);
    g_elapsed_seconds = 2.0;
    assert(ContinuousFastestSectionsPlaybackMs(clock, track, fastest, furthest));
    assert(track == &s_fastest[0] && clock == 2000);
    g_elapsed_seconds = 2.5;
    assert(ContinuousFastestSectionsPlaybackMs(clock, track, fastest, furthest));
    assert(track == &s_fastest[1] && clock == 500 && fastest);
    g_elapsed_seconds = 9.0;
    assert(ContinuousFastestSectionsPlaybackMs(clock, track, fastest, furthest));
    assert(track == &s_fastest[1] && clock == 3000);

    // Full-run mode ignores golds, even when they are faster.
    SetPBGhostFastestSectionsEnabled(false);
    assert(!IsPBGhostFastestSectionsEnabled());
    assert(!s_draw_cache.populated);
    g_elapsed_seconds = 2.5;
    g_last_button_count = 0;
    assert(ButtonSynchronizedPlaybackMs(clock, track, fastest, furthest, false));
    assert(track == &s_playback && clock == 2500 && !fastest && !furthest);

    // With button sync ON, the player's Button 1 starts the next section.
    g_last_button_count = 1;
    g_button_splits = {{1, 5.0}};
    g_elapsed_seconds = 5.5;
    assert(ButtonSynchronizedPlaybackMs(clock, track, fastest, furthest, false));
    assert(track == &s_playback && clock == 3500 && !fastest);
    assert(ButtonSynchronizedPlaybackMs(clock, track, fastest, furthest, true));
    assert(track == &s_fastest[1] && clock == 500 && fastest);
    g_elapsed_seconds = 10.0;
    assert(ButtonSynchronizedPlaybackMs(clock, track, fastest, furthest, true));
    assert(clock == 3000); // waits at the end of the gold segment

    // Without a gold, the furthest incomplete run supplies real positions.
    s_fastest[1].clear();
    s_furthest.reached = 2;
    s_furthest.button_ms = {};
    s_furthest.button_ms[0] = 3500;
    s_furthest.button_ms[1] = 8000;
    s_furthest.samples = {{0, 50, 0, 0, 0}, {8000, 80, 0, 0, 0}};
    g_elapsed_seconds = 2.5;
    assert(ContinuousFastestSectionsPlaybackMs(clock, track, fastest, furthest));
    assert(track == &s_furthest.samples && clock == 4000 && !fastest && furthest);
    g_elapsed_seconds = 5.5;
    assert(ButtonSynchronizedPlaybackMs(clock, track, fastest, furthest, true));
    assert(track == &s_furthest.samples && clock == 4000 && furthest);

    // Invalid native button time must not substitute elapsed wall clock.
    g_button_splits.clear();
    assert(!ButtonSynchronizedPlaybackMs(clock, track, fastest, furthest, true));
    g_practice_active = true;
    assert(!ContinuousFastestSectionsPlaybackMs(clock, track, fastest, furthest));
    g_practice_active = false;
    return 0;
}
'''
with tempfile.TemporaryDirectory() as directory:
    test = Path(directory) / 'fastest_section_mode.cpp'
    test.write_text(preamble + source + main, encoding='utf-8')
    exe = Path(directory) / 'fastest_section_mode'
    subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', '-O2',
                    '-I', str(root/'include'), str(test), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('PASS: fastest/full source toggle, continuous composition, button sync, waiting, and furthest-run fallback')

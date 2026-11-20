"""Compile and execute actual ghost.cpp with inert Win32/UE4 mocks on Linux.

This verifies C++ syntax and capture transitions only; it is not an in-game
rendering test. The production source is included verbatim at run time.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
compiler = shutil.which('g++') or shutil.which('clang++')
if compiler is None:
    print('SKIP: C++ compiler unavailable')
    raise SystemExit(0)

source = (root / 'src/ghost.cpp').read_text(encoding='utf-8')
assert 'namespace refunct {' in source
source = 'namespace refunct {' + source.split('namespace refunct {', 1)[1]
preamble = (root / 'tests/ghost_native_mock_preamble.hpp').read_text(encoding='utf-8')
# Inline definitions avoid Release-mode NDEBUG suppressing assert test actions.
main = r'''
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
int main() {
    using namespace refunct;
    SceneComponent component{{10.f, 20.f, 30.f}};
    PlayerController controller{{25.f, 180.f, 0.f}};
    MyCharacter pawn{&component, &controller};
    g_player = &pawn;
    UpdatePBGhostRecording(&pawn);
    assert(s_capture.started && s_capture.usable && s_capture.samples.size() == 1);
    assert(s_capture.samples.front().pitch == 25.f);
    g_elapsed_seconds = 0.05;
    controller.control_rotation.pitch = 330.f;
    component.location.x = 30.f;
    UpdatePBGhostRecording(&pawn);
    assert(s_capture.samples.size() == 2);
    assert(s_capture.samples.back().pitch == -30.f);
    g_elapsed_seconds = 0.10;
    component.location.x = 50.f;
    g_timer_complete = true;
    UpdatePBGhostRecording(&pawn);
    assert(s_capture.complete && s_capture.samples.size() == 3);
    // Completion at the same rounded ms must still finish the capture.
    ResetPBGhostCapture();
    g_timer_complete = false; g_elapsed_seconds = 0.0;
    UpdatePBGhostRecording(&pawn);
    g_elapsed_seconds = 0.05;
    UpdatePBGhostRecording(&pawn);
    g_timer_complete = true;
    UpdatePBGhostRecording(&pawn);
    assert(s_capture.complete);
    // A practice or mid-run-started capture must never inherit old samples.
    g_run_started_at_zero = false;
    UpdatePBGhostRecording(&pawn);
    assert(!s_capture.started && s_capture.samples.empty());
    g_run_started_at_zero = true;
    g_last_button_count = 3;
    g_timer_complete = false;
    UpdatePBGhostRecording(&pawn);
    assert(s_capture.started && !s_capture.usable);
    return 0;
}
'''
# The full production file is compiled, with headers mocked only so that no
# native Refunct process is required to test its pure state-machine behaviour.
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    unit = path / 'ghost_mock.cpp'
    unit.write_text(preamble + source + main, encoding='utf-8')
    program = path / 'ghost_mock'
    subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', '-O2',
                    '-I', str(root / 'include'), str(unit), '-o', str(program)], check=True)
    subprocess.run([str(program)], check=True)
print('PASS: actual ghost.cpp compiles and capture state transitions work')

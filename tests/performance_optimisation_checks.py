"""Regression coverage for low-risk anti-hitch changes.

Exercises the *actual* cached-count summation extracted from practice.cpp in a
portable C++20 harness. UE4 reflection, Windows x86 linkage and live frame-time
measurements cannot be reproduced by this test.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
practice = (root / "src/practice.cpp").read_text(encoding="utf-8")
sections = (root / "src/sections.cpp").read_text(encoding="utf-8")
sky = (root / "src/sky.cpp").read_text(encoding="utf-8")
fog = (root / "src/fog.cpp").read_text(encoding="utf-8")

# All 37 buttons are observed while any World gameplay setting is active.
# Do not poll actors when those settings are at their defaults, and never
# prevent the manual repair pass from running.
check = sections.split("void UpdateButtonFix() {", 1)[1].split("\n} // namespace", 1)[0]
assert "if (!s_button_fix_manual_pending && !AnyWorldGameplayOptionActive())" in check
assert "for (int i = 0; i < kRefunctButtonCount; ++i)" in check
assert "if (!s_button_fix_manual_pending && i != state->buttons) continue;" not in check
assert check.index("!AnyWorldGameplayOptionActive()") < check.index("InitializeSectionButtons()")
assert "if (s_button_fix_manual_pending && remaining == attempted)" in check

# Disabled sky/fog customisation must not perform global UObject-array checks
# at rendered frame rate. Picker events and deferred INI writes still run.
for code, entry, lookup in (
    (sky, "void UpdateSkyColours(MyCharacter* player)", "const auto* objects = SkyObjects();"),
    (fog, "void UpdateFogColours(MyCharacter* player)", "const GlobalObjectArray* objects = Objects();"),
):
    tick = code.split(entry, 1)[1]
    assert tick.index("PollPicker();") < tick.index("FlushSettings();")
    assert tick.index("FlushSettings();") < tick.index("if (!needed || !player")
    assert tick.index("if (!needed || !player") < tick.index(lookup)

# Original checks and full-world scanning remain the fallback when the counts
# are stale or native progress cannot be explained by cached level arrays.
assert "s_loaded_native_counts.player == g_player" in practice
assert "s_loaded_native_counts.world == world" in practice
assert "s_loaded_native_counts.resets == state->resets" in practice
assert "s_loaded_native_counts.valid = false;" in practice
assert "total_cubes != kTotalCubes || total_tiles != kTotalColourableTiles" in practice
assert "if (SummarizeLoadedCounts(s_loaded_native_counts, *state, progress)) return;" in practice
progress_tick = practice.split("void UpdateProgressCounters() {", 1)[1]
assert progress_tick.index("SummarizeLoadedCounts(s_loaded_native_counts, *state, progress)") < progress_tick.index("const auto* objects = reinterpret_cast<const GlobalObjectArray*>")

compiler = shutil.which("g++") or shutil.which("clang++")
if not compiler:
    raise SystemExit("SKIP: portable C++20 compiler unavailable")
start = practice.index("struct LoadedNativeObjectCounts {")
end = practice.index("} // namespace\n\nvoid UpdateProgressCounters()", start)
actual = practice[start:end]

harness = r'''
#include <array>
#include <algorithm>
#include <cassert>
struct MyCharacter {};
constexpr int kPracticeClusterCount = 31;
struct LevelState { int level = 0, cubes = 0, platforms = 0; };
struct ProgressCounters { int loaded_cubes = 0, loaded_tiles = 0; bool loaded_valid = false; };
''' + actual + r'''
int main() {
    LoadedNativeObjectCounts snapshot{};
    // 18 cubes / 251 tiles, matching Refunct's validated map totals.
    for (int i = 0; i < kPracticeClusterCount; ++i) {
        snapshot.cubes[i] = i < 18 ? 1 : 0;
        snapshot.tiles[i] = 8 + (i < 3 ? 1 : 0);
    }
    snapshot.valid = true;
    for (int level = 0; level < kPracticeClusterCount; ++level) {
        LevelState state{level, 0, 0};
        ProgressCounters result{};
        assert(SummarizeLoadedCounts(snapshot, state, result));
        assert(result.loaded_valid);
        assert(result.loaded_cubes == (std::min)(level + 1, 18));
        assert(result.loaded_tiles == 8 * (level + 1) + (std::min)(level + 1, 3));
    }
    LevelState invalid{0, 19, 0};
    ProgressCounters untouched{};
    assert(!SummarizeLoadedCounts(snapshot, invalid, untouched));
    assert(!untouched.loaded_valid);
    snapshot.valid = false;
    assert(!SummarizeLoadedCounts(snapshot, LevelState{}, untouched));
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="refunct-perf-test-") as td:
    cpp = Path(td) / "main.cpp"
    exe = Path(td) / "test"
    cpp.write_text(harness, encoding="utf-8")
    subprocess.run([compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-O2", str(cpp), "-o", str(exe)], check=True, capture_output=True, text=True)
    subprocess.run([str(exe)], check=True, capture_output=True, text=True)
print("PASS: gated full-button scan, idle sky/fog guards, and native-count summation")

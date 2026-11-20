"""Compile/run the actual UpdateDiscVisibility body with mocked sky MID access.

This is a portable unit test of control flow, not UE4/Windows in-game validation.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/sky.cpp').read_text(encoding='utf-8')
start = source.index('void UpdateDiscVisibility() {')
stop = source.index('// Fallback for sky MIDs', start)
actual_function = source[start:stop]

harness = r'''
#include <cmath>
#include <cstdint>
#include <string>
#include <cassert>
using ULONGLONG = unsigned long long;
struct SkyScalar {
    bool name_known = false;
    float original = 0;
    float last_written = 0;
    bool modified = false;
    bool has_original = false;
};
struct MockSettings { bool disc_visible = true; } s_settings;
struct MockTarget { void* mid = nullptr; SkyScalar sun_brightness{}; } s_target;
const wchar_t* kDiscBrightnessParameter = L"Sun brightness";
ULONGLONG s_disc_retry_after = 0;
int s_stage = -1;
float native_brightness = 2.5f;
bool native_read_ok = true, native_write_ok = true, reject_restore_readback = false;
int writes = 0;
ULONGLONG GetTickCount64() { return 1234; }
void DiscStage(int stage, const wchar_t*) { s_stage = stage; }
void ResolveScalarNameFromMID(void*, SkyScalar& v, const wchar_t*) { v.name_known = true; }
bool GetMIDScalar(void*, int, float& v) {
    if (!native_read_ok) return false;
    v = reject_restore_readback && native_brightness != 0 ? 0 : native_brightness;
    return true;
}
bool SetMIDScalar(void*, int, float v) {
    ++writes;
    if (!native_write_ok) return false;
    native_brightness = v;
    return true;
}
'''
# In real code, FNameView is the scalar name; integer works in the mocks.
harness = harness.replace('bool name_known = false;', 'bool name_known = false; int name = 0;')
harness += actual_function
harness += r'''
int main() {
    // A normal user OFF -> ON roundtrip must restore the captured brightness.
    s_settings.disc_visible = false;
    UpdateDiscVisibility();
    assert(native_brightness == 0 && s_target.sun_brightness.modified);
    assert(s_target.sun_brightness.has_original);
    assert(std::fabs(s_target.sun_brightness.original - 2.5f) < 0.0001f);
    UpdateDiscVisibility(); // Native brightness is zero, but original stays 2.5.
    assert(std::fabs(s_target.sun_brightness.original - 2.5f) < 0.0001f);
    s_settings.disc_visible = true;
    UpdateDiscVisibility();
    assert(native_brightness == 2.5f && !s_target.sun_brightness.modified);

    // Failing writes cannot silently mark a restoration successful.
    s_settings.disc_visible = false;
    UpdateDiscVisibility();
    s_settings.disc_visible = true;
    native_write_ok = false;
    UpdateDiscVisibility();
    assert(s_target.sun_brightness.modified && native_brightness == 0);
    native_write_ok = true;
    UpdateDiscVisibility();
    assert(native_brightness == 2.5f && !s_target.sun_brightness.modified);

    // Failed read-back also retains the pending restore until it can be confirmed.
    s_settings.disc_visible = false;
    UpdateDiscVisibility();
    s_settings.disc_visible = true;
    reject_restore_readback = true;
    UpdateDiscVisibility();
    assert(s_target.sun_brightness.modified);
    reject_restore_readback = false;
    UpdateDiscVisibility();
    assert(native_brightness == 2.5f && !s_target.sun_brightness.modified);

    // A native zero before hiding must not be captured as the original value.
    s_target.sun_brightness = SkyScalar{};
    native_brightness = 0;
    s_settings.disc_visible = false;
    UpdateDiscVisibility();
    assert(!s_target.sun_brightness.has_original);
    assert(!s_target.sun_brightness.modified);
    s_settings.disc_visible = true;
    UpdateDiscVisibility();
    assert(native_brightness == 0 && !s_target.sun_brightness.modified);

    // If native gameplay already reestablished a nonzero value, never override it.
    native_brightness = 4.0f;
    s_settings.disc_visible = false;
    UpdateDiscVisibility();
    native_brightness = 3.0f; // Native day/night update while our override is active.
    s_settings.disc_visible = true;
    UpdateDiscVisibility();
    assert(native_brightness == 3.0f && !s_target.sun_brightness.modified);
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory) / 'disc_test.cpp'
    exe = Path(directory) / 'disc_test'
    path.write_text(harness, encoding='utf-8')
    subprocess.run(['g++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                    '-O2', str(path), '-o', str(exe)], check=True, timeout=25)
    subprocess.run([str(exe)], check=True, timeout=10)
print('Native sky-disc OFF/ON restoration, retry, and zero-snapshot checks passed')

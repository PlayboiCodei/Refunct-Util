"""Static startup-gating regression checks (not a substitute for an x86 build)."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
bootstrap = (root / "src/bootstrap.cpp").read_text(encoding="utf-8")
compat = (root / "src/compatibility.cpp").read_text(encoding="utf-8")
cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
header = (root / "include/refunct/features/compatibility.hpp").read_text(encoding="utf-8")

# No module function pointer or MinHook may be set up before the fingerprint is checked.
assert bootstrap.index("if (!ValidateGameExecutable(game_path))") < bootstrap.index("const uintptr_t function_rvas[]")
assert bootstrap.index("if (!ValidateGameExecutable(game_path))") < bootstrap.index("g_draw_text =")
assert bootstrap.index("if (!ValidateGameExecutable(game_path))") < bootstrap.index("MH_Initialize()")
assert "_wcsicmp(game_filename" in bootstrap
assert 'diagnostic_mode == 2' in bootstrap

# Entire executable, robust hashing and fail-closed mismatch.
assert "BCRYPT_SHA256_ALGORITHM" in compat
assert "BCryptHashData" in compat and "BCryptFinishHash" in compat
assert "CREATE_NEW" in compat and "PinCreateStatus::Failed" in compat
assert "stored != current" in compat
assert "COMPATIBILITY ERROR: game executable changed; ALL hooks disabled" in compat
assert "kKnownCompatibleRefunctExeSha256" in header
assert "kKnownCompatibleRefunctExeSha256" in compat
assert "src/compatibility.cpp" in cmake and "minhook refunct_pure bcrypt" in cmake
print("PASS: fingerprint gate, failure paths and optional release pin are wired before hooks")

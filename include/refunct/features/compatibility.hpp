#pragma once

namespace refunct {
// Optional release-grade protection. Set this to the 64-character lowercase
// SHA-256 of a verified, working Refunct-Win32-Shipping.exe before shipping
// a public release. This release is pinned to the supplied, runtime-log-matched EXE.
inline constexpr char kKnownCompatibleRefunctExeSha256[] = "f9145dc602580414a81c56ad9c23b618e1ee164a2fb9911f53399c67711027de";

// Fingerprint the *file on disk*, before resolving any game-specific address.
// The first known-working game executable is pinned next to the ASI. If the
// executable changes, all Refunct hooks are disabled for that launch.
// Note: first-run pinning is trust-on-first-use, not proof of compatibility.
bool ValidateGameExecutable(const wchar_t* executable_path);
} // namespace refunct

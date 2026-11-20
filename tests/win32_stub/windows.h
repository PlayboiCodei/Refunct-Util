#pragma once
#include <cstdint>
using LONGLONG = long long;
struct LARGE_INTEGER { LONGLONG QuadPart = 0; };
// Test-only fakes: model a 1 MHz performance counter without Win32 SDK.
inline LONGLONG& Counter() { static LONGLONG c = 0; return c; }
inline int QueryPerformanceFrequency(LARGE_INTEGER* f) { f->QuadPart=1000000; return 1; }
inline int QueryPerformanceCounter(LARGE_INTEGER* c) { c->QuadPart=Counter(); return 1; }

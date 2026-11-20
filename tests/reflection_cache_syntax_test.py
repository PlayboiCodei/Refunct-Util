"""Portable C++ syntax test of the four freshly introduced reflected-call caches.

This compiles the actual source blocks against minimal ABI test doubles; the
complete ASI still requires the real Windows/x86 SDK and MSVC.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
blocks = []
for name, start, end in (
    ('tubes', 'struct TubeMaterialMethod {', 'void* GetMaterial(void* mesh) {'),
    ('platforms', 'struct PlatformMaterialCall {', 'void* ReadLiftMesh(void* actor) {'),
    ('jumppads', 'struct JumpPadMaterialCall {', 'void* ReadJumpPadMesh(void* actor) {'),
    ('sections', 'struct ButtonMaterialCall {', 'void* ButtonGetMaterial(void* mesh) {'),
    ('sections', 'struct ButtonPropertyCache {', 'bool ButtonFunctionParameter(void* function,'),
):
    code = (root / 'src' / f'{name}.cpp').read_text(encoding='utf-8')
    blocks.append(code[code.index(start):code.index(end, code.index(start))])

stub = r'''
#include <cstdint>
#include <cstddef>
#include <string>
#include <array>
#include <unordered_map>
#include <cstring>
#include <cwchar>
#include <utility>
struct UnrealNameView { int index; };
struct UObjectHeader { void* class_ptr; int name; };
struct UFieldHeader { UObjectHeader object; void* next; };
struct UStructHeader { UObjectHeader object; void* super_struct; void* children; };
using FNameView = std::array<uint32_t, 2>;
static int dummy = 1;
static bool IsReadable(const void* p, size_t) { return p != nullptr; }
static std::wstring UnrealName(int) { return L"ObjectProperty"; }
static int32_t FindClassPropertyOffset(void*, const wchar_t*) { return 4; }
static void* FindReflectedFunction(void*, const wchar_t*) { return &dummy; }
static int32_t Param(void*, const wchar_t*, const wchar_t*, int32_t=96) { return 4; }
static int32_t PlatformParam(void*, const wchar_t*, const wchar_t*, int32_t=96) { return 4; }
static int32_t JumpPadParam(void*, const wchar_t*, const wchar_t*, int32_t=96) { return 4; }
static bool ButtonFunctionParameter(void*, const wchar_t*, const wchar_t*, int32_t& val) { val=4; return true; }
static void fake_event(void*,void*,void*) {}
static void (*g_process_event)(void*,void*,void*) = fake_event;
'''
source = stub + '\n'.join(blocks) + '\nint main() { return 0; }\n'
with tempfile.TemporaryDirectory() as tmp:
    target = Path(tmp) / 'reflected.cpp'
    target.write_text(source, encoding='utf-8')
    p = subprocess.run(['g++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-function', '-fsyntax-only', str(target)],
                       capture_output=True, text=True, timeout=30)
    if p.returncode:
        print(p.stderr[-5000:])
        raise SystemExit(p.returncode)
print('PASS: freshly introduced class/reflective-call caches compile as C++20')

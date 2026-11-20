"""Compiles the actual button-component refresh code against a small UE4 mock.

This checks reflection offsets, the no-guess collision policy, restoration and
failure handling. Gameplay contact still requires Refunct v9.30 Win32 testing.
"""
from pathlib import Path
import subprocess
import tempfile
import shutil

root = Path(__file__).resolve().parents[1]
source = (root / 'src/sections.cpp').read_text(encoding='utf-8')
start = source.index('bool RefreshButtonContactComponent(')
end = source.index('// A visual SetActive/actor collision mismatch', start)
actual_cpp = source[start:end]
assert 'RefreshButtonContactComponent(button, L"Button base", one_based_index)' in actual_cpp
assert 'RefreshButtonContactComponent(button, L"Button", one_based_index)' in actual_cpp
assert 'if (mode == 0 || mode == 2)' in actual_cpp  # Don't invent collision profiles
assert 'write_mode(static_cast<uint8_t>(mode))' in actual_cpp

mock = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <cwchar>
struct UObjectHeader { void* class_ptr = nullptr; };
struct UStructHeader { int fake = 0; };
struct Component { UObjectHeader h{}; int mode = 0; int writes = 0; bool ignore_restoration = false; };
struct Actor { UObjectHeader h{}; Component* base = nullptr; Component* core = nullptr; };
struct SectionPlacementContext { struct ButtonCollision { void* actor = nullptr; void* actor_class = nullptr; }; };
static int cls_token = 0, get_token = 0, set_token = 0;
static bool missing_core = false;
static int logs = 0;
static bool IsReadable(const void* ptr, size_t) { return ptr != nullptr; }
static void* FindReflectedFunction(void*, const wchar_t* name) {
    if (std::wcscmp(name, L"GetCollisionEnabled") == 0) return &get_token;
    if (std::wcscmp(name, L"SetCollisionEnabled") == 0) return &set_token;
    return nullptr;
}
static int FindClassPropertyOffset(void* instance, const wchar_t* name) {
    if (instance == &cls_token) {
        if (std::wcscmp(name, L"Button base") == 0) return offsetof(Actor, base);
        if (std::wcscmp(name, L"Button") == 0) return missing_core ? -1 : offsetof(Actor, core);
        return -1;
    }
    if (instance == &get_token && std::wcscmp(name, L"ReturnValue") == 0) return 0;
    if (instance == &set_token && std::wcscmp(name, L"NewType") == 0) return 0;
    return -1;
}
static void PE(void* obj, void* fn, void* params) {
    auto* c = static_cast<Component*>(obj);
    auto* p = static_cast<uint8_t*>(params);
    if (fn == &get_token) p[0] = static_cast<uint8_t>(c->mode);
    else if (fn == &set_token) {
        ++c->writes;
        if (c->ignore_restoration && p[0] != 0) return;
        c->mode = p[0];
    } else assert(false);
}
static void (*g_process_event)(void*, void*, void*) = PE;
static void Log(const wchar_t*) { ++logs; }
namespace refunct {
'''
main = r'''
}
int main() {
    using namespace refunct;
    Component base{}, core{};
    Actor actor{};
    actor.base = &base;
    actor.core = &core;
    SectionPlacementContext::ButtonCollision button{&actor, &cls_token};
    // QueryAndPhysics and QueryOnly are re-registered and restored precisely.
    base.mode = 3; core.mode = 1;
    assert(RefreshButtonContactComponents(button, 1));
    assert(base.mode == 3 && base.writes == 2);
    assert(core.mode == 1 && core.writes == 2);
    // Never enable a component whose intended collision profile is NoCollision.
    base.mode = 0; base.writes = 0; core.mode = 2; core.writes = 0;
    assert(RefreshButtonContactComponents(button, 2));
    assert(base.mode == 0 && base.writes == 0 && core.mode == 2 && core.writes == 0);
    // Optional component absent: the other one still refreshes.
    missing_core = true; base.mode = 1; base.writes = 0;
    assert(RefreshButtonContactComponents(button, 3));
    assert(base.mode == 1 && base.writes == 2);
    missing_core = false;
    // Setter refused to restore the mode. Failure must not be mistaken for repair.
    base.mode = 3; base.ignore_restoration = true;
    assert(!RefreshButtonContactComponents(button, 4));
    assert(base.mode == 0);
    assert(logs >= 2);
    std::cout << "PASS actual button collision refresh: restore, no-guess, failure" << std::endl;
}
'''
compiler = shutil.which('g++') or shutil.which('clang++')
if not compiler:
    raise SystemExit('SKIP: C++ compiler not installed')
with tempfile.TemporaryDirectory(prefix='refunct_ghost_') as td:
    cpp = Path(td) / 'test.cpp'
    exe = Path(td) / 'test'
    cpp.write_text(mock + actual_cpp + main, encoding='utf-8')
    subprocess.run([compiler, '-std=c++20', '-O1', '-Wall', '-Wextra', '-Werror', str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

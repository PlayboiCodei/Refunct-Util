"""Regress the live game's missing reflected SceneComponent::SetMobility.

The real UE4 code is compiled against synthetic UProperty metadata. Confirm a
fresh deferred static-mesh component can become Movable *without* a reflected
SetMobility function, with strict type/offset/value checks. Game render status
and whether component registration is truly deferred require in-game testing.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
compiler = shutil.which('g++') or shutil.which('clang++')
if not compiler:
    print('SKIP: no portable C++20 compiler')
    raise SystemExit(0)

source = (root / 'src/ghost.cpp').read_text(encoding='utf-8')
spawn = source.split('bool SpawnWorldGhostPart(', 1)[1].split('void PlaceWorldGhost(', 1)[0]
assert 'GhostPrepareDeferredMobility(component)' in spawn
assert spawn.index('GhostPrepareDeferredMobility(component)') < spawn.index('api.finish, args.data()')
assert spawn.index('set_mesh, args.data()') < spawn.index('api.finish, args.data()')
assert 'GhostDeferredMobilityIsMovable(component)' in spawn
assert 'L"SetMobility"' not in spawn

preamble = (root / 'tests/ghost_native_mock_preamble.hpp').read_text(encoding='utf-8')
source = 'namespace refunct {' + source.split('namespace refunct {', 1)[1]
main = r'''
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

int main() {
    using namespace refunct;
    // The Shipping build can omit a UFunction for SetMobility entirely.
    assert(FindReflectedFunction(nullptr, L"SetMobility") == nullptr);
    UObjectHeader byte_kind{};
    byte_kind.name.index = 502;
    UFieldHeader mobility_property{};
    mobility_property.object.name.index = 501;
    mobility_property.object.class_ptr = &byte_kind;
    UStructHeader component_class{};
    component_class.children = &mobility_property;

    struct SyntheticComponent {
        UObjectHeader header;
        std::uint8_t padding[64]{};
        std::uint8_t mobility = 0;
        std::uint8_t sentinel = 0x5A;
    } component{};
    component.header.class_ptr = &component_class;
    g_mock_mobility_offset = static_cast<int>(offsetof(SyntheticComponent, mobility));

    assert(GhostPrepareDeferredMobility(&component));
    assert(component.mobility == 2 && component.sentinel == 0x5A);
    assert(GhostDeferredMobilityIsMovable(&component));

    component.mobility = 1;
    assert(GhostPrepareDeferredMobility(&component));
    assert(component.mobility == 2);

    // Reject malformed/unexpected data without writing anything.
    component.mobility = 9;
    assert(!GhostPrepareDeferredMobility(&component));
    assert(component.mobility == 9);

    component.mobility = 0;
    byte_kind.name.index = 503; // FloatProperty is not mobility's type.
    assert(!GhostPrepareDeferredMobility(&component));
    assert(component.mobility == 0);
    byte_kind.name.index = 502;

    g_mock_mobility_offset = -1;
    assert(!GhostPrepareDeferredMobility(&component));
    assert(component.mobility == 0);

    g_mock_mobility_offset = static_cast<int>(offsetof(SyntheticComponent, mobility));
    component_class.children = nullptr;
    assert(!GhostPrepareDeferredMobility(&component));
    assert(component.mobility == 0);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='ghost_deferred_mobility_') as directory:
    unit = Path(directory) / 'mobility_mock.cpp'
    unit.write_text(preamble + source + main, encoding='utf-8')
    exe = Path(directory) / 'mobility_mock'
    subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror', '-O2',
                    '-I', str(root / 'include'), str(unit), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('PASS: validated deferred ghost Mobility property works without reflected SetMobility')

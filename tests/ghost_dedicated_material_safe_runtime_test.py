"""Native mock: dedicated material lookup is exact, bounded, and fail-safe.

This does NOT claim UnrealPak mounting or material rendering works in-game.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
compiler = shutil.which('g++') or shutil.which('clang++')
if compiler is None:
    print('SKIP: portable C++20 compiler unavailable')
    raise SystemExit(0)

source = (root / 'src' / 'ghost.cpp').read_text(encoding='utf-8')
prelude = (root / 'tests' / 'ghost_native_mock_preamble.hpp').read_text(encoding='utf-8')
assert 'ApplyDedicatedGhostMaterial();' in source
assert 's_dedicated_ghost' in source
assert 'M_GhostTranslucent' in source
assert 'GetMaterial' in source and 'SetMaterial' in source
assert 'GhostMaterialParameter(' not in source
assert 'ScalarParameterValues' not in source
assert 'VectorParameterValues' not in source
assert 'DiscoverGhostAppearance(' not in source
assert 'GhostMaterialBlendMode(' not in source
assert 'keeping stable opaque ghost' in source
assert 'kBatchLimit = 256' in source

prelude = prelude.replace(
    '    if (name.index == 501) return L"Mobility";',
    '    if (name.index == 610) return L"M_GhostTranslucent";\n'
    '    if (name.index == 611) return L"SomeOtherMaterial";\n'
    '    if (name.index == 612) return L"Material";\n'
    '    if (name.index == 501) return L"Mobility";'
)
source = 'namespace refunct {' + source.split('namespace refunct {', 1)[1]
main = r'''
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
int main() {
    using namespace refunct;
    UStructHeader material_class{};
    material_class.field.object.name.index = 612; // Exact 'Material' class.
    UObjectHeader candidate{};
    candidate.class_ptr = &material_class;
    candidate.name.index = 611;
    candidate.index = 0;
    GlobalObjectItem slots[1]{};
    slots[0].object = &candidate;
    GlobalObjectArray objects{};
    objects.objects = slots;
    objects.num_elements = 1;
    objects.max_elements = 1;
    g_game_module = &objects;
    s_dedicated_ghost = {};
    assert(ResolveDedicatedGhostMaterial() == nullptr);
    assert(s_dedicated_ghost.asset == nullptr);

    candidate.name.index = 610;
    s_dedicated_ghost.scan_cursor = 0;
    s_dedicated_ghost.next_scan_ms = 0;
    assert(ResolveDedicatedGhostMaterial() == &candidate);
    assert(ResolveDedicatedGhostMaterial() == &candidate);

    slots[0].object = nullptr;  // Asset was garbage-collected.
    assert(ResolveDedicatedGhostMaterial() == nullptr);
    assert(s_dedicated_ghost.asset == nullptr);
    g_game_module = nullptr;
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='ghost_dedicated_material_') as temp:
    unit = Path(temp) / 'mock.cpp'
    binary = Path(temp) / 'mock'
    unit.write_text(prelude + source + main, encoding='utf-8')
    subprocess.run([compiler, '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-I', str(root / 'include'), str(unit), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('PASS: exact material lookup, GC invalidation, opaque fallback, no unsafe scanning')

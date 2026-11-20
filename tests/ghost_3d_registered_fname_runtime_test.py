"""Test ghost 3D FName fallback using actual ghost.cpp and isolated UE4 mocks.

The game log shows that the reflected library is present but conversion failed.
Registered-UObject fallback should produce real FName values without unsafe
material instance memory reads, and the stock material must not be finalized
before a bounded scan has completed.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

root=Path(__file__).resolve().parents[1]
compiler=shutil.which('g++') or shutil.which('clang++')
if not compiler:
    print('SKIP: no C++ compiler')
    raise SystemExit(0)
source=(root/'src/ghost.cpp').read_text(encoding='utf-8')
assert 'GhostDiscoverRegisteredParameterNames' in source
assert 'if (!state.parameter_scan_complete) return;' in source
assert 'GhostParameterNameEquals' in source
assert 'GhostGlassMIDColor(mid, state.color_name)' in source
for unsafe in ('VectorParameterValues','ScalarParameterValues','GhostMaterialBlendMode('):
    assert unsafe not in source
prelude=(root/'tests/ghost_native_mock_preamble.hpp').read_text(encoding='utf-8')
prelude=prelude.replace('    if (name.index == 501) return L"Mobility";', '''
    if (name.index == 810) return L"cOlOr"; // FName spelling/casing may differ
    if (name.index == 811) return L"OPACITY";
    if (name.index == 812) return L"Color2";
    if (name.index == 501) return L"Mobility";''')
source='namespace refunct {' + source.split('namespace refunct {',1)[1]
main=r'''
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
int main() {
    using namespace refunct;
    UObjectHeader u1{},u2{},u3{};
    u1.name.index=810;u2.name.index=811;u3.name.index=812;
    GlobalObjectItem registry_slots[3]{};
    registry_slots[0].object=&u1;
    registry_slots[1].object=&u2;
    registry_slots[2].object=&u3;
    GlobalObjectArray objects{};
    objects.objects=registry_slots;
    objects.num_elements=3;
    objects.max_elements=3;
    g_game_module=&objects;
    s_transparency_3d=true;
    s_ghost_glass={};
    assert(ResolveGhostGlassParameterNames());
    assert(s_ghost_glass.names_ready);
    assert(s_ghost_glass.color_name.index==810);
    assert(s_ghost_glass.opacity_name.index==811);
    assert(s_ghost_glass.color2_name.index==812);
    assert(s_ghost_glass.parameter_scan_complete);

    // Optional Opacity/Color2 names do not block a known Color parameter.
    s_ghost_glass={};
    objects.num_elements=1;
    assert(ResolveGhostGlassParameterNames());
    assert(s_ghost_glass.names_ready);
    assert(s_ghost_glass.color_name.index==810);
    assert(s_ghost_glass.opacity_name.index==0);

    // An entirely missing Color keeps the scan finished and refuses to
    // invent an FName from the cooked PAK's package-local name-map indices.
    s_ghost_glass={};
    registry_slots[0].object=&u2;
    assert(!ResolveGhostGlassParameterNames());
    assert(s_ghost_glass.parameter_scan_complete);
    assert(s_ghost_glass.color_name.index==0);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='ghost_registered_fname_') as td:
    src=Path(td)/'ghost_name.cpp'; exe=Path(td)/'ghost_name'
    src.write_text(prelude+source+main,encoding='utf-8')
    subprocess.run([compiler,'-std=c++20','-O2','-Wall','-Wextra','-Werror',
                    '-I',str(root/'include'),str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('PASS: registered FName fallback and optional opacity/Color2 handling')

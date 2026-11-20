"""Native-mock regression test for both opacity switches and non-colliding 3D mode.

Build the real ghost.cpp against the existing inert Win32/UE4 mock. Checks alpha
values passed to the renderer, independently persistent toggles, and the actor
refresh when 3D opacity changes. It cannot replace a Windows in-game test.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
compiler = shutil.which('g++') or shutil.which('clang++')
if not compiler:
    print('SKIP: C++20 compiler unavailable')
    raise SystemExit(0)
source = (ROOT/'src/ghost.cpp').read_text(encoding='utf-8')
header = (ROOT/'include/refunct/features/ghost.hpp').read_text(encoding='utf-8')
menu = (ROOT/'src/menu.cpp').read_text(encoding='utf-8')
for option in ('Transparent2D', 'Transparent3D', 'PassThroughCamera'):
    assert option in source
for fn in ('IsPBGhost2DTransparencyEnabled', 'SetPBGhost2DTransparencyEnabled',
           'IsPBGhost3DTransparencyEnabled', 'SetPBGhost3DTransparencyEnabled',
           'IsPBGhostPassThroughEnabled', 'SetPBGhostPassThroughEnabled'):
    assert fn in source and fn in header and fn in menu
assert 'L"M_Basic"' in source
assert 'FindLoadedGhostAppearanceMaterial()' in source
assert 'GhostPartOpacity(' in source
assert 'GhostGlassMIDScalar(mid, state.opacity_name,' in source
assert 'GhostGlassMIDColor(mid, state.color_name)' in source
assert 'GhostGlassMIDColor(mid, state.color2_name)' in source
assert 'GhostCallByte(component, L"SetCollisionEnabled", L"NewType", 0)' in source
for unsafe in ('ScalarParameterValues', 'VectorParameterValues', 'GhostMaterialBlendMode('):
    assert unsafe not in source, f'Unsafe general-purpose memory scan: {unsafe}'

preamble = (ROOT/'tests/ghost_native_mock_preamble.hpp').read_text(encoding='utf-8')
source = 'namespace refunct {' + source.split('namespace refunct {',1)[1]
main = r'''
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
static refunct::LinearColor last_color{};
static int draw_count=0;
static void MockDrawRect(void*, refunct::LinearColor color, float, float, float, float) {
    last_color=color;
    ++draw_count;
}
int main() {
    using namespace refunct;
    g_draw_rect=&MockDrawRect;
    assert(IsPBGhost2DTransparencyEnabled());
    assert(IsPBGhost3DTransparencyEnabled());
    assert(IsPBGhostPassThroughEnabled());
    assert(std::abs(GhostPartOpacity(0)-0.62f)<0.0001f);
    assert(std::abs(GhostPartOpacity(2)-0.26f)<0.0001f);
    Rectangle(nullptr,0,0,10,10,0.34f);
    assert(draw_count==1 && std::abs(last_color.a-0.34f)<0.0001f);
    SetPBGhost2DTransparencyEnabled(false);
    assert(!IsPBGhost2DTransparencyEnabled());
    assert(IsPBGhost3DTransparencyEnabled());
    Rectangle(nullptr,0,0,10,10,0.34f);
    assert(draw_count==2 && last_color.a==1.0f);
    assert(last_color.r>0.f && last_color.g>last_color.r && last_color.b>=last_color.g);
    SetPBGhost3DTransparencyEnabled(false);
    assert(!IsPBGhost3DTransparencyEnabled());
    // Off uses M_Basic's compiled BLEND_Opaque shader, not glass opacity 1.
    // The scalar value remains the original translucent preset for ON mode.
    for (size_t i=0; i<6; ++i) assert(GhostPartOpacity(i)<1.0f);
    assert(!s_ghost_glass.names_ready);
    // Switching styles must only replace ghost actors, not recorded playback.
    s_playback.push_back({0,0,0,88,0});
    SetPBGhost3DTransparencyEnabled(true);
    assert(s_playback.size()==1);
    assert(std::abs(GhostPartOpacity(4)-0.34f)<0.0001f);
    SetPBGhostPassThroughEnabled(false);
    assert(!IsPBGhostPassThroughEnabled());
    assert(!IsPBGhost2DTransparencyEnabled());
    SetPBGhost2DTransparencyEnabled(true);
    assert(IsPBGhost2DTransparencyEnabled());
    Rectangle(nullptr,0,0,10,10,0.34f);
    assert(std::abs(last_color.a-0.34f)<0.0001f);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='ghost_styles_') as tmp:
    path = Path(tmp)/'ghost_styles.cpp'
    binary = Path(tmp)/'ghost_styles'
    path.write_text(preamble+source+main,encoding='utf-8')
    subprocess.run([compiler,'-std=c++20','-O2','-Wall','-Wextra','-Werror',
                    '-I',str(ROOT/'include'),str(path),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
print('PASS: independent 2D/3D transparency, solid cyan opacity, no replay mutation')

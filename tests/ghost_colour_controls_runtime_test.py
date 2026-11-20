"""Real ghost.cpp C++ mock: persistent 24-bit RGB controls 2D and 3D.

Also verifies menu HEX/picker routing and avoids unsafe material introspection.
Win32 UI, actual shader output and in-game behaviour are not simulated.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
compiler = shutil.which('g++') or shutil.which('clang++')
if not compiler:
    print('SKIP: C++20 compiler unavailable')
    raise SystemExit(0)

source = (root/'src/ghost.cpp').read_text(encoding='utf-8')
header = (root/'include/refunct/features/ghost.hpp').read_text(encoding='utf-8')
menu = (root/'src/menu.cpp').read_text(encoding='utf-8')
assert 'L"ColourRGB"' in source
assert 'GhostColourLinear()' in source
assert 'const LinearColor tint = GhostColourLinear();' in source
assert 'if (s_world_parts.count) DestroyWorldGhostActors();' in source
for function in ('GetPBGhostColourRgb', 'SetPBGhostColourRgb'):
    assert function in source and function in header and function in menu
assert 'GhostColourMethodLabel(),' in menu
assert 'case 4: s_ghost_use_windows_picker = !s_ghost_use_windows_picker;' in menu
assert 'else if (i == 4) OpenGhostColourMethod();' in menu
assert 'else if (s_tile_hex_axis == 18) SetPBGhostColourRgb(hex);' in menu
assert 's_hud_picker_target == kGhostColourPickerTarget' in menu
assert 'SetPBGhostColourRgb(result);' in menu
for unsafe in ('ScalarParameterValues', 'VectorParameterValues', 'GhostMaterialBlendMode('):
    assert unsafe not in source

prelude = (root/'tests/ghost_native_mock_preamble.hpp').read_text(encoding='utf-8')
source = 'namespace refunct {' + source.split('namespace refunct {',1)[1]
main = r'''
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
static refunct::LinearColor seen{};
static int drawn=0;
static void Draw(void*, refunct::LinearColor c, float,float,float,float) {
    seen=c;
    ++drawn;
}
static bool approx(float a, float b) { return std::fabs(a-b) < 0.0001f; }
int main() {
    using namespace refunct;
    g_draw_rect=&Draw;
    assert(GetPBGhostColourRgb()==kDefaultGhostColour);
    Rectangle(nullptr,0,0,10,10,0.42f);
    assert(drawn==1 && approx(seen.r,69.f/255.f));
    assert(approx(seen.g,235.f/255.f) && approx(seen.b,1.f));
    assert(approx(seen.a,0.42f));
    s_playback.push_back({0,1,2,88,45});
    SetPBGhostColourRgb(0xCC6611u);
    assert(GetPBGhostColourRgb()==0xCC6611u);
    assert(s_playback.size()==1);
    Rectangle(nullptr,0,0,10,10,0.42f);
    assert(approx(seen.r,204.f/255.f));
    assert(approx(seen.g,102.f/255.f));
    assert(approx(seen.b,17.f/255.f));
    assert(approx(seen.a,0.42f));
    const auto linear=GhostColourLinear();
    assert(approx(linear.r,seen.r) && approx(linear.g,seen.g) && approx(linear.b,seen.b));
    SetPBGhost2DTransparencyEnabled(false);
    Rectangle(nullptr,0,0,10,10,0.42f);
    assert(approx(seen.a,1.f) && approx(seen.r,204.f/255.f));
    SetPBGhost3DTransparencyEnabled(false);
    assert(GetPBGhostColourRgb()==0xCC6611u);
    assert(s_playback.size()==1);
    SetPBGhostColourRgb(0x1000000u); // out of RRGGBB range
    assert(GetPBGhostColourRgb()==0xCC6611u);
    SetPBGhostColourRgb(0x000000u); // black must be representable
    assert(GetPBGhostColourRgb()==0);
    Rectangle(nullptr,0,0,10,10,0.42f);
    assert(seen.r==0 && seen.g==0 && seen.b==0 && seen.a==1);
    SetPBGhostColourRgb(0xFFFFFFu);
    Rectangle(nullptr,0,0,10,10,0.42f);
    assert(seen.r==1 && seen.g==1 && seen.b==1 && seen.a==1);
    assert(s_playback.size()==1);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='ghost_rgb_') as tmp:
    unit = Path(tmp)/'ghost_rgb.cpp'
    exe = Path(tmp)/'ghost_rgb'
    unit.write_text(prelude+source+main,encoding='utf-8')
    subprocess.run([compiler,'-std=c++20','-O2','-Wall','-Wextra','-Werror',
                    '-I',str(root/'include'),str(unit),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('PASS: shared RGB, opacity independence, unchanged replay, HEX and picker routing')

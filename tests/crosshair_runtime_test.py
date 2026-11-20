"""Exercise production crosshair geometry and menu controls without Unreal."""
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
menu = (root / 'src/menu.cpp').read_text()
state = (root / 'include/refunct/state.hpp').read_text()
hud = (root / 'src/hud.cpp').read_text()
profiles = (root / 'src/customisation_profiles.cpp').read_text()
storage = (root / 'src/storage.cpp').read_text()
for field in ('Style','Size','Thickness','Gap','Opacity','Rgb'):
    assert f'write(L"Crosshair", L"{field}"' in storage
    assert f'crosshair_read(L"{field}"' in storage
    assert f'add("Crosshair{field}"' in profiles
    assert f'values.emplace("Crosshair{field}"' in profiles
    assert f'value("Crosshair{field}")' in profiles
assert 'return ValidCrosshair(CrosshairSettings{' in profiles
assert 'if (g_original_hud) g_original_hud(hud);' in hud
assert 'if (draw_replacement && crosshair_viewport_valid && g_draw_rect)' in hud
assert 'DrawWithNativeReticleSuppressed(native_byte, target.bits,' in hud
assert 'crosshair_read(L"Style", 0, 0, 5)' in storage
assert 'DrawCrosshairGeometry(crosshair, center_x, viewport_height * 0.5f,' in hud
assert 'case Page::CustomisationShadows: case Page::Crosshair:' in menu
assert 'case Page::Crosshair: return CrosshairEntries();' in menu
assert 'raw <= static_cast<int>(Page::Crosshair)' in menu
assert 'else if (s_tile_hex_axis == 19) { g_crosshair.rgb = static_cast<int>(hex); EnableCrosshairEditing(g_crosshair); SaveSettings(); }' in menu
helper = 'void AdjustCrosshairRow(' + menu.split('void AdjustCrosshairRow(',1)[1].split('void AdjustHudTextSize(',1)[0]
page_enum = state.split('enum class Page {',1)[1].split('};',1)[0]
activate_prefix = 'case Page::Crosshair:\n            if (i == 1)'
activate = activate_prefix + menu.split(activate_prefix,1)[1].split('case Page::CustomisationProfiles:',1)[0]
assert 'if (i == 1) OpenCrosshairColour();' in activate
code = r'''
#include "refunct/crosshair.hpp"
#include <array>
#include <cassert>
#include <string>
#include <vector>
using namespace refunct;
int saves=0, back=0, picker=0;
bool s_crosshair_use_windows_picker=false;
void SaveSettings(){++saves;}
std::wstring CrosshairColourLabel(){return L"Colour";}
void OpenCrosshairColour(){++picker;}
void GoBack(){++back;}
''' + helper + '\nenum class Page {' + page_enum + '};\nvoid Enter(int i){switch(Page::Crosshair){' + activate + r'''
default: assert(false);}}
int main(){
    assert(ValidCrosshair(g_crosshair));
    assert(CrosshairEntries().size()==8);
    std::vector<std::array<float,4>> rects;
    auto draw=[&](auto settings){rects.clear();DrawCrosshairGeometry(settings,960,540,[&](float x,float y,float w,float h){rects.push_back({x,y,w,h});});};
    draw(g_crosshair); assert(rects.empty());
    Enter(0); draw(g_crosshair); assert(rects.size()==1);
    assert((rects[0]==std::array<float,4>{956,536,8,8}));
    Enter(0); draw(g_crosshair); assert(rects.size()==4);
    assert((rects[0]==std::array<float,4>{948,539,8,2}));
    assert((rects[1]==std::array<float,4>{964,539,8,2}));
    assert((rects[2]==std::array<float,4>{959,528,2,8}));
    assert((rects[3]==std::array<float,4>{959,544,2,8}));
    Enter(0); draw(g_crosshair); assert(rects.size()==5);
    assert((rects[4]==std::array<float,4>{959,539,2,2}));
    Enter(0); assert(g_crosshair.style==4); draw(g_crosshair); assert(rects.empty());
    Enter(0); assert(g_crosshair.style==5); draw(g_crosshair); assert(!rects.empty());
    Enter(0); assert(g_crosshair.style==0);
    AdjustCrosshairRow(2,1); assert(g_crosshair.style==5 && g_crosshair.size==9);
    draw(g_crosshair); const auto before_thickness=rects;
    AdjustCrosshairRow(3,1); draw(g_crosshair); assert(rects!=before_thickness);
    AdjustCrosshairRow(2,1); draw(g_crosshair); assert(rects!=before_thickness);
    Enter(6); EnableCrosshairEditing(g_crosshair); assert(g_crosshair.style==5);
    g_crosshair.style=1; const int dot_thickness=g_crosshair.thickness; AdjustCrosshairRow(3,1); assert(g_crosshair.thickness==dot_thickness);
    Enter(6); AdjustCrosshairRow(0,-1); assert(g_crosshair.style==5);
    AdjustCrosshairRow(0,-1); assert(g_crosshair.style==4);
    AdjustCrosshairRow(0,-1); assert(g_crosshair.style==3);
    for(int i=0;i<200;++i){for(int row=2;row<=5;++row)AdjustCrosshairRow(row,1);}
    assert(g_crosshair.size==100 && g_crosshair.thickness==20 && g_crosshair.gap==50 && g_crosshair.opacity==100);
    for(int i=0;i<200;++i){for(int row=2;row<=5;++row)AdjustCrosshairRow(row,-1);}
    assert(g_crosshair.size==1 && g_crosshair.thickness==1 && g_crosshair.gap==0 && g_crosshair.opacity==0);
    draw(g_crosshair); assert(rects.empty());
    auto invalid=g_crosshair; invalid.size=101; assert(!ValidCrosshair(invalid)); draw(invalid); assert(rects.empty());
    Enter(1); assert(picker==1); AdjustCrosshairRow(1,1); assert(s_crosshair_use_windows_picker);
    Enter(6); assert(g_crosshair==CrosshairSettings{}); Enter(7); assert(back==1);
    const int before=saves; AdjustCrosshairRow(6,1); AdjustCrosshairRow(7,-1); assert(saves==before);
}
'''
compiler = shutil.which('g++') or shutil.which('clang++')
assert compiler
with tempfile.TemporaryDirectory() as td:
    cpp=Path(td)/'test.cpp'; exe=Path(td)/'test'; cpp.write_text(code)
    subprocess.run([compiler,'-std=c++20','-Wall','-Wextra','-Werror','-I',str(root/'include'),str(cpp),'-o',str(exe)],check=True,timeout=30)
    subprocess.run([str(exe)],check=True,timeout=5)
print('PASS: actual crosshair geometry, menu actions, clamps, reset, profile and persistence wiring')

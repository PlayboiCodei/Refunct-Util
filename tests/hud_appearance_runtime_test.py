"""Compile the actual HUD styling lambdas in an isolated C++20 fixture.

Portable test only: validates colour alpha, per-item scale and layout inputs;
Windows x86 HUD calls and gameplay still require in-game verification.
"""
from pathlib import Path
from tempfile import TemporaryDirectory
import subprocess
import textwrap

root = Path(__file__).resolve().parents[1]
hud = (root / 'src/hud.cpp').read_text('utf-8')
style = hud.split('    const auto styled_color =', 1)[1].split('    if (g_show_timer)', 1)[0]
source = r'''#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>
#include <cassert>
#include <iostream>
struct LinearColor {float r,g,b,a;};
enum class HudElement : size_t {
 Timer, PBRun, SumOfBest, LastButtonSplit, Coordinates, Cubes, Tiles,
 FPS, EnabledModules, PracticeStatus, RecordStatus, Speed, CompareTime,
 BestSegmentTime, PBSplitTime, CompletionPercent, ActiveCheats, ActiveCustomisations,
 GameTime, LocalTime, ButtonNumber, KeyDisplay, Count
};
enum class HudCorner : size_t {TopLeft, Center};
constexpr size_t kHudElementCount=static_cast<size_t>(HudElement::Count);
constexpr size_t kHudPlacementCount=2;
constexpr int kHudColourModeCount=11;
constexpr int kHudMinSizePercent=50;
constexpr int kHudMaxSizePercent=200;
std::array<uint8_t,kHudElementCount> g_hud_colour_modes{};
std::array<uint32_t,kHudElementCount> g_hud_custom_colours{};
std::array<int,kHudElementCount> g_hud_size_percent{};
std::array<HudCorner,kHudElementCount> g_hud_corners{};
std::array<bool,kHudElementCount> g_hud_show_headers{};
int main(){
 struct HudLine {
  std::wstring text;
  float scale, advance;
  LinearColor color;
  bool module_header=false, module_detail=false,warning_background=false; int key_row=-1;
 };
 std::array<std::vector<HudLine>, kHudPlacementCount> corners{};
 for (auto& v : g_hud_size_percent) v=100;
 g_hud_corners[static_cast<size_t>(HudElement::Speed)]=HudCorner::Center;
 g_hud_show_headers[static_cast<size_t>(HudElement::Speed)]=true;
 const auto styled_color = ''' + style + r'''
 const auto approx = [](float a,float b){return std::fabs(a-b)<0.0001f;};
 add_line(HudElement::Timer, L"Timer", 1.15f, 29.0f, {0.43f,0.87f,1.0f,0.6f});
 assert(corners[0].size()==1);
 assert(approx(corners[0][0].scale,1.15f));
 assert(approx(corners[0][0].advance,29));
 assert(approx(corners[0][0].color.g,0.87f));
 g_hud_colour_modes[0]=10;
 g_hud_custom_colours[0]=0xFF0080;
 g_hud_size_percent[0]=150;
 add_line(HudElement::Timer, L"Timer", 1.15f, 29.0f, {0.2f,0.3f,0.4f,0.5f});
 assert(approx(corners[0][1].scale,1.725f));
 assert(approx(corners[0][1].advance,43.5f));
 assert(approx(corners[0][1].color.r,1.0f));
 assert(approx(corners[0][1].color.g,0.0f));
 assert(approx(corners[0][1].color.b,128.0f/255.0f));
 assert(approx(corners[0][1].color.a,0.5f));
 g_hud_colour_modes[static_cast<size_t>(HudElement::Speed)]=5;
 g_hud_size_percent[static_cast<size_t>(HudElement::Speed)]=50;
 add_line(HudElement::Speed,L"Speed", 1.0f,20.0f,{0.8f,0.9f,0.3f,1.0f}, true,true);
 assert(corners[1].size()==1);
 assert(approx(corners[1][0].advance,10.0f));
 assert(corners[1][0].module_header && corners[1][0].module_detail);
 assert(corners[1][0].color.r > corners[1][0].color.g);
 // Bad or stale sizes are treated as default, not as a zero-sized text row.
 g_hud_size_percent[static_cast<size_t>(HudElement::Speed)]=999;
 add_line(HudElement::Speed,L"Speed",0.9f,18.0f,{1,1,1,1});
 assert(approx(corners[1][1].advance,18.0f));
 std::cout << "PASS: independently coloured and scaled HUD lines; default and alpha intact\n";
}
'''
with TemporaryDirectory(prefix='hud_colour_test_') as td:
    source_path=Path(td)/'test.cpp'
    executable=Path(td)/'test'
    source_path.write_text(source)
    compile_process=subprocess.run(['g++','-std=c++20','-Wall','-Wextra','-Werror',str(source_path),'-o',str(executable)],capture_output=True,text=True,timeout=25)
    assert compile_process.returncode==0, compile_process.stderr
    result=subprocess.run([str(executable)],capture_output=True,text=True,timeout=5)
    assert result.returncode==0, result.stdout+result.stderr
    print(result.stdout.strip())

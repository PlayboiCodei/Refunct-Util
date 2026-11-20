"""Compile the real persistent-HUD styling/header lambda with a portable fixture.

This validates label stripping and the headerless multline lists, not Win32 drawing.
"""
from pathlib import Path
from tempfile import TemporaryDirectory
import subprocess

root=Path(__file__).resolve().parents[1]
hud=(root/'src/hud.cpp').read_text('utf-8')
fragment=hud.split('    const auto styled_color =',1)[1].split('    if (g_show_timer)',1)[0]
source = r'''#include <algorithm>
#include <array>
#include <cstdint>
#include <cstddef>
#include <iostream>
#include <string>
#include <utility>
#include <vector>
#include <cassert>
struct LinearColor {float r,g,b,a;};
enum class HudElement : size_t {
  Timer, PBRun, SumOfBest, LastButtonSplit, Coordinates, Cubes, Tiles,
  FPS, EnabledModules, PracticeStatus, RecordStatus, Speed, CompareTime,
  BestSegmentTime, PBSplitTime, CompletionPercent, ActiveCheats, ActiveCustomisations,
  GameTime, LocalTime, ButtonNumber, KeyDisplay, Count
};
enum class HudCorner : size_t {TopLeft,Center};
constexpr size_t kHudElementCount=static_cast<size_t>(HudElement::Count);
constexpr size_t kHudPlacementCount=2;
constexpr int kHudColourModeCount=11;
constexpr int kHudMinSizePercent=50, kHudMaxSizePercent=200;
std::array<uint8_t,kHudElementCount> g_hud_colour_modes{};
std::array<uint32_t,kHudElementCount> g_hud_custom_colours{};
std::array<int,kHudElementCount> g_hud_size_percent{};
std::array<bool,kHudElementCount> g_hud_show_headers{};
std::array<HudCorner,kHudElementCount> g_hud_corners{};
int main() {
 struct HudLine { std::wstring text; float scale,advance; LinearColor color;
   bool module_header=false,module_detail=false,warning_background=false; int key_row=-1; };
 std::array<std::vector<HudLine>, kHudPlacementCount> corners{};
 for (auto& size : g_hud_size_percent) size=100;
 const auto styled_color = ''' + fragment + r'''
 add_line(HudElement::Timer,L"00:30.125",1.0f,20.0f,{1,1,1,1});
 assert(corners[0].back().text == L"00:30.125");
 g_hud_show_headers[0]=true;
 add_line(HudElement::Timer,L"00:30.125",1.0f,20.0f,{1,1,1,1});
 assert(corners[0].back().text == L"Run timer: 00:30.125");
 add_line(HudElement::Cubes,L"Cubes collected: 2/15",1,20,{1,1,1,1});
 assert(corners[0].back().text == L"2/15");
 add_line(HudElement::CompareTime,L"Other profile total (12, PB): 02:01.000",1,20,{1,1,1,1});
 assert(corners[0].back().text == L"02:01.000");
 add_line(HudElement::GameTime,L"Game clock: 12:31",1,20,{1,1,1,1});
 assert(corners[0].back().text == L"12:31");
 add_line(HudElement::RecordStatus,L"CHEATS ACTIVE",1,20,{1,1,1,1},false,false,true);
 assert(corners[0].back().text == L"CHEATS ACTIVE");
 g_hud_show_headers[static_cast<size_t>(HudElement::RecordStatus)]=true;
 add_line(HudElement::RecordStatus,L"CHEATS ACTIVE",1,20,{1,1,1,1},false,false,true);
 assert(corners[0].back().text == L"Record status: CHEATS ACTIVE");
 g_hud_show_headers[static_cast<size_t>(HudElement::Coordinates)]=true;
 add_line(HudElement::Coordinates,L"X 2 Y 3 Z 4",1,20,{1,1,1,1});
 assert(corners[0].back().text == L"Position: X 2 Y 3 Z 4");
 auto before=corners[0].size();
 add_line(HudElement::EnabledModules,L"Enabled features (2)",1,20,{1,1,1,1},true);
 assert(corners[0].size()==before); // Header OFF
 add_line(HudElement::EnabledModules,L"Flying",1,20,{1,1,1,1},false,true);
 assert(corners[0].size()==before+1);
 assert(corners[0].back().text == L"Flying");
 g_hud_show_headers[static_cast<size_t>(HudElement::EnabledModules)]=true;
 add_line(HudElement::EnabledModules,L"Enabled features (2)",1,20,{1,1,1,1},true);
 assert(corners[0].back().text == L"Enabled features (2)");
 g_hud_size_percent[static_cast<size_t>(HudElement::Speed)]=150;
 add_line(HudElement::Speed,L"Speed: 400 u/s",1,20,{1,1,1,1});
 assert(corners[0].back().text == L"400 u/s");
 assert(corners[0].back().advance == 30.0f);
 std::cout<<"PASS: 22 HUD elements, independent headers, label stripping, values and spacing\n";
}'''
with TemporaryDirectory(prefix='hud_headers_') as tmp:
    file=Path(tmp)/'test.cpp'
    file.write_text(source,'utf-8')
    exe=Path(tmp)/'test'
    compile=subprocess.run(['g++','-std=c++20','-Wall','-Wextra','-Werror',str(file),'-o',str(exe)],capture_output=True,text=True,timeout=25)
    assert compile.returncode==0, compile.stderr
    result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=5)
    assert result.returncode==0, result.stdout + result.stderr
    print(result.stdout.strip())

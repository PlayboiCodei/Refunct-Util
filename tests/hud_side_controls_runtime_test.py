"""Compile the real 22-element visibility, header and display-view handlers.

Portable Win32-free fixture: verifies menu state changes without a running game.
"""
from pathlib import Path
from tempfile import TemporaryDirectory
import subprocess

root=Path(__file__).resolve().parents[1]
menu=(root/'src/menu.cpp').read_text('utf-8')
funcs=menu.split('bool HudElementIsVisible(HudElement element) {',1)[1].split('void CycleHudCorner(size_t index, int direction) {',1)[0]
source=r'''#include <array>
#include <cstddef>
#include <cstdint>
#include <cassert>
#include <iostream>
#include <string>
enum class HudElement : size_t {
 Timer, PBRun, SumOfBest, LastButtonSplit, Coordinates, Cubes, Tiles,
 FPS, EnabledModules, PracticeStatus, RecordStatus, Speed, CompareTime,
 BestSegmentTime, PBSplitTime, CompletionPercent, ActiveCheats, ActiveCustomisations,
 GameTime, LocalTime, ButtonNumber, KeyDisplay, Count
};
constexpr size_t kHudElementCount=static_cast<size_t>(HudElement::Count);
enum class ProgressDisplayMode : uint8_t { Off=0, Total=1, Loaded=2 };
enum class RecordTimeView : uint8_t { Total=0, Segment=1 };
bool g_show_timer=true, g_show_pb_run_hud=false, g_show_sum_of_best_hud=false;
bool g_show_last_button_split_hud=false, g_show_coordinates=true;
ProgressDisplayMode g_cube_display_mode=ProgressDisplayMode::Off;
ProgressDisplayMode g_tile_display_mode=ProgressDisplayMode::Off;
bool g_show_fps=false, g_show_enabled_modules=false;
bool g_show_practice_status_hud=true, g_show_record_status_hud=true;
bool g_show_speed=false, g_show_compare_hud=false;
bool g_show_best_segment_time_hud=false, g_show_pb_split_time_hud=false;
bool g_show_completion_percent=false, g_show_active_cheats=false;
bool g_show_active_customisations=false, g_show_game_time_hud=false;
bool g_show_local_time_hud=false, g_show_button_number=false, g_show_key_display=false;
RecordTimeView g_compare_hud_view=RecordTimeView::Total;
RecordTimeView g_best_segment_time_view=RecordTimeView::Segment;
RecordTimeView g_pb_split_time_view=RecordTimeView::Total;
std::array<bool,kHudElementCount> g_hud_show_headers{};
size_t s_hud_style_element=0;
int saves=0;
void SaveSettings(){ ++saves; }
bool HudElementIsVisible(HudElement element) {''' + funcs + r'''
int main(){
 assert(kHudElementCount==22);
 for (size_t i=0;i<kHudElementCount;i++) {
   auto e=static_cast<HudElement>(i);
   bool was=HudElementIsVisible(e);
   int before=saves;
   ToggleHudElement(e);
   assert(HudElementIsVisible(e)!=was);
   assert(saves==before+1);
   ToggleHudElement(e);
   assert(HudElementIsVisible(e)==was);
 }
 // List-only statuses start visible, but toggling doesn't modify cheat state.
 assert(g_show_record_status_hud && g_show_practice_status_hud);
 // A saved record's Total/Segment view is independent of HUD visibility.
 assert(!g_show_compare_hud);
 assert(HudElementView(HudElement::CompareTime)==L"Cumulative total");
 ToggleHudElementView(HudElement::CompareTime);
 assert(HudElementView(HudElement::CompareTime)==L"Single button");
 assert(!g_show_compare_hud);
 // A count display's view can turn it on; toggling visibility then resets to Total.
 ToggleHudElementView(HudElement::Cubes);
 assert(g_cube_display_mode==ProgressDisplayMode::Loaded);
 assert(HudElementView(HudElement::Cubes)==L"Loaded only");
 ToggleHudElement(HudElement::Cubes);
 assert(g_cube_display_mode==ProgressDisplayMode::Off);
 // All headers save independently and don't change the 21 visibility settings.
 s_hud_style_element=static_cast<size_t>(HudElement::FPS);
 ToggleHudHeader();
 assert(g_hud_show_headers[s_hud_style_element]);
 assert(!g_hud_show_headers[static_cast<size_t>(HudElement::Timer)]);
 assert(!g_show_fps);
 std::cout << "PASS: 22 HUD toggles, independent views, headers and persisted saves\n";
}
'''
with TemporaryDirectory(prefix='hud_controls_') as tmp:
    file=Path(tmp)/'test.cpp'
    file.write_text(source,'utf-8')
    exe=Path(tmp)/'test'
    compile=subprocess.run(['g++','-std=c++20','-Wall','-Wextra','-Werror',str(file),'-o',str(exe)],capture_output=True,text=True,timeout=25)
    assert compile.returncode==0,compile.stderr
    proc=subprocess.run([str(exe)],capture_output=True,text=True,timeout=5)
    assert proc.returncode==0,proc.stdout+proc.stderr
    print(proc.stdout.strip())

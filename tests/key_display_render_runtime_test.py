"""Portable C++ harness for the actual Win32 HUD keycap renderer fragment."""
from pathlib import Path
from tempfile import TemporaryDirectory
import subprocess

root = Path(__file__).resolve().parents[1]
hud = (root/'src/hud.cpp').read_text('utf-8')
fragment = hud.split('        const auto render_line = [&](const HudLine& line, float row_y) {',1)[1].split('        size_t rendered_details =',1)[0]
source = r'''#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cwchar>
#include <iostream>
#include <string>
#include <vector>
struct LinearColor {float r,g,b,a;};
using DWORD = unsigned long;
constexpr int VK_SHIFT=0x10, VK_LSHIFT=0xA0, VK_RSHIFT=0xA1, VK_SPACE=0x20;
struct Drawn {std::wstring title;LinearColor color;};
struct Rect {float x,y,w,h;LinearColor color;};
std::vector<Drawn> texts;
std::vector<Rect> rects;
bool foreground=true;
void* GetForegroundWindow(){return reinterpret_cast<void*>(1);}
unsigned GetCurrentProcessId(){return 12;}
unsigned GetWindowThreadProcessId(void*,DWORD* pid){*pid=foreground?12:13;return 1;}
short GetAsyncKeyState(int key){return key=='W'||key==VK_LSHIFT?short(0x8000u):0;}
void DrawText(void*,const std::wstring& t,float,float,float,LinearColor c){texts.push_back({t,c});}
void DrawRect(void*,LinearColor c,float x,float y,float w,float h){rects.push_back({x,y,w,h,c});}
void(*g_draw_rect)(void*,LinearColor,float,float,float,float) = DrawRect;
struct HudLine {std::wstring text;float scale,advance;LinearColor color;
 bool module_header=false,module_detail=false,warning_background=false;
 int key_row=-1;};
int main(){
 void* hud=nullptr;
 constexpr bool at_center=false, at_right=false;
 const float center_x=640;
 const int viewport_width=1280;
 const auto text_width=[](const std::wstring& s,float scale){return 9.0f*float(s.size())*scale;};
 const auto render_line = [&](const HudLine& line, float row_y) {''' + fragment + r'''
 HudLine line{L"key", 1.0f, 32.0f,{0.3f,0.8f,1.0f,1.0f}};
 for(int row=0;row<3;row++){line.key_row=row;render_line(line,10.0f+32.0f*row);}
 assert(texts.size()==6 && rects.size()==12);
 assert(texts[0].title==L"W" && texts[1].title==L"A");
 assert(texts[2].title==L"S" && texts[3].title==L"D");
 assert(texts[4].title==L"SHIFT" && texts[5].title==L"SPACE");
 assert(rects[1].color.b>0.9f); // W held, bright fill
 assert(rects[3].color.b<0.2f); // A idle, dark fill
 assert(rects[9].color.b>0.9f); // SHIFT held via VK_LSHIFT
 assert(rects[11].color.b<0.2f); // SPACE idle
 // Don't track background keystrokes when the game loses focus.
 foreground=false; texts.clear();rects.clear();
 line.key_row=0;render_line(line,10);
 assert(rects.size()==2 && rects[1].color.b<0.2f);
 // Scale keycaps independently and retain alignment of other HUD content.
 foreground=true;rects.clear();texts.clear();
 line.scale=1.5f;render_line(line,10);
 assert(rects[0].w==57.0f);
 line.key_row=-1;line.text=L"Run timer: 00:03";
 render_line(line,20);
 assert(texts.back().title==L"Run timer: 00:03");
 std::cout<<"PASS: keycap geometry, held/idle colours, six keys, focus & scaling\n";
}
'''
with TemporaryDirectory(prefix='refunct_keys_render_') as td:
    path=Path(td)/'test.cpp'; exe=Path(td)/'test'
    path.write_text(source,'utf-8')
    comp=subprocess.run(['g++','-std=c++20','-Wall','-Wextra','-Werror',str(path),'-o',str(exe)],capture_output=True,text=True,timeout=25)
    assert comp.returncode==0,comp.stderr
    run=subprocess.run([str(exe)],capture_output=True,text=True,timeout=5)
    assert run.returncode==0,run.stdout+run.stderr
    print(run.stdout.strip())

"""Use actual cache/monitor routines with deterministic UE/Win32 mocks.
No Windows ABI or live performance claims: verifies semantics and work avoided.
"""
from pathlib import Path
import subprocess, tempfile, shutil
root = Path(__file__).resolve().parents[1]
compiler = shutil.which('g++') or shutil.which('clang++')
assert compiler
reflection = (root/'src/reflection.cpp').read_text().split('\n',1)[1]
performance = (root/'src/performance.cpp').read_text().split('\n',2)[2]
source = (root/'src/hud.cpp').read_text()
width = source[source.index('float HudTextWidth('):source.index('} // namespace',source.index('float HudTextWidth('))]
pre = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>
#include "refunct/discovery_watermark.hpp"
#include "refunct/performance.hpp"
using ULONGLONG = unsigned long long;
struct LARGE_INTEGER { long long QuadPart; };
static long long now_qpc = 1000000;
static int clock_calls = 0;
int QueryPerformanceCounter(LARGE_INTEGER* v) { ++clock_calls; v->QuadPart=now_qpc; return 1; }
int QueryPerformanceFrequency(LARGE_INTEGER* v) {v->QuadPart=1000000;return 1;}
ULONGLONG GetTickCount64() { return now_qpc/1000; }
namespace refunct {
struct FNameView {uint32_t index,number;};
struct FStringView {wchar_t* data;int32_t count,capacity;};
struct UObjectHeader {void* vtable; int32_t flags,index; void* class_ptr; FNameView name; void* outer;};
struct UFieldHeader {UObjectHeader object;void* next;};
struct UStructHeader {UFieldHeader field;void* super_struct;void* children;};
struct GlobalObjectItem {UObjectHeader* object;int32_t flags, cluster_index,serial_number;};
struct GlobalObjectArray {GlobalObjectItem* objects;int32_t num_elements,max_elements;};
struct TunedFloatProperty {float* address=nullptr;float original=0,last_written=0;};
constexpr uintptr_t kGlobalObjectArrayRva=0;
void* g_game_module=nullptr;
int name_calls=0, read_calls=0, width_calls=0;
bool IsReadable(const void* p,size_t n) {++read_calls;return p&&n;}
const wchar_t* Names[] = {L"Class",L"Noise",L"Desired",L"Other",L"Parent"};
void Append(FNameView* n,FStringView* out) {
 ++name_calls;
 if(n->index >= std::size(Names))return;
 std::wstring text=Names[n->index];
 if(n->number) text+=L"_"+std::to_wstring(n->number);
 out->count=out->capacity=static_cast<int>(text.size()+1);
 out->data=new wchar_t[out->count]; std::wcscpy(out->data,text.c_str());
}
void Free(void* p){delete[] static_cast<wchar_t*>(p);}
auto g_fname_append=&Append; auto g_memory_free=&Free;
std::wstring UnrealName(FNameView);
std::atomic_bool g_util_log_enabled{true};
std::vector<std::wstring> messages;
void Log(const wchar_t* text) {messages.emplace_back(text);}
void TextSize(void*,const FStringView* v,float* w,float* h,void*,float scale) {
 ++width_calls; *w=(v->count-1)*10.f*scale; *h=20;
}
auto g_text_size=&TextSize;
}
'''
main=r'''
int main(){
 using namespace refunct;
 // Stable registry: no repeated name conversion on a cache hit.
 UStructHeader klass{}; UFieldHeader noise{}, first{}, second{};
 klass.field.object.name={0,0};klass.field.object.index=0;klass.children=&noise;
 noise.object.name={1,0};noise.object.index=1;noise.next=&first;
 first.object.name={2,0};first.object.index=2;first.object.outer=&klass;
 second.object.name={2,0};second.object.index=3;second.object.outer=&klass;
 GlobalObjectItem slots[]={{&klass.field.object,0,0,11},{&noise.object,0,0,12},
                          {&first.object,0,0,13},{&second.object,0,0,14}};
 GlobalObjectArray objects{slots,4,4};g_game_module=&objects;
 assert(FindCachedClassField(&klass,L"Desired",2048)==&first);
 const auto names=name_calls;
 for(int i=0;i<100;++i) assert(FindCachedClassField(&klass,L"Desired",2048)==&first);
 assert(name_calls==names);
 assert(UnrealName({2,7})==L"Desired_7");assert(UnrealName({2,0})==L"Desired");
 // Missing fields are not permanently cached; changed slot identity forces a walk.
 assert(!FindCachedClassField(&klass,L"Other",2048));
 slots[2].serial_number=99;noise.next=&second;
 assert(FindCachedClassField(&klass,L"Desired",2048)==&second);
 first.object.name={3,0};second.next=&first;
 assert(FindCachedClassField(&klass,L"Other",2048)==&first);
 klass.children=&first;assert(!FindCachedClassField(&klass,L"Desired",2048));
 // Incremental discovery never postpones the full fallback deadline.
 DiscoveryWatermark mark; int registry=1, replaced=2; ULONGLONG due=0;
 assert(mark.Resume(&registry,100,1000,due)==0);
 mark.Complete(&registry,100,1000,30000,due);assert(due==31000);
 assert(mark.Resume(&registry,100,2000,due)==-1);
 assert(mark.Resume(&registry,120,1500,due)==-1);
 assert(mark.Resume(&registry,120,2000,due)==100);
 mark.Complete(&registry,120,2000,30000,due);assert(due==31000);
 assert(mark.Resume(&registry,120,31000,due)==0);
 assert(mark.Resume(&replaced,120,5000,due)==0);
 assert(mark.Resume(&registry,80,5000,due)==0);
 // HUD widths are keyed by text, scale and HUD identity, and expire.
 int hud=1,hud2=2; assert(HudTextWidth(&hud,L"ABC",1)==30);
 assert(HudTextWidth(&hud,L"ABC",1)==30 && width_calls==1);
 assert(HudTextWidth(&hud,L"ABC",2)==60 && width_calls==2);
 assert(HudTextWidth(&hud2,L"ABC",1)==30 && width_calls==3);
 now_qpc+=2100000;assert(HudTextWidth(&hud,L"ABC",1)==30 && width_calls==4);
 // Disabled monitor does not query QPC. Nested timings aggregate over windows.
 g_performance_monitor_enabled=false;
 int before=clock_calls;{PerformanceScope p(PerfArea::Tick);}assert(clock_calls==before);
 g_performance_monitor_enabled=true;
 for(int i=0;i<600;++i){
   {PerformanceScope tick(PerfArea::Tick);
    {PerformanceScope native(PerfArea::NativeTick);now_qpc+=2000;}
    {PerformanceScope water(PerfArea::Water);now_qpc+=100;}
    {PerformanceScope swim(PerfArea::WaterSwimming);now_qpc+=15;}
    {PerformanceScope swim(PerfArea::WaterSwimming);now_qpc+=15;}
    {PerformanceScope hud(PerfArea::Hud);now_qpc+=100;}}
   now_qpc+=7800;
 }
 assert(std::any_of(messages.begin(),messages.end(),[](const auto& s){return s.find(L"Performance Water:")!=std::wstring::npos;}));
 assert(std::any_of(messages.begin(),messages.end(),[](const auto& s){return s.find(L"HUD callback intervals:")!=std::wstring::npos;}));
 assert(std::any_of(messages.begin(),messages.end(),[](const auto& s){return s.find(L"Performance Water swimming (pre/post tick):")!=std::wstring::npos;}));
}
'''
with tempfile.TemporaryDirectory() as d:
    cpp=Path(d)/'performance.cpp'; exe=Path(d)/'performance'
    cpp.write_text(pre+reflection+performance+'\nnamespace refunct {\n'+width+'\n}\n'+main)
    subprocess.run([compiler,'-std=c++20','-O2','-Wall','-Wextra','-Werror','-I',str(root/'include'),str(cpp),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('PASS: real reflection cache, object-slot invalidation, incremental discovery, HUD measurement and profiler')

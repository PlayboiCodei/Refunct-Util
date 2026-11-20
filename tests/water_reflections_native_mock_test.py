"""Compile and exercise the production planar-reflection scan against mock UObjects.

This is not an in-game render test; it validates reversible mutations and scan lifecycle.
"""
import shutil
import subprocess
import tempfile
from pathlib import Path
root=Path(__file__).resolve().parents[1]
source=(root/'src/water.cpp').read_text()
start=source.index('// Apply only to live, map-owned native PlanarReflection components.')
end=source.index('// Only identify effect components by their own type',start)
logic=source[start:end]
compiler=shutil.which('clang++') or shutil.which('g++')
if compiler is None:
    print('SKIP: C++ compiler unavailable')
    raise SystemExit(0)

code=r'''
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <iostream>
using ULONGLONG=unsigned long long;
using int32_t=std::int32_t;
struct MyCharacter{};
struct MockClass {std::wstring name;};
struct UObjectHeader {void* class_ptr; void* outer; int flags;};
struct MockObject {UObjectHeader head; std::wstring name; bool visible=true;};
struct GlobalObjectItem {void* object; int serial_number;};
struct GlobalObjectArray {GlobalObjectItem* objects; int num_elements;};
struct WaterConfigMock {bool reflections_visible=true;bool reflection_geometry_visible=true;} config;
uint32_t WaterReflectionHideMask(){return config.reflection_geometry_visible ? 0u : 1u;}
struct ColourWorkBudget {explicit ColourWorkBudget(int){} bool expired() const{return false;}};
MockClass componentClass{L"PlanarReflectionComponent"}, actorClass{L"PlanarReflection"}, otherClass{L"Cube"};
std::vector<GlobalObjectItem> registry;
GlobalObjectArray globalObjects{};
ULONGLONG now=1000;
int setCount=0;
int getCount=0;
int wakeCount=0;
int registryReads=0;
std::vector<std::wstring> logs;
const GlobalObjectArray* WaterObjects(){++registryReads;globalObjects={registry.data(),(int)registry.size()};return &globalObjects;}
WaterConfigMock& WaterConfig(){return config;}
ULONGLONG GetTickCount64(){return now;}
bool IsReadable(const void* p,size_t){return p!=nullptr;}
bool WaterIsA(void* obj,const wchar_t* name){
    auto* hdr=(UObjectHeader*)obj;
    return ((MockClass*)hdr->class_ptr)->name==name;
}
std::wstring WaterObjectName(void* obj){return ((MockObject*)obj)->name;}
void Log(const wchar_t* text){logs.emplace_back(text);}
bool WaterGetVisible(void* obj,bool& state){getCount++;state=((MockObject*)obj)->visible;return true;}
bool WaterSetVisible(void* obj,bool value){setCount++;((MockObject*)obj)->visible=value;return true;}
bool WaterReadComponentActive(void*, bool& active){active=true;return true;}
void WaterWakePlanarReflection(void*){wakeCount++;}
namespace refunct {
'''+'''\n''' + r'''
struct WaterReflectionTarget {void* component=nullptr;int32_t index=-1;int32_t serial=0;bool original_visible=true;bool modified=false;bool warning_logged=false; bool originally_active=false;bool restored_this_cycle=false;};
std::vector<WaterReflectionTarget> s_reflection_targets;
std::unordered_map<void*,bool> s_reflection_class_cache;
MyCharacter* s_reflection_player=nullptr;
int32_t s_reflection_scan_cursor=-1;
bool s_reflection_scan_complete=false;
bool s_reflection_restore_pending=false;
ULONGLONG s_reflection_retry_after=0;
ULONGLONG s_reflection_next_verify=0;
bool s_reflection_apply_requested=false;
bool s_reflection_missing_logged=false;
constexpr int32_t kReflectionScanBatch=768;
constexpr size_t kMaxReflectionTargets=8;
'''+logic+r'''
}
int main(){
    using namespace refunct;
    MyCharacter player{}, secondPlayer{};
    MockObject owner{{&actorClass,nullptr,0},L"PlanarReflection_2",true};
    MockObject comp{{&componentClass,&owner,0},L"NewPlanarReflectionComponent",true};
    MockObject unrelated{{&otherClass,nullptr,0},L"OtherCube",true};
    MockObject cdo{{&componentClass,&owner,0x10},L"Default__Component",true};
    registry={{&owner,1},{&comp,13},{&unrelated,5},{&cdo,2}};
    config.reflections_visible=false;
    WaterUpdateReflections(&player);
    assert(!comp.visible && unrelated.visible && cdo.visible);
    assert(setCount==1 && s_reflection_targets.size()==1 && s_reflection_scan_complete);
    // Subsequent ticks must reuse cached target and not rescan/rewrite.
    const int settled_reads=registryReads;
    for(int i=0;i<240;++i) WaterUpdateReflections(&player);
    assert(setCount==1 && registryReads==settled_reads);
    now+=1001;
    WaterUpdateReflections(&player);
    assert(setCount==1 && s_reflection_targets.size()==1);
    // Disabling is reversible, without losing the initial visibility state.
    config.reflections_visible=true;
    s_reflection_restore_pending=true;
    s_reflection_apply_requested=true;
    WaterUpdateReflections(&player);
    assert(comp.visible && setCount==2 && !s_reflection_targets[0].modified);
    assert(wakeCount==1);
    config.reflections_visible=false;
    s_reflection_apply_requested=true;
    WaterUpdateReflections(&player);
    assert(!comp.visible && setCount==3);
    // Serial mismatch invalidates the stale capture; never touch another object.
    registry[1].serial_number=14;
    now+=1001;
    WaterUpdateReflections(&player);
    assert(s_reflection_targets.size()==1 && !comp.visible);
    // Switching levels discards old refs and discovers the new native component.
    MockObject nextOwner{{&actorClass,nullptr,0},L"PlanarReflection_3",true};
    MockObject nextComp{{&componentClass,&nextOwner,0},L"NewPlanarReflectionComponent",true};
    registry={{&nextOwner,3},{&nextComp,22},{&unrelated,5}};
    now+=1001;
    WaterUpdateReflections(&secondPlayer);
    assert(!nextComp.visible && s_reflection_targets.size()==1);
    config.reflections_visible=true;
    s_reflection_restore_pending=true;
    s_reflection_apply_requested=true;
    WaterUpdateReflections(&secondPlayer);
    assert(nextComp.visible && wakeCount==2);
    // When first discovered OFF (including a prior toggle before discovery),
    // an explicit ON must override that cached first-observed OFF state.
    MockObject hiddenOwner{{&actorClass,nullptr,0},L"PlanarReflection_4",true};
    MockObject hiddenComp{{&componentClass,&hiddenOwner,0},L"NewPlanarReflectionComponent",false};
    registry={{&hiddenOwner,5},{&hiddenComp,24}};
    config.reflections_visible=false;
    now+=1001;
    WaterUpdateReflections(&player);
    assert(!hiddenComp.visible);
    config.reflections_visible=true;
    s_reflection_restore_pending=true;
    s_reflection_apply_requested=true;
    WaterUpdateReflections(&player);
    assert(hiddenComp.visible && s_reflection_targets[0].restored_this_cycle);
    // A player change during restore must not abandon an ON request.
    MockObject stuckOwner{{&actorClass,nullptr,0},L"PlanarReflection_5",true};
    MockObject stuckComp{{&componentClass,&stuckOwner,0},L"NewPlanarReflectionComponent",false};
    registry={{&stuckOwner,10},{&stuckComp,25}};
    config.reflections_visible=true;
    s_reflection_restore_pending=true;
    now+=1001;
    WaterUpdateReflections(&secondPlayer);
    assert(stuckComp.visible && s_reflection_targets.size()==1);
    // Saved object-exclusion profiles must rediscover the capture on launch,
    // even when the global reflections switch is already ON.
    config.reflection_geometry_visible=false;
    stuckComp.visible=false;
    s_reflection_restore_pending=false;
    s_reflection_targets.clear();
    s_reflection_scan_complete=false;
    s_reflection_scan_cursor=-1;
    now+=1001;
    WaterUpdateReflections(&player);
    assert(stuckComp.visible && s_reflection_targets.size()==1);
    // A live capture may be hidden again by a level transition.
    stuckComp.visible=false;
    now+=1001;
    WaterUpdateReflections(&player);
    assert(stuckComp.visible);
    // Missing capture must retry, even when master reflection is already ON.
    registry={{&stuckOwner,10}};
    now+=1001;
    WaterUpdateReflections(&player);
    assert(s_reflection_targets.empty() && s_reflection_scan_complete);
    const int missing_reads=registryReads;
    for(int i=0;i<240;++i) WaterUpdateReflections(&player);
    assert(registryReads==missing_reads);
    registry.push_back({&stuckComp,25});
    now+=15001;
    WaterUpdateReflections(&player);
    assert(s_reflection_targets.size()==1 && stuckComp.visible);
    // An explicit ON request must survive an empty registry scan.
    registry={{&stuckOwner,10}};
    s_reflection_restore_pending=true;
    s_reflection_apply_requested=true;
    WaterUpdateReflections(&player);
    assert(s_reflection_targets.empty() && s_reflection_restore_pending);
    registry.push_back({&stuckComp,25});
    stuckComp.visible=false;
    now+=15001;
    WaterUpdateReflections(&player);
    assert(stuckComp.visible && !s_reflection_restore_pending);
    config.reflection_geometry_visible=true;
    // When ON by default and never switched OFF, avoid unnecessary discovery.
    s_reflection_restore_pending=false;
    s_reflection_targets.clear();
    s_reflection_scan_complete=false;
    s_reflection_scan_cursor=-1;
    int previousSet=setCount;
    WaterUpdateReflections(&secondPlayer);
    assert(previousSet==setCount && s_reflection_targets.empty());
    std::cout<<"PASS: native-mock reflection toggle, caching, restore, serials, reload\n";
}
'''
with tempfile.TemporaryDirectory() as temporary:
    src=Path(temporary)/'reflection_test.cpp'
    exe=Path(temporary)/'reflection_test'
    src.write_text(code,encoding='utf-8')
    proc=subprocess.run([compiler,'-std=c++20','-O0','-Wall','-Wextra','-Werror',str(src),'-o',str(exe)],capture_output=True,text=True,timeout=50)
    if proc.returncode:print(proc.stderr)
    assert proc.returncode==0, 'mock C++ compilation failed'
    proc=subprocess.run([str(exe)],capture_output=True,text=True,timeout=20)
    if proc.returncode:print(proc.stdout,proc.stderr)
    assert proc.returncode==0, 'mock C++ runtime failed'
    print(proc.stdout.strip())

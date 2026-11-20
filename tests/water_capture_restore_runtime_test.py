"""Compile production planar-capture recovery helpers against a native mock.
Not a Windows/Refunct rendering test: validates reflected calls and fallbacks.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
s = (root / 'src/water.cpp').read_text()
start = s.index('// SetVisibility is insufficient for some UE4 planar capture render states:')
end = s.index('struct WaterCaptureArrayHeader {', start)
logic = s[start:end]
compiler = shutil.which('clang++') or shutil.which('g++')
assert compiler, 'C++ compiler unavailable'
code = r'''
#include <array>
#include <cassert>
#include <cstdint>
#include <cwchar>
#include <string>
#include <vector>
#include <initializer_list>
using int32_t=std::int32_t;
struct UObjectHeader { void* class_ptr=nullptr; };
struct UFieldHeader { UObjectHeader object{}; void* next=nullptr; };
struct UStructHeader { UFieldHeader field{}; void* super_struct=nullptr; void* children=nullptr; };
struct MockComponent { bool visible=true; bool hidden=false; bool active=false; };
std::vector<std::wstring> logs;
void Log(const wchar_t* s) { logs.emplace_back(s); }
bool IsReadable(const void* x, size_t) { return x!=nullptr; }
std::wstring WaterObjectName(void*){ return L""; }
int32_t FindClassPropertyOffset(void*, const wchar_t*) {return -1;}
constexpr uintptr_t kHidden=1, kActive=2, kActivate=3;
void* FindReflectedFunction(void*,const wchar_t* name) {
    const auto kind=std::wstring(name);
    if(kind==L"SetHiddenInGame") return reinterpret_cast<void*>(kHidden);
    if(kind==L"IsActive") return reinterpret_cast<void*>(kActive);
    if(kind==L"Activate") return reinterpret_cast<void*>(kActivate);
    return nullptr;
}
int32_t WaterParam(void* f,const wchar_t* name,const wchar_t*) {
    auto n=std::wstring(name);
    if(f==reinterpret_cast<void*>(kHidden)) {
        if(n==L"bNewHidden")return 0;
        if(n==L"bPropagateToChildren")return 1;
    }
    if(f==reinterpret_cast<void*>(kActive) && n==L"ReturnValue")return 0;
    if(f==reinterpret_cast<void*>(kActivate) && n==L"bReset")return 0;
    return -1;
}
int hiddenCalls=0,activateCalls=0;
void ProcessEvent(void* p,void* f,void* args) {
    auto* c=static_cast<MockComponent*>(p);
    auto* values=static_cast<uint8_t*>(args);
    if(f==reinterpret_cast<void*>(kHidden)) {hiddenCalls++;c->hidden=values[0]!=0;}
    else if(f==reinterpret_cast<void*>(kActive)) values[0]=c->active?1:0;
    else if(f==reinterpret_cast<void*>(kActivate)) {activateCalls++;c->active=true;}
}
void (*g_process_event)(void*,void*,void*)=ProcessEvent;
''' + logic + r'''
int main(){
    MockComponent comp{};
    bool active=false;
    assert(WaterReadComponentActive(&comp,active) && !active);
    comp.hidden=true;
    WaterWakePlanarReflection(&comp);
    assert(!comp.hidden && comp.active && hiddenCalls==1 && activateCalls==1);
    WaterWakePlanarReflection(&comp);
    assert(hiddenCalls==2 && activateCalls==1);
    comp.active=false;
    WaterWakePlanarReflection(&comp);
    assert(hiddenCalls==3 && activateCalls==2 && comp.active);
    return 0;
}
'''
with tempfile.TemporaryDirectory() as temporary:
    src=Path(temporary)/'capture_restore.cpp'
    exe=Path(temporary)/'capture_restore'
    src.write_text(code)
    result=subprocess.run([compiler,'-std=c++20','-Wall','-Wextra','-Werror','-O0',str(src),'-o',str(exe)],capture_output=True,text=True,timeout=30)
    assert result.returncode==0,result.stderr
    result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=10)
    assert result.returncode==0,result.stdout+result.stderr
print('PASS: production reflected capture recovery helpers compile and execute')

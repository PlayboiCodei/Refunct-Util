"""Native C++ harness for the production capture-hidden-list restore path.

Runs on the host with mocked UE4 objects and Win32 memory permissions. It
verifies original empty list restoration, unrelated entries rejection, and
no restoration for unwritable memory. It is not a Win32 or visual test.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/water.cpp').read_text()
start = source.index('bool WaterRestoreGeometryReflections(const GlobalObjectArray* objects) {')
end = source.index('void WaterUpdateGeometryReflections(MyCharacter* player,', start)
code_under_test = source[start:end]
compiler = shutil.which('clang++') or shutil.which('g++')
assert compiler, 'Compiler needed for native test'
code = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>
#include <string>
using int32_t = std::int32_t;
using DWORD = unsigned long;
constexpr int MEM_COMMIT=1, PAGE_GUARD=8, PAGE_NOACCESS=2;
constexpr int PAGE_READWRITE=4, PAGE_WRITECOPY=5;
constexpr int PAGE_EXECUTE_READWRITE=0x40, PAGE_EXECUTE_WRITECOPY=0x80;
struct MEMORY_BASIC_INFORMATION {int State=MEM_COMMIT,Protect=PAGE_READWRITE;};
bool writable=true;
size_t VirtualQuery(const void*, MEMORY_BASIC_INFORMATION* out, size_t n) {
    out->Protect=writable ? PAGE_READWRITE : PAGE_NOACCESS;
    return n;
}
struct WaterCaptureArrayHeader {const std::uint8_t* data=nullptr;int32_t count=0,capacity=0;};
struct CaptureHiddenWeakRef {int32_t index=-1,serial=0;};
struct GlobalObjectItem {void* object=nullptr;int32_t serial_number=0;};
struct GlobalObjectArray {GlobalObjectItem* objects;int32_t num_elements;};
struct WaterConfigMock {bool reflections_visible=true;} config;
WaterConfigMock& WaterConfig(){return config;}
std::vector<CaptureHiddenWeakRef> s_geometry_hidden;
void* s_geometry_capture=nullptr;
int32_t s_geometry_capture_index=-1, s_geometry_capture_serial=0;
struct Capture {std::uint8_t prefix[32]{};WaterCaptureArrayHeader list{};};
Capture capture;
int wake=0,visible=0;
void WaterWakePlanarReflection(void*) {++wake;}
bool WaterSetVisible(void*,bool) {++visible;return true;}
void Log(const wchar_t*) {}
bool WaterGeometryCaptureStillLive(const GlobalObjectArray* objects) {
    return objects && s_geometry_capture && s_geometry_capture_index>=0 &&
      s_geometry_capture_index<objects->num_elements &&
      objects->objects[s_geometry_capture_index].object==s_geometry_capture &&
      objects->objects[s_geometry_capture_index].serial_number==s_geometry_capture_serial;
}
bool WaterReadCaptureHiddenList(void*,int32_t& offset, WaterCaptureArrayHeader& out) {
    offset=32;out=capture.list;return true;
}
bool WaterGeometryHiddenListMatches(const WaterCaptureArrayHeader& list) {
    if(list.count!=static_cast<int32_t>(s_geometry_hidden.size()))return false;
    for(int32_t i=0;i<list.count;++i){
      CaptureHiddenWeakRef weak{};
      std::memcpy(&weak,list.data+sizeof(weak)*i,sizeof(weak));
      if(weak.index!=s_geometry_hidden[i].index||weak.serial!=s_geometry_hidden[i].serial)return false;
    }
    return true;
}
''' + code_under_test + r'''
int main(){
    GlobalObjectItem registry[]={{&capture,91}};
    GlobalObjectArray objects{registry,1};
    s_geometry_capture=&capture;s_geometry_capture_index=0;s_geometry_capture_serial=91;
    CaptureHiddenWeakRef refs[]={{3,5},{7,9}};
    capture.list={reinterpret_cast<const std::uint8_t*>(refs),2,2};
    s_geometry_hidden={refs[0],refs[1]};
    // A foreign edit must never be overwritten.
    refs[1].serial=10;
    assert(!WaterRestoreGeometryReflections(&objects));
    assert(capture.list.count==2 && s_geometry_hidden.size()==2);
    refs[1].serial=9;
    writable=false;
    assert(!WaterRestoreGeometryReflections(&objects));
    assert(capture.list.count==2);
    writable=true;
    assert(WaterRestoreGeometryReflections(&objects));
    assert(capture.list.count==0 && s_geometry_hidden.empty());
    assert(wake==1 && visible==2);
    // All water reflections OFF must not be reenabled by the restore.
    capture.list.count=1;s_geometry_hidden={{3,5}};config.reflections_visible=false;
    assert(WaterRestoreGeometryReflections(&objects));
    assert(capture.list.count==0 && wake==1 && visible==2);
    // A stale capture is never dereferenced or toggled.
    capture.list.count=1;s_geometry_hidden={{3,5}};registry[0].serial_number=92;
    assert(WaterRestoreGeometryReflections(&objects));
    assert(s_geometry_hidden.empty() && wake==1);
    std::cout << "PASS: capture geometry exclusions restored only when owned and writable\n";
}
'''
with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp) / 'test.cpp'
    exe = Path(tmp) / 'test'
    src.write_text(code)
    p = subprocess.run([compiler, '-std=c++20','-Wall','-Wextra','-Werror',str(src),'-o',str(exe)],capture_output=True,text=True)
    assert p.returncode == 0, p.stdout+p.stderr
    p = subprocess.run([str(exe)],capture_output=True,text=True,timeout=15)
    assert p.returncode == 0, p.stdout+p.stderr
    print(p.stdout.strip())

"""Portable compilation/simulation of the real native FixedWaterHeight helpers.

No Windows build or Refunct runtime is implied by this test.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[1] / 'src' / 'water.cpp'

class NativeFixedWaterHeightTest(unittest.TestCase):
    def test_reflected_height_override_and_restoration(self):
        code = SOURCE.read_text(encoding='utf-8')
        start = code.index('int32_t FindNativeFixedWaterlineOffset(MyCharacter* player) {')
        end = code.index('void UpdateRaisedWaterFallback(MyCharacter* player) {', start)
        actual = code[start:end]
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            self.skipTest('no portable C++ compiler available')
        harness = r'''
#include <array>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <string>
#include <iostream>
#include <limits>
using ULONGLONG = unsigned long long;
struct UObjectHeader {void* vtable{}; int32_t flags{};int32_t index{}; void* class_ptr{}; uint64_t name{}; void* outer{};};
struct UFieldHeader { UObjectHeader object; void* next{}; };
struct UStructHeader { UFieldHeader field; void* super_struct{}; void* children{}; };
struct MyCharacter { UObjectHeader object; std::array<uint8_t,512> reserved{};};
struct Item {void* object{}; int32_t flags{};int32_t cluster{};int32_t serial_number{};};
struct GlobalObjectArray {Item* objects{}; int32_t num_elements{};};
struct Config {int height_offset = 0;};
struct NativeFixedWaterline {MyCharacter* player = nullptr; int32_t index = -1; int32_t serial=0; void* klass=nullptr; int32_t offset=-1;float original=0;float last_written=0;bool modified=false;};
NativeFixedWaterline s_fixed_waterline{};
ULONGLONG s_fixed_waterline_retry_after=0;
bool s_fixed_waterline_warned=false, s_fixed_waterline_logged=false;
bool s_fixed_waterline_old_volume_logged=false, s_fixed_waterline_virtual_logged=false;
bool g_restart_requested=false, g_full_level_reload_requested=false, no_swimming=false, have_surface=true;
Config cfg{}; Item items[1]{};GlobalObjectArray array{items,1};
UObjectHeader class_object{}; UStructHeader class_struct{}; UObjectHeader float_class{}, bool_class{};
alignas(16) std::array<uint8_t,128> raw_field{};
UFieldHeader* field=reinterpret_cast<UFieldHeader*>(raw_field.data());
MyCharacter player{};
float* HeightPtr(){return reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(&player)+0x130);}
bool IsReadable(const void* p, size_t){return p!=nullptr;}
bool IsWritable(const void* p, size_t){return p!=nullptr;}
ULONGLONG GetTickCount64(){return 10000;}
int registry_reads=0;
const GlobalObjectArray* WaterObjects(){++registry_reads;return &array;}
Config& WaterConfig(){return cfg;}
bool WaterNoSwimming(){return no_swimming;}
int native_water_volume=0, default_water_volume=0;
void* active_volume=nullptr;
void* SwimCurrentPhysicsVolume(MyCharacter*) { return active_volume; }
bool WaterIsA(void* volume,const wchar_t* cls) {
 if (std::wcscmp(cls,L"DefaultPhysicsVolume")==0) return volume==&default_water_volume;
 if (std::wcscmp(cls,L"PhysicsVolume")==0) return volume==&default_water_volume || volume==&native_water_volume;
 return false;
}
bool RaisedWaterOriginalSurface(const GlobalObjectArray*,float& z){z=-64;return have_surface;}
std::wstring WaterObjectName(void* o){
 if(o==field) return L"FixedWaterHeight";
 if(o==&float_class) return L"FloatProperty";
 if(o==&bool_class) return L"BoolProperty";
 return L"";
}
void Log(const wchar_t*){}
''' + '\n' + actual + r'''
int main(){
 auto require=[](bool ok, const char* msg){if(!ok) { std::cerr << msg << std::endl;std::exit(1); }};
 // The original C++ ABI uses a 32-bit property offset, validated by class metadata.
 std::memcpy(raw_field.data()+0x3C, &std::array<int32_t,1>{0x130}[0], 4);
 field->object.class_ptr=&float_class;
 class_struct.field.object.class_ptr = nullptr;
 class_struct.children=field;
 player.object.class_ptr=&class_struct;
 player.object.index=0;
 items[0]={&player,0,0,13};
 *HeightPtr()=-64;
 for(int i=0;i<480;++i) UpdateNativeFixedWaterline(&player);
 require(registry_reads==0,"default height performed idle registry validation");
 cfg.height_offset=400;
 UpdateNativeFixedWaterline(&player);
 require(std::fabs(*HeightPtr()-336)<0.01f,"height +400 failed");
 require(s_fixed_waterline.modified,"ownership missing");
 cfg.height_offset=700;
 UpdateNativeFixedWaterline(&player);
 require(std::fabs(*HeightPtr()-636)<0.01f,"height +700 failed");
 // The real water volume must use the original waterline during its exit
 // overlap event, otherwise Refunct teleports the player to the raised top.
 active_volume=&native_water_volume;
 UpdateNativeFixedWaterline(&player);
 require(std::fabs(*HeightPtr()+64)<0.01f,"native water exit retained raised waterline");
 require(s_fixed_waterline.modified,"native state should remain owned");
 active_volume=&default_water_volume;
 UpdateNativeFixedWaterline(&player);
 require(std::fabs(*HeightPtr()-636)<0.01f,"entering raised layer failed");
 no_swimming=true;
 UpdateNativeFixedWaterline(&player);
 require(std::fabs(*HeightPtr()+64)<0.01f,"remove water did not restore height");
 require(!s_fixed_waterline.modified,"restore retained modified state");
 no_swimming=false;
 cfg.height_offset=-100;
 UpdateNativeFixedWaterline(&player);
 require(std::fabs(*HeightPtr()+164)<0.01f,"negative height failed");
 cfg.height_offset=0;
 UpdateNativeFixedWaterline(&player);
 require(std::fabs(*HeightPtr()+64)<0.01f,"reset height failed");
 cfg.height_offset=100;
 UpdateNativeFixedWaterline(&player);
 require(std::fabs(*HeightPtr()-36)<0.01f,"new height failed");
 items[0].object=nullptr;items[0].serial_number++; // Stale pointer no longer in the live array.
 cfg.height_offset=200;
 UpdateNativeFixedWaterline(&player);
 require(std::fabs(*HeightPtr()-36)<0.01f,"modified stale actor");
 // Rebind a fresh, live actor and prove a wrong type is rejected.
 s_fixed_waterline={};s_fixed_waterline_retry_after=0;
 field->object.class_ptr=&bool_class;
 *HeightPtr()=-64;
 UpdateNativeFixedWaterline(&player);
 require(std::fabs(*HeightPtr()+64)<0.01f,"wrong type unexpectedly written");
 std::cout<<"native FixedWaterHeight source simulation passed"<<std::endl;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp)/'test.cpp'
            exe = Path(tmp)/'test'
            src.write_text(harness,encoding='utf-8')
            build = subprocess.run([compiler,'-std=c++20','-O1','-Wall','-Wextra',str(src),'-o',str(exe)],capture_output=True,text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(exe)],capture_output=True,text=True)
            self.assertEqual(run.returncode, 0, run.stdout+'\n'+run.stderr)
            self.assertIn('source simulation passed',run.stdout)

if __name__=='__main__':unittest.main()

"""Compile the production reflected-geometry scan/apply/restore code with fake UObjects.
This checks types and state transitions, not the UE4 Win32 ABI or visual effects.
"""
from pathlib import Path
import tempfile, subprocess, shutil
root=Path(__file__).resolve().parents[1]
src=(root/'src/water.cpp').read_text()
code_under_test=src[src.index('// Capture-only wall/lift reflection removal.'):src.index('// Apply only to live, map-owned native PlanarReflection components.')]
compiler=shutil.which('clang++') or shutil.which('g++')
assert compiler
code=r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <iostream>
#include <string>
#include <vector>
using int32_t=std::int32_t;
using ULONGLONG=unsigned long long;
ULONGLONG now_ms=10000;
ULONGLONG GetTickCount64(){return now_ms;}
int layout_reads=0;
using DWORD=unsigned long;
constexpr int MEM_COMMIT=1,PAGE_GUARD=8,PAGE_NOACCESS=2;
constexpr int PAGE_READWRITE=4,PAGE_WRITECOPY=5,PAGE_EXECUTE_READWRITE=0x40,PAGE_EXECUTE_WRITECOPY=0x80;
struct MEMORY_BASIC_INFORMATION {int State=MEM_COMMIT;int Protect=PAGE_READWRITE;};
size_t VirtualQuery(const void*,MEMORY_BASIC_INFORMATION* x,size_t n){*x={};return n;}
struct UObjectHeader{void* vtable{};int flags=0,index=-1;void* class_ptr{};void* outer{};};
struct UFieldHeader{UObjectHeader object;void* next=nullptr;};
struct UStructHeader{UFieldHeader field;void* super_struct=nullptr;void* children=nullptr;};
struct GlobalObjectItem{UObjectHeader* object=nullptr;int serial_number=0;};
struct GlobalObjectArray{GlobalObjectItem* objects=nullptr;int num_elements=0;};
struct WaterCaptureArrayHeader{const std::uint8_t* data=nullptr;int32_t count=0,capacity=0;};
struct WaterReflectionTarget{void* component=nullptr;int32_t index=-1,serial=0;};
struct MyCharacter{};
struct ColourWorkBudget{explicit ColourWorkBudget(int){}bool expired() const{return false;}};
struct Config{
  bool reflection_geometry_visible=false,reflections_visible=true;
  bool reflection_buttons_visible=false,reflection_pipes_visible=false,reflection_beacons_visible=false;
  bool reflection_jump_pads_visible=false,reflection_cubes_visible=false;
} config;
uint32_t WaterReflectionHideMask(){
 return (config.reflection_geometry_visible?0u:1u) |
 (config.reflection_buttons_visible?0u:2u) |
 (config.reflection_pipes_visible?0u:4u) |
 (config.reflection_beacons_visible?0u:8u) |
 (config.reflection_jump_pads_visible?0u:16u) |
 (config.reflection_cubes_visible?0u:32u);
}
Config& WaterConfig(){return config;}
std::vector<WaterReflectionTarget> s_reflection_targets;
struct Capture {UObjectHeader header;char padding[64]{};WaterCaptureArrayHeader hidden{};};
Capture capture;
UObjectHeader worldActor{}, mesh{},buttonActor{},buttonMesh{},buttonBorder{},beacon{},pipeActor{},pipeMesh{},padActor{},padMesh{},cubeActor{},cubeMesh{},otherActor{},otherMesh{};
UStructHeader captureClass{};
UFieldHeader hiddenField{};
UObjectHeader arrayPropertyClass{}, islandClass{}, meshClass{},buttonClass{},pipeClass{},beaconClass{},padClass{},cubeClass{};
std::vector<std::pair<void*,std::wstring>> names;
std::vector<std::pair<std::int32_t,std::int32_t>> weakEntries;
UObjectHeader extraPad{}, extraCube{}, beamRoot{}, attachedBeam{}, assetBeam{}, beamAsset{}, unrelatedParticle{}, wrongOwner{}, templatePad{};
bool alternateComponents=false;
GlobalObjectItem items[24]{};
GlobalObjectArray registry{items,15};
using PE=void(*)(void*,void*,void*);
PE g_process_event=nullptr;
int visCount=0;
std::wstring WaterObjectName(void* obj) {
 for(auto& [p,name]:names)if(p==obj)return name;
 return L"unknown";
}
bool IsReadable(const void* p,size_t size){return p&&size>0;}
bool WaterIsA(void* obj,const wchar_t* cls) {
 const bool staticMesh=obj==&mesh||obj==&buttonMesh||obj==&buttonBorder||obj==&pipeMesh||obj==&padMesh||obj==&cubeMesh||obj==&otherMesh||obj==&extraPad||obj==&wrongOwner||obj==&templatePad;
 return (staticMesh&&(std::wstring(cls)==L"StaticMeshComponent" ||
                     std::wstring(cls)==L"PrimitiveComponent")) ||
        (obj==&extraCube && (std::wstring(cls)==L"MeshComponent" || std::wstring(cls)==L"PrimitiveComponent")) ||
        ((obj==&beacon||obj==&attachedBeam||obj==&assetBeam||obj==&unrelatedParticle) &&
         (std::wstring(cls)==L"PrimitiveComponent"||std::wstring(cls)==L"ParticleSystemComponent")) ||
        (obj==&beamRoot && std::wstring(cls)==L"SceneComponent");
}
int32_t FindClassPropertyOffset(void* cls,const wchar_t* name) {
 ++layout_reads;
 return cls==&captureClass&&std::wstring(name)==L"HiddenComponents"
   ? offsetof(Capture,hidden):-1;
}
void* ReadReflectedObject(void* actor,const wchar_t* prop) {
 if(actor==&padActor && std::wstring(prop)==L"Mover")return alternateComponents?nullptr:&padMesh;
 if(actor==&cubeActor && std::wstring(prop)==L"Mesh")return alternateComponents?nullptr:&cubeMesh;
 if(actor==&attachedBeam && std::wstring(prop)==L"AttachParent")return &beamRoot;
 if(actor==&assetBeam && std::wstring(prop)==L"Template")return &beamAsset;
 if(actor==&otherActor && std::wstring(prop)==L"Mover")return &otherMesh;
 if(actor==&worldActor && std::wstring(prop)==L"BaseMesh")return &mesh;
 if(actor==&buttonActor){
   if(std::wstring(prop)==L"Button") return &buttonMesh;
   if(std::wstring(prop)==L"Button base") return &buttonBorder;
   if(std::wstring(prop)==L"Beacon") return alternateComponents?&beamRoot:&beacon;
 }
 return nullptr;
}
void* FindReflectedFunction(void*,const wchar_t* name) {
 return std::wstring(name)==L"HideComponent" ? reinterpret_cast<void*>(0x1234):nullptr;
}
int32_t WaterParam(void*,const wchar_t* name,const wchar_t*) {
 return std::wstring(name)==L"InComponent"?0:-1;
}
void Log(const wchar_t*) {}
bool WaterSetVisible(void*,bool){++visCount;return true;}
void WaterWakePlanarReflection(void*){}
bool WaterReflectionTargetValid(const WaterReflectionTarget& t,const GlobalObjectArray* objects){
 return objects&&t.index>=0&&t.index<objects->num_elements&&objects->objects[t.index].object==t.component&&objects->objects[t.index].serial_number==t.serial;
}
constexpr int32_t kReflectionScanBatch=768;
''' + r'''
struct CaptureHiddenWeakRef {int32_t index=-1;int32_t serial=0;};
enum class ReflectionKind : uint8_t { Walls=0,Buttons=1,Pipes=2,Beacons=3,JumpPads=4,Cubes=5 };
struct ReflectionGeometryCandidate{void* component=nullptr;int32_t index=-1;int32_t serial=0;ReflectionKind kind=ReflectionKind::Walls;};
uint32_t s_geometry_applied_mask=~0u;
std::vector<ReflectionGeometryCandidate> s_geometry_candidates;
std::vector<CaptureHiddenWeakRef> s_geometry_hidden;
void* s_geometry_capture=nullptr;
int32_t s_geometry_capture_index=-1;
int32_t s_geometry_capture_serial=0;
MyCharacter* s_geometry_player=nullptr;
int32_t s_geometry_scan_cursor=-1;
size_t s_geometry_apply_cursor=0;
bool s_geometry_scan_complete=false;
bool s_geometry_unsupported_logged=false;
bool s_geometry_enabled_logged=false;
ULONGLONG s_geometry_retry_after=0;
constexpr size_t kMaxGeometryCandidates=512;
constexpr size_t kGeometryApplyBatch=4;
''' + code_under_test + r'''
void ProcessEventMock(void*,void*,void* args){
 void* candidate=nullptr;
 std::memcpy(&candidate,args,sizeof(candidate));
 assert(candidate==&mesh||candidate==&buttonMesh||candidate==&buttonBorder||
        candidate==&beacon||candidate==&pipeMesh||candidate==&padMesh||candidate==&cubeMesh||
        candidate==&extraPad||candidate==&extraCube||candidate==&attachedBeam||candidate==&assetBeam);
 auto* obj=static_cast<UObjectHeader*>(candidate);
 // Model UE TArray reuse when Count is reset without freeing its Data.
 weakEntries.resize(capture.hidden.count);
 // UE allocates serial on first weak-reference construction.
 if (items[obj->index].serial_number == 0) items[obj->index].serial_number=100+obj->index;
 weakEntries.emplace_back(obj->index,items[obj->index].serial_number);
 capture.hidden.data=reinterpret_cast<const std::uint8_t*>(weakEntries.data());
 capture.hidden.count=static_cast<int32_t>(weakEntries.size());
 capture.hidden.capacity=static_cast<int32_t>(weakEntries.capacity());
}
int main(){
 static_assert(sizeof(CaptureHiddenWeakRef)==8);
 g_process_event=ProcessEventMock;
 capture.header.class_ptr=&captureClass;capture.header.index=0;
 worldActor.class_ptr=&islandClass;worldActor.index=1;
 mesh.class_ptr=&meshClass;mesh.index=2;
 buttonActor.class_ptr=&buttonClass;buttonActor.index=3;
 buttonMesh.class_ptr=&meshClass;buttonMesh.index=4;
 buttonBorder.class_ptr=&meshClass;buttonBorder.index=5;
 beacon.class_ptr=&beaconClass;beacon.index=6;beacon.outer=&buttonActor;
 pipeActor.class_ptr=&pipeClass;pipeActor.index=7;
 pipeMesh.class_ptr=&meshClass;pipeMesh.index=8;pipeMesh.outer=&pipeActor;
 items[0]={reinterpret_cast<UObjectHeader*>(&capture),10};
 items[1]={&worldActor,20};items[2]={&mesh,30};
 items[3]={&buttonActor,31};items[4]={&buttonMesh,0};items[5]={&buttonBorder,0};
 items[6]={&beacon,0};items[7]={&pipeActor,35};items[8]={&pipeMesh,0};
 padActor.class_ptr=&padClass;padActor.index=9;
 padMesh.class_ptr=&meshClass;padMesh.index=10;padMesh.outer=&padActor;
 cubeActor.class_ptr=&cubeClass;cubeActor.index=11;
 cubeMesh.class_ptr=&meshClass;cubeMesh.index=12;cubeMesh.outer=&cubeActor;
 otherActor.class_ptr=&padClass;otherActor.index=13;
 otherMesh.class_ptr=&meshClass;otherMesh.index=14;otherMesh.outer=&worldActor;
 items[9]={&padActor,0};items[10]={&padMesh,0};items[11]={&cubeActor,0};items[12]={&cubeMesh,0};
 items[13]={&otherActor,0};items[14]={&otherMesh,0};
 captureClass.children=&hiddenField;
 hiddenField.object.class_ptr=&arrayPropertyClass;
 names={{&hiddenField,L"HiddenComponents"}, {&arrayPropertyClass,L"ArrayProperty"},
  {&islandClass,L"BP_IslandChunk_C"},{&meshClass,L"StaticMeshComponent"},
  {&buttonClass,L"BP_Button_C"},{&pipeClass,L"BP_TravelPipe_C"},
  {&beaconClass,L"ParticleSystemComponent"},
  {&worldActor,L"BP_IslandChunk_C_1"},{&buttonActor,L"BP_Button_C_0"},
  {&pipeActor,L"BP_TravelPipe_C_0"},{&pipeMesh,L"PipeMesh_0"},
  {&mesh,L"BaseMesh_0"},{&buttonMesh,L"Button_0"},{&buttonBorder,L"ButtonBase_0"},
  {&beacon,L"Beacon_0"},{&padClass,L"BP_Jumppad_C"},{&cubeClass,L"BP_PowerCore_C"},
  {&padActor,L"BP_Jumppad_0"},{&padMesh,L"Mover"},{&cubeActor,L"BP_PowerCore_0"},
  {&cubeMesh,L"Mesh"},{&otherActor,L"BP_Jumppad_1"},{&otherMesh,L"WrongOwner"}};
 MyCharacter player;
 s_reflection_targets.push_back({&capture,0,10});
 weakEntries.reserve(16);
 WaterUpdateGeometryReflections(&player,&registry);
 WaterUpdateGeometryReflections(&player,&registry);
 assert(s_geometry_candidates.size()==7);
 assert(s_geometry_hidden.size()==7 && capture.hidden.count==7);
 assert(items[4].serial_number==104 && items[8].serial_number==108);
 assert(!s_geometry_unsupported_logged);
 // A capture can be recreated while the player pointer remains unchanged.
 // Model the new capture identity with an empty native list.
 items[0].serial_number=11;
 capture.hidden.count=0;
 s_reflection_targets[0].serial=11;
 WaterUpdateGeometryReflections(&player,&registry);
 WaterUpdateGeometryReflections(&player,&registry);
 assert(s_geometry_hidden.size()==7 && capture.hidden.count==7);
 assert(s_geometry_capture_serial==11);
 // Re-enabling buttons should retain wall, pipe and beacon exclusions.
 config.reflection_buttons_visible=true;
 WaterUpdateGeometryReflections(&player,&registry);
 WaterUpdateGeometryReflections(&player,&registry);
 assert(s_geometry_hidden.size()==5 && capture.hidden.count==5);
 // Re-enable beams too: wall, pipe, jump pad and cube still excluded.
 config.reflection_beacons_visible=true;
 WaterUpdateGeometryReflections(&player,&registry);
 WaterUpdateGeometryReflections(&player,&registry);
 assert(s_geometry_hidden.size()==4 && capture.hidden.count==4);
 config.reflection_pipes_visible=true;
 config.reflection_geometry_visible=true;
 WaterUpdateGeometryReflections(&player,&registry);
 assert(s_geometry_hidden.size()==2 && capture.hidden.count==2);
 assert(s_geometry_hidden[0].index==10 && s_geometry_hidden[1].index==12);
 assert(items[14].serial_number==0); // reject other-owner mesh
 config.reflection_jump_pads_visible=true;
 WaterUpdateGeometryReflections(&player,&registry);
 assert(s_geometry_hidden.size()==1 && s_geometry_hidden[0].index==12);
 config.reflection_jump_pads_visible=false;
 config.reflection_cubes_visible=true;
 WaterUpdateGeometryReflections(&player,&registry);
 assert(s_geometry_hidden.size()==1 && s_geometry_hidden[0].index==10);
 config.reflection_jump_pads_visible=true;
 WaterUpdateGeometryReflections(&player,&registry);
 assert(s_geometry_hidden.empty()&&capture.hidden.count==0);
 // Production regression: a missing named property must not prevent
 // exclusion, and a non-primitive Beacon root can own rendered children.
 alternateComponents=true;
 registry.num_elements=24;
 auto add=[&](UObjectHeader& obj,int idx,void* cls,void* owner,const wchar_t* name){
     obj.index=idx;obj.class_ptr=cls;obj.outer=owner;items[idx]={&obj,0};names.push_back({&obj,name});
 };
 add(extraPad,15,&meshClass,&padActor,L"AdditionalVisual");
 add(extraCube,16,&meshClass,&cubeActor,L"AdditionalCubeVisual");
 add(beamRoot,17,&beaconClass,&buttonActor,L"BeaconRoot");
 add(attachedBeam,18,&beaconClass,&beamRoot,L"GenericParticle");
 add(assetBeam,19,&beaconClass,&buttonActor,L"ParticleSystemComponent_0");
 add(beamAsset,20,&beaconClass,nullptr,L"P_ButtonBeam");
 add(unrelatedParticle,21,&beaconClass,&buttonActor,L"PressSparks");
 add(wrongOwner,22,&meshClass,&worldActor,L"JumppadVisual");
 add(templatePad,23,&meshClass,&padActor,L"TemplatePad");templatePad.flags=0x20;
 config.reflection_jump_pads_visible=false;config.reflection_cubes_visible=false;
 config.reflection_beacons_visible=false;
 for(int i=0;i<4;++i) WaterUpdateGeometryReflections(&player,&registry);
 auto has=[&](int idx){return std::any_of(s_geometry_hidden.begin(),s_geometry_hidden.end(),
     [&](const CaptureHiddenWeakRef& x){return x.index==idx;});};
 assert(s_geometry_hidden.size()==7);
 for(int idx:{6,10,12,15,16,18,19}) assert(has(idx));
 for(int idx:{4,5,14,17,20,21,22,23}) assert(!has(idx));
 const auto prevCalls=visCount;
 for(int i=0;i<20;++i) WaterUpdateGeometryReflections(&player,&registry);
 assert(visCount==prevCalls && s_geometry_hidden.size()==7);
 config.reflection_beacons_visible=true;
 for(int i=0;i<4;++i) WaterUpdateGeometryReflections(&player,&registry);
 assert(s_geometry_hidden.size()==4 && has(10)&&has(12)&&has(15)&&has(16));
 config.reflection_jump_pads_visible=true;config.reflection_cubes_visible=true;
 WaterUpdateGeometryReflections(&player,&registry);
 assert(s_geometry_hidden.empty()&&capture.hidden.count==0);
 // Foreign-owned hidden list must be left untouched, with a bounded retry.
 config.reflection_cubes_visible=false;
 weakEntries={{99,123}};capture.hidden={reinterpret_cast<const uint8_t*>(weakEntries.data()),1,1};
 WaterUpdateGeometryReflections(&player,&registry);
 assert(s_geometry_retry_after==now_ms+1000 && capture.hidden.count==1);
 const int reads=layout_reads;
 for(int i=0;i<240;++i) WaterUpdateGeometryReflections(&player,&registry);
 assert(layout_reads==reads && capture.hidden.count==1);
 now_ms+=1000;
 WaterUpdateGeometryReflections(&player,&registry);
 assert(layout_reads>reads && capture.hidden.count==1);
 // Explicit category edit bypasses retry even within the same second.
 config.reflection_cubes_visible=true;config.reflection_jump_pads_visible=false;
 const int before_edit=layout_reads;
 WaterUpdateGeometryReflections(&player,&registry);
 assert(layout_reads>before_edit && capture.hidden.count==1);
 // New capture serial must bypass the cooldown and allow safe recovery.
 capture.hidden.count=0;items[0].serial_number=77;s_reflection_targets.front().serial=77;
 for(int i=0;i<4;++i) WaterUpdateGeometryReflections(&player,&registry);
 assert(capture.hidden.count==2 && has(10) && has(15));
 std::cout<<"PASS: category exclusions, alternate visual components, beacon children/assets, ownership, idle and restoration\n";
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'mock.cpp';bin=Path(d)/'mock';p.write_text(code)
 r=subprocess.run([compiler,'-std=c++20','-Wall','-Wextra',str(p),'-o',str(bin)],capture_output=True,text=True)
 assert r.returncode==0,r.stdout+r.stderr
 r=subprocess.run([str(bin)],capture_output=True,text=True,timeout=20)
 assert r.returncode==0,r.stdout+r.stderr
 print(r.stdout.strip())

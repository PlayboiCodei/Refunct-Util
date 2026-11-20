"""Compile the real reflected water brush API helpers against native metadata mocks.

Checks enum/byte property verification, SetMobility, scale, bounds, and rejects
missing reflection. Does not prove engine collision in Refunct itself.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'src/water.cpp').read_text(encoding='utf-8')


def extract(signature: str) -> str:
    start = source.index(signature)
    first = source.index('{', start)
    depth = 0
    for idx in range(first, len(source)):
        if source[idx] == '{':
            depth += 1
        elif source[idx] == '}':
            depth -= 1
            if depth == 0:
                return source[start:idx+1]
    raise AssertionError(signature)


symbols = (
    'bool WaterHeightReadMobility(',
    'bool WaterHeightSetMobility(',
    'bool WaterHeightGetScale(',
    'bool WaterHeightSetScale(',
    'bool WaterHeightGetBounds(',
)
body = '\n\n'.join(extract(f) for f in symbols)
mock = r'''
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>
#include <iostream>
struct Vec3 {float x,y,z;};
struct UObjectHeader {void* vtable=nullptr; int32_t flags=0,index=0; void* class_ptr=nullptr; const wchar_t* name=L"";};
struct UFieldHeader {UObjectHeader object; void* next=nullptr;};
struct UStructHeader {UFieldHeader field; void* super_struct=nullptr; void* children=nullptr;};
struct Property {UFieldHeader header; int32_t offset=0;};
struct Root {UObjectHeader header; uint8_t mobility=0; Vec3 scale{1,1,1}; Vec3 position{0,0,-50};};
struct Volume {UObjectHeader header; Root* root=nullptr;};
UObjectHeader byte_class{}, enum_class{};
Property mobility_property{};
UStructHeader root_class{};
Root root{};
Volume volume{};
int fn_mobility=1,fn_get_scale=2,fn_set_scale=3,fn_bounds=4;
bool expose_mobility=true;
bool is_writable=true;
bool IsReadable(const void* ptr,size_t){return ptr!=nullptr;}
std::wstring UnrealName(const wchar_t* name){return name?name:L"";}
std::wstring WaterObjectName(void* object){return object?static_cast<UObjectHeader*>(object)->name:L"";}
int32_t FindClassPropertyOffset(void*, const wchar_t* name) {
    return std::wcscmp(name,L"Mobility")==0 ? static_cast<int32_t>(offsetof(Root,mobility)) : -1;
}
void* FindReflectedFunction(void*, const wchar_t* name){
    if(std::wcscmp(name,L"SetMobility")==0) return expose_mobility?&fn_mobility:nullptr;
    if(std::wcscmp(name,L"K2_GetComponentScale")==0) return &fn_get_scale;
    if(std::wcscmp(name,L"SetWorldScale3D")==0) return &fn_set_scale;
    if(std::wcscmp(name,L"GetActorBounds")==0) return &fn_bounds;
    return nullptr;
}
int32_t WaterParam(void* fn,const wchar_t* name,const wchar_t* type){
    if(!fn)return -1;
    if(fn==&fn_mobility && std::wcscmp(name,L"NewMobility")==0 &&
        std::wcscmp(type,L"ByteProperty")==0) return 0;
    if(fn==&fn_get_scale && std::wcscmp(name,L"ReturnValue")==0) return 0;
    if(fn==&fn_set_scale && std::wcscmp(name,L"NewScale")==0) return 0;
    if(fn==&fn_bounds && std::wcscmp(name,L"Origin")==0) return 0;
    if(fn==&fn_bounds && std::wcscmp(name,L"BoxExtent")==0) return 12;
    return -1;
}
void NativeProcessEvent(void* obj,void* fn,void* args){
    if(fn==&fn_mobility){static_cast<Root*>(obj)->mobility=*static_cast<uint8_t*>(args);}
    else if(fn==&fn_get_scale){std::memcpy(args,&static_cast<Root*>(obj)->scale,sizeof(Vec3));}
    else if(fn==&fn_set_scale){std::memcpy(&static_cast<Root*>(obj)->scale,args,sizeof(Vec3));}
    else if(fn==&fn_bounds){
        auto* v=static_cast<Volume*>(obj);
        Vec3 origin=v->root->position;
        Vec3 extent{100,100,450.f*v->root->scale.z};
        std::memcpy(args,&origin,sizeof(origin));
        std::memcpy(static_cast<uint8_t*>(args)+12,&extent,sizeof(extent));
    }
}
auto g_process_event = &NativeProcessEvent;
void reset(){
    byte_class.name=L"ByteProperty"; enum_class.name=L"EnumProperty";
    mobility_property.header.object.name=L"Mobility";
    mobility_property.header.object.class_ptr=&byte_class;
    mobility_property.offset=offsetof(Root,mobility);
    root_class.children=&mobility_property;
    root.header.class_ptr=&root_class;
    root.mobility=0;root.scale={1,1,1};root.position={0,0,-50};
    volume.root=&root;expose_mobility=true;
}
'''
tests = r'''
int main(){
    reset();uint8_t value=9;
    assert(WaterHeightReadMobility(&root,value) && value==0);
    assert(WaterHeightSetMobility(&root,2));
    assert(WaterHeightReadMobility(&root,value) && value==2);
    assert(WaterHeightSetMobility(&root,0));
    assert(value==2 && root.mobility==0);
    assert(WaterHeightSetScale(&root,{2,3,1.5f}));
    Vec3 scale{};
    assert(WaterHeightGetScale(&root,scale) && scale.z==1.5f);
    float bottom=0,height=0;
    assert(WaterHeightGetBounds(&volume,bottom,height));
    assert(std::fabs(height-1350.f)<0.01f);
    expose_mobility=false;
    assert(!WaterHeightSetMobility(&root,2) && root.mobility==0);
    expose_mobility=true;
    mobility_property.header.object.class_ptr=&enum_class;
    assert(WaterHeightReadMobility(&root,value));
    mobility_property.header.object.class_ptr=nullptr;
    assert(!WaterHeightReadMobility(&root,value));
    assert(!WaterHeightSetMobility(&root,2));
    std::cout<<"PASS: reflected native mobility/scale/bounds helpers and failure guards\n";
}
'''
compiler = shutil.which('g++') or shutil.which('clang++')
if not compiler:
    print('SKIP no available C++ compiler')
else:
    with tempfile.TemporaryDirectory() as temp:
        path=Path(temp)/'test.cpp'
        exe=Path(temp)/'test'
        path.write_text(mock+'\n'+body+'\n'+tests,encoding='utf-8')
        result=subprocess.run([compiler,'-std=c++20','-Wall','-Wextra','-Werror',str(path),'-o',str(exe)],capture_output=True,text=True,timeout=35)
        assert result.returncode==0,result.stderr
        result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=15)
        assert result.returncode==0,result.stderr
        print(result.stdout.strip())

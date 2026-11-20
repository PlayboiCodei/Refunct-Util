"""Compile production native targeting and frame suppression against a mock UE object registry.
The actual decorated game's x86 renderer still requires in-game verification.
"""
from pathlib import Path
import shutil, subprocess, tempfile
root = Path(__file__).resolve().parents[1]
hud = (root/'src/hud.cpp').read_text()
helpers = hud.split('struct ReticleObjectId {',1)[1].split('\n} // namespace',1)[0]
helpers = 'struct ReticleObjectId {' + helpers
frame = hud.split('    bool draw_replacement = false;',1)[1].split('    PerformanceScope perf_hud',1)[0]
frame = '    bool draw_replacement = false;' + frame
code = r'''
#include "refunct/crosshair.hpp"
#include <array>
#include <cassert>
#include <cstring>
#include <string>
#include <unordered_map>
#include <cstdint>
#include <initializer_list>
using namespace refunct;
using ULONGLONG=unsigned long long;
struct UObjectHeader {void* vtable{}; int32_t flags{},index{}; void* class_ptr{}; int name{}; void* outer{};};
struct UFieldHeader {UObjectHeader object; void* next{};};
struct UStructHeader {UFieldHeader field; void* super_struct{}; void* children{};};
struct GlobalObjectItem {void* object{}; int32_t flags{},cluster{},serial_number{};};
struct GlobalObjectArray {GlobalObjectItem* objects{}; int32_t max_elements{},num_elements{};};
GlobalObjectArray registry;
constexpr uintptr_t kGlobalObjectArrayRva=0;
void* g_game_module=&registry;
std::array<GlobalObjectItem,4> slots{};
std::unordered_map<int,std::wstring> names;
std::unordered_map<void*,size_t> memory;
ULONGLONG now=1000;
int logs=0, notices=0, lookups=0;
void Log(const wchar_t*){++logs;}
void Notify(const std::wstring&){++notices;}
ULONGLONG GetTickCount64(){return now;}
bool IsReadable(const void* p,size_t n){if(!p||!n)return false; auto a=reinterpret_cast<uintptr_t>(p);for(const auto& [ptr,size]:memory){auto b=reinterpret_cast<uintptr_t>(ptr);if(a>=b&&a-b<=size&&n<=size-(a-b))return true;}return false;}
bool writable=true;
bool IsWritable(const void* p,size_t n){return writable&&IsReadable(p,n);}
std::wstring UnrealName(int n){return names[n];}
void* property_ptr=nullptr;
int offset=0x100;
void* FindCachedClassField(void*,const wchar_t* name,int){++lookups;return std::wstring(name)==L"ShowReticle"?property_ptr:nullptr;}
int32_t FindClassPropertyOffset(void*,const wchar_t*){return offset;}
''' + helpers + r'''
int original_calls=0, native_draws=0;
uint8_t* original_flag=nullptr; uint8_t native_mask=1;
void Original(void*){++original_calls;if(*original_flag&native_mask)++native_draws;}
void(*g_original_hud)(void*)=Original;
void(*g_draw_rect)()=+[](){};
bool Frame(void* hud,CrosshairSettings crosshair,bool crosshair_viewport_valid){
    const bool custom_crosshair=ValidCrosshair(crosshair)&&crosshair.style!=0;
    if(!custom_crosshair && g_original_hud) g_original_hud(hud);
''' + frame + r'''
    return draw_replacement;
}
int main(){
    UObjectHeader pkg{},bool_type{};
    UStructHeader native{},blueprint{};
    alignas(16) std::array<uint8_t,256> property{};
    alignas(16) std::array<uint8_t,512> object{};
    auto* hud=reinterpret_cast<UObjectHeader*>(object.data());
    auto* field=reinterpret_cast<UObjectHeader*>(property.data());
    names={{1,L"/Script/Refunct"},{2,L"BoolProperty"},{3,L"MyHUD"},{4,L"BP_MyHUD_C"},{5,L"ShowReticle"}};
    pkg.name=1; bool_type.name=2; native.field.object.name=3; native.field.object.outer=&pkg;
    blueprint.field.object.name=4;blueprint.super_struct=&native;
    field->name=5;field->class_ptr=&bool_type;field->outer=&native;
    property_ptr=field;native.children=field;
    hud->class_ptr=&blueprint; hud->index=1;
    slots[1]={hud,0,0,45};registry={slots.data(),4,4};
    memory={{&pkg,sizeof(pkg)},{&bool_type,sizeof(bool_type)},{&native,sizeof(native)},
        {&blueprint,sizeof(blueprint)},{property.data(),property.size()},{object.data(),object.size()},
        {slots.data(),sizeof(slots)},{&registry,sizeof(registry)}};
    auto* flag=object.data()+offset;original_flag=flag;
    property[0x50]=1;property[0x52]=1;property[0x53]=0xff;
    *flag=1; CrosshairSettings settings;
    // Default path: native HUD runs once, no reflection work.
    assert(!Frame(hud,settings,true)); assert(native_draws==1&&original_calls==1&&lookups==0);
    settings.style=2;
    // Native suppression occurs inside the original HUD invocation and is restored.
    assert(Frame(hud,settings,true)); assert(native_draws==1&&original_calls==2&&*flag==1&&lookups==1); assert(g_crosshair_status==CrosshairStatus::NativeFlagCleared);
    assert(Frame(hud,settings,true)); assert(lookups==1&&native_draws==1&&*flag==1);
    // Off and zero opacity suppress but draw no replacement.
    settings.style=4;assert(!Frame(hud,settings,false));assert(native_draws==1&&*flag==1);
    settings.style=1;settings.opacity=0;assert(!Frame(hud,settings,true));assert(native_draws==1&&*flag==1);
    settings.opacity=100;*flag=0;assert(!Frame(hud,settings,true));assert(native_draws==1&&*flag==0);
    *flag=1;assert(!Frame(hud,settings,false));assert(native_draws==2&&*flag==1);
    writable=false;assert(!Frame(hud,settings,true));assert(native_draws==3&&*flag==1);writable=true;
    // A recycled HUD address invalidates cache; ambiguous layout must fail open.
    slots[1].serial_number++;property[0x58]=1;property[0x5a]=2;property[0x5b]=2;
    assert(!Frame(hud,settings,true));assert(native_draws==4&&*flag==1&&notices==1); assert(g_crosshair_status==CrosshairStatus::NativeFlagUnavailable);
    assert(!Frame(hud,settings,true));assert(lookups==2&&notices==1);
    property[0x58]=property[0x5a]=property[0x5b]=0;now+=1001;
    assert(Frame(hud,settings,true));assert(lookups==3&&*flag==1);
    // Wrong declaring class/type and unknown metadata do not write to the HUD.
    slots[1].serial_number++;field->outer=&blueprint;assert(!Frame(hud,settings,true));assert(*flag==1);
    field->outer=&native;slots[1].serial_number++;field->class_ptr=&pkg;assert(!Frame(hud,settings,true));assert(*flag==1);
    field->class_ptr=&bool_type;slots[1].serial_number++;assert(Frame(hud,settings,true));
    // Repeated descriptors for the exact same byte/mask are not a conflict.
    slots[1].serial_number++;property[0x58]=1;property[0x5a]=1;property[0x5b]=0xff;
    assert(Frame(hud,settings,true)); assert(g_crosshair_status==CrosshairStatus::NativeFlagCleared);
    // Game-owned flag stays unchanged, even for bitfields with neighbours.
    const uint8_t packed[]={4,1,4,4};auto bits=DecodeReticleBits(0x100,packed);assert(bits&&bits.offset==0x101&&bits.mask==4);
    uint8_t v=0x85;
    assert(DrawWithNativeReticleSuppressed(&v,bits,[&]{assert(v==0x81);v|=0x10;},[&]{return &v;}));assert(v==0x95);
    // No write after native callback destroys or recycles the HUD.
    v=0x85; assert(!DrawWithNativeReticleSuppressed(&v,bits,[&]{assert(v==0x81);},[&]()->uint8_t*{return nullptr;}));assert(v==0x81);
    // Invalid native bool value is not silently overwritten.
    const uint8_t scalar[]={1,0,1,0xff};bits=DecodeReticleBits(0x100,scalar);v=2;
    assert(!DrawWithNativeReticleSuppressed(&v,bits,[&]{assert(v==2);},[&]{return &v;}));
    const uint8_t malformed[]={4,4,3,3};assert(!DecodeReticleBits(0x100,malformed));
    // Game-default mode never changes native visibility.
    settings.style=0; const auto count=native_draws; assert(!Frame(hud,settings,true));assert(native_draws==count+1&&*flag==1);
}
'''
compiler=shutil.which('g++') or shutil.which('clang++')
assert compiler
with tempfile.TemporaryDirectory() as td:
    cpp=Path(td)/'test.cpp'; exe=Path(td)/'test';cpp.write_text(code)
    subprocess.run([compiler,'-std=c++20','-Wall','-Wextra','-Werror','-I',str(root/'include'),str(cpp),'-o',str(exe)],check=True,timeout=30)
    subprocess.run([str(exe)],check=True,timeout=5)
print('PASS: native ShowReticle lookup, verified replacement gating, restoration, failure fallback, cache identity, off and default')

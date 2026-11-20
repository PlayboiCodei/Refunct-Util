"""Compile the shadow-capture writer with a host-layout mock.

The production path requires the verified 32-bit Refunct ABI (16-byte entries).
The test adapts only entry stride and FString offsets to the 64-bit host for
execution. A Windows x86 build and live scene-render check are still required.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
src = (root / 'src/water.cpp').read_text()
start = src.index('// Toggle only the diagnosed DynamicShadows entry.')
end = src.index('// The actual capture show flags are native UE4 state', start)
logic = src[start:end]
assert 'stride != 16 || struct_size != 16 || name_offset != 0' in logic
assert 'enabled_offset != 12 || sizeof(FStringView) != 12' in logic
assert 'std::wstring_view(name.data, name.count - 1) == L"DynamicShadows"' in logic
assert '*match = enabled ? 1 : 0;' in logic
# The mock compiler is 64-bit; exercise the same data-validation and write
# control flow with the host layout, rather than falsely claiming an x86 test.
logic = logic.replace('stride != 16 || struct_size != 16 || name_offset != 0',
                      'stride != 24 || struct_size != 24 || name_offset != 0')
logic = logic.replace('enabled_offset != 12 || sizeof(FStringView) != 12',
                      'enabled_offset != 16 || sizeof(FStringView) != 16')
compiler = shutil.which('clang++') or shutil.which('g++')
assert compiler
code = r'''
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>
#include <string_view>
using int32_t = std::int32_t;
using DWORD = unsigned long;
constexpr int MEM_COMMIT=1, PAGE_GUARD=8, PAGE_NOACCESS=2;
constexpr int PAGE_READWRITE=4, PAGE_WRITECOPY=5;
constexpr int PAGE_EXECUTE_READWRITE=0x40, PAGE_EXECUTE_WRITECOPY=0x80;
struct MEMORY_BASIC_INFORMATION { int State, Protect; };
bool writable=true;
size_t VirtualQuery(const void*, MEMORY_BASIC_INFORMATION* p, size_t size) {
    p->State=MEM_COMMIT; p->Protect=writable ? PAGE_READWRITE : PAGE_NOACCESS;
    return size;
}
struct FStringView { wchar_t* data; int32_t count,capacity; };
static_assert(sizeof(FStringView)==16);
struct UObjectHeader { void* class_ptr; };
struct UFieldHeader { UObjectHeader object; void* next; };
struct UStructHeader { UFieldHeader field; void* super_struct; void* children; };
struct WaterCaptureArrayHeader { const std::uint8_t* data; int32_t count,capacity; };
struct ScriptStruct { UStructHeader header; int property_size; };
struct Component { UObjectHeader header; std::uint8_t data[256]{}; };
struct ArrayProp { UFieldHeader field; std::uint8_t extra[256]{}; };
void* arrayPropType=nullptr, *structProp=nullptr, *scriptStruct=nullptr;
bool IsReadable(const void* p,size_t n) { return p!=nullptr && n>0; }
std::wstring WaterObjectName(void* p) {
    if(p==arrayPropType)return L"ArrayProperty";
    if(p==structProp)return L"ShowFlagSettings";
    return p ? L"ShowFlagSettings" : L"";
}
void* WaterLocateDescriptorPointer(void*,const wchar_t* kind,const wchar_t*) {
    return std::wstring(kind)==L"StructProperty" ? structProp : scriptStruct;
}
int32_t FindClassPropertyOffset(void* p,const wchar_t* name){
    if(std::wstring(name)==L"ShowFlagSettings")return 32;
    if(p==scriptStruct){
        if(std::wstring(name)==L"ShowFlagName")return 0;
        if(std::wstring(name)==L"Enabled")return 16;
    }
    return -1;
}
''' + logic + r'''
int main() {
    UObjectHeader type{};
    ArrayProp prop{};
    UStructHeader component_class{};
    ScriptStruct script{};
    Component component{};
    component.header.class_ptr=&component_class;
    component_class.children=&prop;
    prop.field.object.class_ptr=&type;
    arrayPropType=&type;
    structProp=&prop;
    scriptStruct=&script;
    const int stride=24;
    script.property_size=stride;
    std::memcpy(reinterpret_cast<std::uint8_t*>(&prop)+0x24,
                &script.property_size,4);
    wchar_t name1[]=L"DynamicShadows",name2[]=L"Fog";
    FStringView flag1{name1,15,15}, flag2{name2,4,4};
    std::array<std::uint8_t,48> entries{};
    std::memcpy(entries.data(),&flag1,sizeof(flag1));
    std::memcpy(entries.data()+stride,&flag2,sizeof(flag2));
    entries[16]=1; entries[stride+16]=1;
    WaterCaptureArrayHeader header{entries.data(),2,2};
    std::memcpy(reinterpret_cast<std::uint8_t*>(&component)+32,&header,sizeof(header));
    assert(WaterSetCaptureDynamicShadows(&component,false));
    assert(entries[16]==0 && entries[stride+16]==1);
    assert(WaterSetCaptureDynamicShadows(&component,true));
    assert(entries[16]==1 && entries[stride+16]==1);
    writable=false;
    assert(!WaterSetCaptureDynamicShadows(&component,false));
    assert(entries[16]==1);
    writable=true;
    entries[16]=3;
    assert(!WaterSetCaptureDynamicShadows(&component,false));
    entries[16]=1;
    flag2={name1,15,15}; // reject ambiguous duplicate DynamicShadows entries
    std::memcpy(entries.data()+stride,&flag2,sizeof(flag2));
    assert(!WaterSetCaptureDynamicShadows(&component,false));
    assert(entries[16]==1);
}
'''
with tempfile.TemporaryDirectory() as tmp:
    source = Path(tmp) / 'test.cpp'
    binary = Path(tmp) / 'test'
    source.write_text(code)
    compile_result = subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror',
                                     str(source), '-o', str(binary)],
                                    capture_output=True, text=True, timeout=40)
    assert compile_result.returncode == 0, compile_result.stderr
    run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
    assert run.returncode == 0, run.stdout + run.stderr
print('PASS: shadow flag enabled/disabled/restore, unrelated flags preserved, invalid memory rejected')

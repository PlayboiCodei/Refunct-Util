"""Compile the real, read-only planar-capture show-flag inspector against mocks.

This validates bounded traversal and data preservation, not Refunct's Windows
x86 ABI or the renderer's native ShowFlags. No game memory is modified.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/water.cpp').read_text(encoding='utf8')
start = source.index('// This inspector reads the reflection capture')
end = source.index('// Toggle only the diagnosed DynamicShadows entry.', start)
body = source[start:end] + source[source.index('void WaterDiagnoseCaptureAPI(void* component)', end):source.index('bool WaterReflectionTargetValid(const WaterReflectionTarget& target,', end)]
compiler = shutil.which('clang++') or shutil.which('g++')
assert compiler, 'native C++ compiler not found'
code = r'''
#include <array>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <initializer_list>
#include <string>
#include <unordered_map>
#include <vector>
using int32_t = std::int32_t;
struct FStringView { wchar_t* data; int32_t count, capacity; };
struct UObjectHeader { void* class_ptr{}; };
struct UFieldHeader { UObjectHeader object; void* next{}; };
struct UStructHeader { UFieldHeader field; void* super_struct{}; void* children{}; };
struct Descriptor { UFieldHeader field{}; std::array<std::uint8_t, 256> space{}; };
struct ScriptStruct { UStructHeader header{}; int32_t property_size{}; };
struct Component { UObjectHeader header{}; char storage[256]{}; };
std::unordered_map<const void*,std::wstring> names;
std::vector<std::wstring> logs;
void Log(const wchar_t* s) { logs.emplace_back(s); }
bool IsReadable(const void* address, size_t count) { return address && count; }
std::wstring WaterObjectName(const void* address) {
    auto it = names.find(address);
    return it == names.end() ? L"unknown" : it->second;
}
void* FindReflectedFunction(void*, const wchar_t*) { return nullptr; }
int32_t FindClassPropertyOffset(void* klass, const wchar_t* name) {
    if (std::wstring(name) == L"ShowFlagSettings") return 32;
    if (names[klass] == L"EngineShowFlagsSetting") {
        if (std::wstring(name) == L"ShowFlagName") return 0;
        if (std::wstring(name) == L"Enabled") return sizeof(FStringView);
    }
    if (std::wstring(name) == L"PrefilterRoughness") return 56;
    return -1;
}
''' + body + r'''
#include <cassert>
int main() {
    UStructHeader componentClass{};
    UFieldHeader ignored{}, roughness{};
    Descriptor arrayDescriptor{}, structDescriptor{};
    UObjectHeader arrayClass{}, floatClass{}, structPropClass{}, scriptStructClass{};
    ScriptStruct script{};
    names[&componentClass] = L"PlanarReflectionComponent";
    names[&ignored] = L"UnrelatedProperty";
    names[&arrayDescriptor] = L"ShowFlagSettings";
    names[&roughness] = L"PrefilterRoughness";
    names[&arrayClass] = L"ArrayProperty";
    names[&floatClass] = L"FloatProperty";
    names[&structPropClass] = L"StructProperty";
    names[&scriptStructClass] = L"ScriptStruct";
    names[&structDescriptor] = L"ShowFlagSettings";
    names[&script] = L"EngineShowFlagsSetting";
    componentClass.children = &ignored;
    ignored.next = &arrayDescriptor;
    arrayDescriptor.field.next = &roughness;
    arrayDescriptor.field.object.class_ptr = &arrayClass;
    roughness.object.class_ptr = &floatClass;
    structDescriptor.field.object.class_ptr = &structPropClass;
    script.header.field.object.class_ptr = &scriptStructClass;

    void* inner = &structDescriptor;
    std::memcpy(reinterpret_cast<char*>(&arrayDescriptor) + 0x60, &inner, sizeof(inner));
    void* scriptPointer = &script;
    std::memcpy(reinterpret_cast<char*>(&structDescriptor) + 0x60, &scriptPointer, sizeof(scriptPointer));
    constexpr int stride = static_cast<int>(sizeof(FStringView)) + 8;
    static_assert(stride <= 64);
    script.property_size = stride;
    std::memcpy(reinterpret_cast<char*>(&structDescriptor) + 0x24, &script.property_size,
                sizeof(script.property_size));

    std::array<std::uint8_t, stride * 2> entries{};
    wchar_t first[] = L"DynamicShadows", second[] = L"Fog";
    FStringView s1{first, 15, 15}, s2{second, 4, 4};
    std::memcpy(entries.data(), &s1, sizeof(s1));
    std::memcpy(entries.data() + stride, &s2, sizeof(s2));
    entries[sizeof(FStringView)] = 1;
    entries[stride + sizeof(FStringView)] = 0;
    const auto original = entries;

    Component component{};
    component.header.class_ptr = &componentClass;
    names[&component] = L"NewPlanarReflectionComponent";
    WaterCaptureArrayHeader header{entries.data(), 2, 2};
    std::memcpy(reinterpret_cast<char*>(&component) + 32, &header, sizeof(header));
    WaterDiagnoseCaptureAPI(&component);
    int showFound = 0, roughFound = 0, countFound = 0;
    bool dynamicFound = false, fogFound = false;
    for (const auto& msg : logs) {
        if (msg.find(L"property: ShowFlagSettings") != std::wstring::npos) ++showFound;
        if (msg.find(L"property: PrefilterRoughness") != std::wstring::npos) ++roughFound;
        if (msg.find(L"array count=2") != std::wstring::npos) ++countFound;
        dynamicFound |= msg.find(L"DynamicShadows raw_enabled=1") != std::wstring::npos;
        fogFound |= msg.find(L"Fog raw_enabled=0") != std::wstring::npos;
    }
    assert(showFound == 1 && roughFound == 1 && countFound == 1);
    assert(dynamicFound && fogFound);
    assert(entries == original);
    assert(logs.size() < 35);

    // Reject malformed FString metadata without dereferencing its data.
    FStringView invalid{nullptr, 1000, 1000};
    std::memcpy(entries.data(), &invalid, sizeof(invalid));
    logs.clear();
    WaterDiagnoseCaptureAPI(&component);
    bool rejected = false;
    for (const auto& msg : logs)
        rejected |= msg.find(L"invalid FString") != std::wstring::npos;
    assert(rejected);

    // A zero-length array is a valid state; no native ShowFlags edited.
    header = WaterCaptureArrayHeader{};
    std::memcpy(reinterpret_cast<char*>(&component) + 32, &header, sizeof(header));
    logs.clear();
    WaterDiagnoseCaptureAPI(&component);
    bool empty = false;
    for (const auto& msg : logs)
        empty |= msg.find(L"array count=0") != std::wstring::npos;
    assert(empty);
    return 0;
}
'''
with tempfile.TemporaryDirectory() as temporary:
    src = Path(temporary) / 'test.cpp'
    exe = Path(temporary) / 'test'
    src.write_text(code)
    build = subprocess.run([compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror',
                            '-O0', str(src), '-o', str(exe)],
                           capture_output=True, text=True, timeout=35)
    assert build.returncode == 0, build.stderr
    run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=12)
    assert run.returncode == 0, run.stdout + run.stderr
print('PASS: verified flag-entry metadata, read-only logging, malformed/empty array cases')

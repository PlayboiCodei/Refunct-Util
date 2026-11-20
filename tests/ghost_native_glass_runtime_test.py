"""Compile real ghost.cpp against reflected UE4 mocks for the known glass material.

Checks the shipping-safe exact-name selector and Blueprint FName/parameter ABI.
This does NOT prove an asset is loaded or rendered by the real game.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
compiler = shutil.which('g++') or shutil.which('clang++')
if not compiler:
    print('SKIP: portable C++20 compiler unavailable')
    raise SystemExit(0)

source = (root / 'src/ghost.cpp').read_text(encoding='utf-8')
assert 'L"M_Glass_Inst"' in source
assert 'L"M_Basic"' in source
assert 'GhostClassName(slot->object, L"Material")' in source
assert 'GhostGlassMIDScalar' in source
assert 'GhostGlassMIDColor' in source
assert 'ApplyGhostGlassMaterial();' in source
for unsafe in ('ScalarParameterValues', 'VectorParameterValues', 'GhostMaterialBlendMode('):
    assert unsafe not in source, f'Potentially dangerous material introspection: {unsafe}'

preamble = (root / 'tests/ghost_native_mock_preamble.hpp').read_text(encoding='utf-8')
preamble = preamble.replace('inline std::wstring UnrealName(FNameView name) {', '''
inline void* g_mock_conv_function = nullptr;
inline void* g_mock_scalar_function = nullptr;
inline void* g_mock_vector_function = nullptr;
inline std::wstring UnrealName(FNameView name) {''')
preamble = preamble.replace('    if (name.index == 501) return L"Mobility";', '''
    if (name.index == 610) return L"MaterialInstanceConstant";
    if (name.index == 611) return L"M_Glass_Inst";
    if (name.index == 612) return L"Material";
    if (name.index == 613) return L"Default__KismetStringLibrary";
    if (name.index == 614) return L"KismetStringLibrary";
    if (name.index == 615) return L"MaterialInstanceDynamic";
    if (name.index == 616) return L"InString";
    if (name.index == 617) return L"ReturnValue";
    if (name.index == 618) return L"ParameterName";
    if (name.index == 619) return L"Value";
    if (name.index == 620) return L"StrProperty";
    if (name.index == 621) return L"NameProperty";
    if (name.index == 622) return L"FloatProperty";
    if (name.index == 623) return L"StructProperty";
    if (name.index == 624) return L"Opacity";
    if (name.index == 625) return L"Color";
    if (name.index == 626) return L"Color2";
    if (name.index == 627) return L"M_Basic";
    if (name.index == 501) return L"Mobility";''')
preamble = preamble.replace('    return std::wcscmp(name, L"Mobility") == 0 ? g_mock_mobility_offset : -1;', '''
    if (std::wcscmp(name, L"InString") == 0) return 0;
    if (std::wcscmp(name, L"ReturnValue") == 0) return 64;
    if (std::wcscmp(name, L"ParameterName") == 0) return 0;
    if (std::wcscmp(name, L"Value") == 0) return 16;
    return std::wcscmp(name, L"Mobility") == 0 ? g_mock_mobility_offset : -1;''')
preamble = preamble.replace('inline void* FindReflectedFunction(void*,const wchar_t*){return nullptr;}', '''
inline void* FindReflectedFunction(void*, const wchar_t* function) {
    if (std::wcscmp(function,L"Conv_StringToName") == 0) return g_mock_conv_function;
    if (std::wcscmp(function,L"SetScalarParameterValue") == 0) return g_mock_scalar_function;
    if (std::wcscmp(function,L"SetVectorParameterValue") == 0) return g_mock_vector_function;
    return nullptr;
}''')
source = 'namespace refunct {' + source.split('namespace refunct {', 1)[1]
main = r'''
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <cstring>

static float g_seen_alpha = -1.f;
static refunct::LinearColor g_seen_color{};
static refunct::FNameView g_seen_scalar_name{},g_seen_vector_name{};
static void MockEvent(void*, void* fn, void* args) {
    using namespace refunct;
    if (fn == g_mock_conv_function) {
        FStringView str{};
        std::memcpy(&str,args,sizeof(str));
        const wchar_t* value = str.data;
        assert(value && str.count>1);
        FNameView name{};
        if (std::wcscmp(value,L"Opacity")==0) name.index=624;
        else if (std::wcscmp(value,L"Color")==0) name.index=625;
        else if (std::wcscmp(value,L"Color2")==0) name.index=626;
        std::memcpy(static_cast<char*>(args)+64,&name,sizeof(name));
    } else if (fn == g_mock_scalar_function) {
        std::memcpy(&g_seen_scalar_name,args,sizeof(g_seen_scalar_name));
        std::memcpy(&g_seen_alpha,static_cast<char*>(args)+16,sizeof(g_seen_alpha));
    } else if (fn == g_mock_vector_function) {
        std::memcpy(&g_seen_vector_name,args,sizeof(g_seen_vector_name));
        std::memcpy(&g_seen_color,static_cast<char*>(args)+16,sizeof(g_seen_color));
    }
}
int main() {
    using namespace refunct;
    // Verify all three exact names are generated through a reflected UFunction.
    UStructHeader library_class{};
    library_class.field.object.name.index=614;
    UObjectHeader library{};library.class_ptr=&library_class;
    UStructHeader material_class{};
    material_class.field.object.name.index=610;
    UObjectHeader glass{};glass.class_ptr=&material_class;glass.name.index=611;
    assert(GhostClassName(&glass,L"MaterialInstanceConstant"));
    assert(!GhostClassName(&glass,L"Material"));
    assert(!GhostClassName(&library,L"MaterialInstanceConstant"));
    UStructHeader basic_class{};basic_class.field.object.name.index=612;
    UObjectHeader basic{};basic.class_ptr=&basic_class;basic.name.index=627;
    assert(GhostClassName(&basic,L"Material"));
    assert(!GhostClassName(&basic,L"MaterialInstanceConstant"));

    // Model the UE4 UObject registry and check mode-specific exact-name lookup.
    library.name.index=613; library.index=2;
    glass.index=1; basic.index=0;
    std::array<GlobalObjectItem,3> slots{};
    slots[0].object=&basic; slots[0].serial_number=11;
    slots[1].object=&glass; slots[1].serial_number=12;
    slots[2].object=&library; slots[2].serial_number=13;
    GlobalObjectArray registry{};
    registry.objects=slots.data();
    registry.num_elements=static_cast<int32_t>(slots.size());
    registry.max_elements=registry.num_elements;
    g_game_module=&registry;
    assert(FindLoadedGhostAppearanceMaterial()==&glass);
    SetPBGhost3DTransparencyEnabled(false);
    assert(FindLoadedGhostAppearanceMaterial()==&basic);
    assert(s_ghost_glass.opaque_material==&basic);
    SetPBGhost3DTransparencyEnabled(true);
    assert(FindLoadedGhostAppearanceMaterial()==&glass);

    UObjectHeader str_type{}, name_type{}, float_type{}, struct_type{};
    str_type.name.index=620;name_type.name.index=621;
    float_type.name.index=622;struct_type.name.index=623;
    UFieldHeader conv_str{}, conv_ret{};
    conv_str.object.name.index=616;conv_str.object.class_ptr=&str_type;
    conv_ret.object.name.index=617;conv_ret.object.class_ptr=&name_type;
    conv_str.next=&conv_ret;
    UStructHeader conv{};conv.children=&conv_str;
    g_mock_conv_function=&conv;

    UFieldHeader scalar_name{},scalar_value{};
    scalar_name.object.name.index=618;scalar_name.object.class_ptr=&name_type;
    scalar_value.object.name.index=619;scalar_value.object.class_ptr=&float_type;
    scalar_name.next=&scalar_value;
    UStructHeader scalar_fn{};scalar_fn.children=&scalar_name;
    g_mock_scalar_function=&scalar_fn;

    UFieldHeader vector_name{},vector_value{};
    vector_name.object.name.index=618;vector_name.object.class_ptr=&name_type;
    vector_value.object.name.index=619;vector_value.object.class_ptr=&struct_type;
    vector_name.next=&vector_value;
    UStructHeader vector_fn{};vector_fn.children=&vector_name;
    g_mock_vector_function=&vector_fn;

    UStructHeader mid_class{};mid_class.field.object.name.index=615;
    UObjectHeader mid{};mid.class_ptr=&mid_class;
    g_process_event=&MockEvent;

    s_ghost_glass.string_library=&library;
    assert(ResolveGhostGlassParameterNames());
    assert(s_ghost_glass.opacity_name.index==624);
    assert(s_ghost_glass.color_name.index==625);
    assert(s_ghost_glass.color2_name.index==626);
    assert(GhostGlassMIDScalar(&mid,s_ghost_glass.opacity_name,0.36f));
    assert(std::abs(g_seen_alpha-0.36f)<0.0001f);
    assert(g_seen_scalar_name.index==624);
    assert(GhostGlassMIDColor(&mid,s_ghost_glass.color_name));
    assert(g_seen_vector_name.index==625);
    assert(std::abs(g_seen_color.r-69.f/255.f)<0.0001f);
    assert(std::abs(g_seen_color.g-235.f/255.f)<0.0001f);
    assert(std::abs(g_seen_color.b-1.0f)<0.0001f);
    SetPBGhostColourRgb(0x8040CCu);
    assert(GhostGlassMIDColor(&mid,s_ghost_glass.color_name));
    assert(std::abs(g_seen_color.r-128.f/255.f)<0.0001f);
    assert(std::abs(g_seen_color.g-64.f/255.f)<0.0001f);
    assert(std::abs(g_seen_color.b-204.f/255.f)<0.0001f);
    assert(GhostGlassMIDColor(&mid,s_ghost_glass.color2_name));
    assert(g_seen_vector_name.index==626);
    // Opaque uses only M_Basic::Color, never glass's scalar Opacity.
    // An actual M_Basic shader is BLEND_Opaque in Refunct's cooked PAK.
    SetPBGhost3DTransparencyEnabled(false);
    assert(!s_ghost_glass.names_ready);
    assert(ResolveGhostGlassParameterNames());
    assert(s_ghost_glass.color_name.index==625);
    assert(s_ghost_glass.opacity_name.index==624); // stale but unused in opaque mode
    assert(GhostGlassMIDColor(&mid,s_ghost_glass.color_name));
    assert(g_seen_vector_name.index==625);
    SetPBGhost3DTransparencyEnabled(true);
    assert(!s_ghost_glass.names_ready);
    // Absent reflection must not invoke ProcessEvent with an arbitrary signature.
    g_mock_vector_function=nullptr;
    assert(!GhostGlassMIDColor(&mid,s_ghost_glass.color_name));
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='ghost_glass_') as temp:
    unit = Path(temp) / 'ghost_glass.cpp'
    exe = Path(temp) / 'ghost_glass'
    unit.write_text(preamble + source + main, encoding='utf-8')
    subprocess.run([compiler, '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-I', str(root / 'include'), str(unit), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('PASS: exact glass class and native FName/scalar/vector parameter calls')

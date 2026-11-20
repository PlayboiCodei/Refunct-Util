"""Verify UE4.14 legacy MID factory is sufficient for ghost colour editing.

Uses real ghost.cpp against synthetic reflected UE4 objects. No injection is
performed; live Refunct appearance still requires in-game confirmation.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
compiler = shutil.which('g++') or shutil.which('clang++')
if not compiler:
    print('SKIP: no portable C++20 compiler')
    raise SystemExit(0)
source = (root/'src/ghost.cpp').read_text(encoding='utf-8')
assert 'CreateAndSetMaterialInstanceDynamic", nullptr' in source
assert 'GhostGlassSetMaterial(mesh, source)' in source
assert 'Ghost 3D: no compatible dynamic material factory' in source
assert 'GhostGlassMIDColor(mid, state.color_name)' in source
for unsafe in ('ScalarParameterValues', 'VectorParameterValues', 'GhostMaterialBlendMode('):
    assert unsafe not in source

prelude = (root/'tests/ghost_native_mock_preamble.hpp').read_text(encoding='utf-8')
prelude = prelude.replace('inline std::wstring UnrealName(FNameView name) {', '''
inline void* g_mock_get = nullptr;
inline void* g_mock_set = nullptr;
inline void* g_mock_legacy = nullptr;
inline void* g_mock_vector = nullptr;
inline std::wstring UnrealName(FNameView name) {''')
prelude = prelude.replace('    if (name.index == 501) return L"Mobility";', '''
    if (name.index == 700) return L"StaticMeshComponent";
    if (name.index == 701) return L"Material";
    if (name.index == 702) return L"MaterialInstanceDynamic";
    if (name.index == 703) return L"ElementIndex";
    if (name.index == 704) return L"ReturnValue";
    if (name.index == 705) return L"IntProperty";
    if (name.index == 706) return L"ObjectProperty";
    if (name.index == 707) return L"ParameterName";
    if (name.index == 708) return L"Value";
    if (name.index == 709) return L"NameProperty";
    if (name.index == 710) return L"StructProperty";
    if (name.index == 711) return L"Color";
    if (name.index == 712) return L"Material";
    if (name.index == 501) return L"Mobility";''')
prelude = prelude.replace('    return std::wcscmp(name, L"Mobility") == 0 ? g_mock_mobility_offset : -1;', '''
    if (std::wcscmp(name,L"ElementIndex") == 0) return 0;
    if (std::wcscmp(name,L"ParameterName") == 0) return 0;
    if (std::wcscmp(name,L"Material") == 0) return 16;
    if (std::wcscmp(name,L"ReturnValue") == 0) return 16;
    if (std::wcscmp(name,L"Value") == 0) return 16;
    return std::wcscmp(name, L"Mobility") == 0 ? g_mock_mobility_offset : -1;''')
prelude = prelude.replace('inline void* FindReflectedFunction(void*,const wchar_t*){return nullptr;}', '''
inline void* FindReflectedFunction(void*,const wchar_t* name) {
    if (std::wcscmp(name,L"GetMaterial") == 0) return g_mock_get;
    if (std::wcscmp(name,L"SetMaterial") == 0) return g_mock_set;
    if (std::wcscmp(name,L"CreateAndSetMaterialInstanceDynamic") == 0) return g_mock_legacy;
    if (std::wcscmp(name,L"SetVectorParameterValue") == 0) return g_mock_vector;
    return nullptr;
}''')
source = 'namespace refunct {' + source.split('namespace refunct {',1)[1]
main = r'''
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <cstring>

static void* g_slot_material = nullptr;
static refunct::UObjectHeader* g_mid_pointer = nullptr;
static refunct::LinearColor g_tint{};
static refunct::FNameView g_tint_parameter{};
static int g_legacy_calls = 0;
static void MockEvent(void*, void* fn, void* args) {
    using namespace refunct;
    if (fn == g_mock_get) {
        std::memcpy(static_cast<char*>(args)+16,&g_slot_material,sizeof(g_slot_material));
    } else if (fn == g_mock_set) {
        std::memcpy(&g_slot_material,static_cast<char*>(args)+16,sizeof(g_slot_material));
    } else if (fn == g_mock_legacy) {
        ++g_legacy_calls;
        g_slot_material=g_mid_pointer;
        std::memcpy(static_cast<char*>(args)+16,&g_mid_pointer,sizeof(g_mid_pointer));
    } else if (fn == g_mock_vector) {
        std::memcpy(&g_tint_parameter,args,sizeof(g_tint_parameter));
        std::memcpy(&g_tint,static_cast<char*>(args)+16,sizeof(g_tint));
    }
}
int main() {
    using namespace refunct;
    UStructHeader mesh_type{}, source_type{}, mid_type{};
    mesh_type.field.object.name.index=700;
    source_type.field.object.name.index=701;
    mid_type.field.object.name.index=702;
    UObjectHeader mesh{}, source{}, mid{}, original{};
    mesh.class_ptr=&mesh_type;
    source.class_ptr=&source_type;
    mid.class_ptr=&mid_type;
    original.class_ptr=&source_type;
    source.index=0;
    mid.index=1;
    std::array<GlobalObjectItem,2> slots{};
    slots[0].object=&source;
    slots[1].object=&mid;
    GlobalObjectArray objects{};
    objects.objects=slots.data();
    objects.num_elements=2;
    objects.max_elements=2;
    g_game_module=&objects;
    g_mid_pointer=&mid;

    UObjectHeader int_kind{},object_kind{},name_kind{},struct_kind{};
    int_kind.name.index=705;object_kind.name.index=706;
    name_kind.name.index=709;struct_kind.name.index=710;
    UFieldHeader elem{},return_field{},material_field{},name_field{},value_field{};
    elem.object.name.index=703;elem.object.class_ptr=&int_kind;
    return_field.object.name.index=704;return_field.object.class_ptr=&object_kind;
    material_field.object.name.index=712;material_field.object.class_ptr=&object_kind;
    name_field.object.name.index=707;name_field.object.class_ptr=&name_kind;
    value_field.object.name.index=708;value_field.object.class_ptr=&struct_kind;
    UStructHeader get{},set{},legacy{},vector{};
    elem.next=&return_field;get.children=&elem;
    // A second element index with a unique next pointer for SetMaterial.
    UFieldHeader elem_set=elem;
    elem_set.next=&material_field;
    set.children=&elem_set;
    UFieldHeader elem_legacy=elem;
    elem_legacy.next=&return_field;
    legacy.children=&elem_legacy;
    name_field.next=&value_field;
    vector.children=&name_field;
    g_mock_get=&get;
    g_mock_set=&set;
    g_mock_legacy=&legacy;
    g_mock_vector=&vector;
    g_process_event=&MockEvent;

    // Neither modern factory exists in the test environment; the one-argument
    // legacy factory must first receive the requested source on slot 0.
    g_slot_material=&original;
    assert(GhostGlassCreateMID(&mesh,&source)==&mid);
    assert(g_legacy_calls==1 && g_slot_material==&mid);
    SetPBGhostColourRgb(0x7F40D0);
    assert(GhostGlassMIDColor(&mid,FNameView{711,0}));
    assert(g_tint_parameter.index==711);
    assert(std::fabs(g_tint.r-127.f/255.f)<0.0001f);
    assert(std::fabs(g_tint.g-64.f/255.f)<0.0001f);
    assert(std::fabs(g_tint.b-208.f/255.f)<0.0001f);

    // If the legacy factory is absent too, the original material is restored;
    // do not silently claim that a non-editable material has been coloured.
    g_mock_legacy=nullptr;
    g_slot_material=&original;
    assert(GhostGlassCreateMID(&mesh,&source)==nullptr);
    assert(g_slot_material==&original);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='ghost_legacy_mid_') as tmp:
    src = Path(tmp)/'ghost_mid.cpp'
    exe = Path(tmp)/'ghost_mid'
    src.write_text(prelude+source+main,encoding='utf-8')
    subprocess.run([compiler,'-std=c++20','-O2','-Wall','-Wextra','-Werror',
                    '-I',str(root/'include'),str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('PASS: legacy UE4 MID factory creates editable ghost instance, with safe fallback')

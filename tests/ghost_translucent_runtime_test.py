"""Test material selection and alpha parameters against reflected UE4 mock objects.

The material's *compiled* blend mode must be translucent. Tests cannot establish
which compatible material assets are loaded in the actual Refunct process.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
compiler = shutil.which('g++') or shutil.which('clang++')
if compiler is None:
    print('SKIP: portable C++20 compiler unavailable')
    raise SystemExit(0)

source = (root / 'src' / 'ghost.cpp').read_text(encoding='utf-8')
prelude = (root / 'tests' / 'ghost_native_mock_preamble.hpp').read_text(encoding='utf-8')
assert 'GhostPrepareDeferredMobility(component)' in source
assert 'GhostCallByte(component, L"SetCollisionEnabled", L"NewType", 0)' in source
assert 'ApplyWorldGhostAppearance();' in source
assert 'GhostMaterialBlendMode(source)' in source
assert 'GhostCreateMID(component, s_ghost_appearance.material)' in source
assert 'GhostSetMaterial(component, source)' in source
assert 'std::array<float, 6> kGhostPartOpacity' in source
assert 'appearance_failures >= 3' in source
assert 'part.appearance_attempted = true;' in source
assert 'GhostSetMIDTint(mid, s_ghost_appearance.colour_parameter, kGhostPartOpacity[i])' in source
assert 'NO usable loaded translucent mesh material with adjustable opacity' in source
assert 'if (!has_opacity) continue;' in source
assert 'if (appearance.scan_cursor < objects->num_elements) return false;' in source
assert 'CreateAndSetMaterialInstanceDynamic' in source

prelude = prelude.replace('inline int g_mock_mobility_offset = -1;', '''
inline int g_mock_mobility_offset = -1;
inline int g_mock_blend_offset = -1;
inline int g_mock_parent_offset = -1;
inline int g_mock_scalar_offset = -1;
inline int g_mock_vector_offset = -1;''')
prelude = prelude.replace('    if (name.index == 501)', '''
    if (name.index == 510) return L"Material";
    if (name.index == 511) return L"MaterialInstance";
    if (name.index == 512) return L"BlendMode";
    if (name.index == 513) return L"Opacity";
    if (name.index == 514) return L"Tint";
    if (name.index == 501)''')
prelude = prelude.replace('return std::wcscmp(name, L"Mobility") == 0 ? g_mock_mobility_offset : -1;', '''
    if (std::wcscmp(name, L"BlendMode") == 0) return g_mock_blend_offset;
    if (std::wcscmp(name, L"Parent") == 0) return g_mock_parent_offset;
    if (std::wcscmp(name, L"ScalarParameterValues") == 0) return g_mock_scalar_offset;
    if (std::wcscmp(name, L"VectorParameterValues") == 0) return g_mock_vector_offset;
    return std::wcscmp(name, L"Mobility") == 0 ? g_mock_mobility_offset : -1;''')
source = 'namespace refunct {' + source.split('namespace refunct {', 1)[1]
main = r'''
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstddef>
int main() {
    using namespace refunct;
    UStructHeader material_class{};
    material_class.field.object.name.index = 510;
    UStructHeader instance_class{};
    instance_class.field.object.name.index = 511;
    struct SyntheticMaterial {
        UObjectHeader header;
        uint8_t padding[64]{};
        uint8_t blend = 2;
    } material{};
    material.header.class_ptr = &material_class;
    g_mock_blend_offset = static_cast<int>(offsetof(SyntheticMaterial, blend));
    assert(GhostMaterialBlendMode(&material) == 2);

    struct ReflectedArray {const uint8_t* data;int32_t count,capacity;};
    struct SyntheticInstance {
        UObjectHeader header;
        uint8_t padding[64]{};
        void* parent = nullptr;
        ReflectedArray scalar{};
        ReflectedArray colour{};
    } instance{};
    instance.header.class_ptr = &instance_class;
    instance.parent = &material;
    const FNameView opacity{513,0}, tint{514,0};
    instance.scalar = {reinterpret_cast<const uint8_t*>(&opacity),1,1};
    instance.colour = {reinterpret_cast<const uint8_t*>(&tint),1,1};
    g_mock_parent_offset = static_cast<int>(offsetof(SyntheticInstance,parent));
    g_mock_scalar_offset = static_cast<int>(offsetof(SyntheticInstance,scalar));
    g_mock_vector_offset = static_cast<int>(offsetof(SyntheticInstance,colour));

    assert(GhostMaterialBlendMode(&instance) == 2);
    FNameView param{};
    assert(GhostMaterialParameter(&instance,L"ScalarParameterValues",false,param));
    assert(param.index == 513);
    assert(GhostMaterialParameter(&instance,L"VectorParameterValues",true,param));
    assert(param.index == 514);

    // Opacity is not achieved by setting alpha on an opaque base material.
    material.blend = 0;
    assert(GhostMaterialBlendMode(&instance) == 0);
    assert(GhostMaterialScore(L"M_Hologram",0,true,true,true) < 0);
    material.blend = 2;
    assert(GhostMaterialScore(L"MI_Transparent",2,true,true,true) >
           GhostMaterialScore(L"M_Water",2,false,false,false));
    assert(GhostMaterialScore(L"MI_Glass",2,false,false,true) >
           GhostMaterialScore(L"DefaultParticle",3,false,false,false));
    assert(GhostMaterialScore(L"M_Transparent_Window",2,false,false,false) >= 45);
    assert(GhostMaterialScore(L"M_UnderwaterPostProcess",2,false,false,false) <
           GhostMaterialScore(L"M_Transparent_Window",2,false,false,false));

    // More than one overridden scalar/vector: the target name is at index 1.
    struct ScalarParameterRecord {
        FNameView name;
        uint8_t rest[24]{};
    };
    static_assert(sizeof(ScalarParameterRecord) == 32);
    struct VectorParameterRecord {
        FNameView name;
        uint8_t rest[36]{};
    };
    static_assert(sizeof(VectorParameterRecord) == 44);
    ScalarParameterRecord scalars[2]{{FNameView{0,0},{}},{opacity,{}}};
    VectorParameterRecord vectors[2]{{FNameView{0,0},{}},{tint,{}}};
    instance.scalar = {reinterpret_cast<const uint8_t*>(scalars),2,2};
    instance.colour = {reinterpret_cast<const uint8_t*>(vectors),2,2};
    assert(GhostMaterialParameter(&instance,L"ScalarParameterValues",false,param));
    assert(param.index == opacity.index);
    assert(GhostMaterialParameter(&instance,L"VectorParameterValues",true,param));
    assert(param.index == tint.index);

    // The real log proves Widget3DPassThrough_Translucent is unusable for
    // the mesh ghost even though SetMaterial accepts it.
    assert(GhostMaterialScore(L"Widget3DPassThrough_Translucent",2,false,false,false) < 0);
    assert(GhostMaterialScore(L"M_UI_Hologram",2,true,true,true) < 0);
    assert(GhostMaterialScore(L"MI_GhostTranslucent",2,true,true,true) > 200);

    instance.scalar.count = 0;
    assert(!GhostMaterialParameter(&instance,L"ScalarParameterValues",false,param));
    instance.scalar.count = 1;
    instance.parent = nullptr;
    assert(GhostMaterialBlendMode(&instance) == -1);
    instance.parent = &instance;
    assert(GhostMaterialBlendMode(&instance) == -1);
    for (const float opacity : kGhostPartOpacity) assert(opacity>0 && opacity<1);
    assert(kGhostTint.b > kGhostTint.r);
    for (size_t i=0; i<kGhostPartOpacity.size(); ++i) {
        const LinearColor tinted = GhostPartTint(kGhostPartOpacity[i]);
        assert(std::abs(tinted.a - kGhostPartOpacity[i]) < 0.00001f);
        assert(tinted.r == kGhostTint.r && tinted.g == kGhostTint.g);
    }
    assert(GhostPartTint(-1.f).a == 0.f);
    assert(GhostPartTint(2.f).a == 1.f);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='ghost_translucency_') as temp:
    unit = Path(temp) / 'translucency_mock.cpp'
    binary = Path(temp) / 'translucency_mock'
    unit.write_text(prelude + source + main, encoding='utf-8')
    subprocess.run([compiler, '-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-I', str(root/'include'), str(unit), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('PASS: translucency requires a blended material; safe opacity/tint parameter handling')

"""Exercise the real pillar removal/restore path with a fake UE4 ProcessEvent.

Historical test filename retained for automation. No Refunct binary is launched.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/pillars.cpp').read_text(encoding='utf-8')
code = source.split('bool SetStaticMeshAsset(', 1)[1].split('void Apply(Target& target,', 1)[0]
code = 'bool SetStaticMeshAsset(' + code

harness = r'''
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>
#include <unordered_map>
#include <vector>
struct UObjectHeader {
    void* class_ptr = nullptr;
    bool visible = true;
    uint8_t collision = 3;
    void* static_mesh = nullptr;
};
struct UStructHeader { int x = 0; };
struct GlobalObjectArray { int x = 0; };
struct Target {
    void* mesh = nullptr;
    bool original_visible = false;
    uint8_t original_collision = 0;
    void* original_static_mesh = nullptr;
    bool mesh_detached = false;
    bool removal_saved = false;
    bool visual_original_visible = false;
    bool visual_saved = false;
    uint64_t next_visibility_check = 0;
    uint64_t next_removal_check = 0;
};
using ULONGLONG = uint64_t;
constexpr ULONGLONG kValidateMs = 1500;
constexpr wchar_t kPillarMesh[] = L"SM_Block_1000x1000x20000";
static bool s_removal_failure_logged = false;
static bool s_visibility_failure_logged = false;
static bool s_detach_fallback_logged = false;
static bool s_detach_success_logged = false;
static int visible_get, visible_set, collision_get, collision_set, static_mesh_set, cls;
static int visibility_writes = 0, collision_writes = 0, mesh_writes = 0;
static bool allow_visibility = true, allow_collision = true, allow_mesh = true;
static bool collision_available = true, mesh_api_available = true, asset_live = true;
static int last_propagate = -1;
static std::vector<std::wstring> messages;
static UObjectHeader asset{};
using ProcessEvent = void(*)(void*, void*, void*);
static void FakeProcessEvent(void* object, void* function, void* params) {
    auto* component = static_cast<UObjectHeader*>(object);
    auto* args = static_cast<uint8_t*>(params);
    if (function == &visible_get) {
        args[0] = component->visible ? 1 : 0;
    } else if (function == &visible_set) {
        ++visibility_writes;
        last_propagate = args[1];
        if (allow_visibility) component->visible = args[0] != 0;
    } else if (function == &collision_get) {
        args[0] = component->collision;
    } else if (function == &collision_set) {
        ++collision_writes;
        if (allow_collision) component->collision = args[0];
    } else if (function == &static_mesh_set) {
        ++mesh_writes;
        void* mesh = nullptr;
        std::memcpy(&mesh, args, sizeof(mesh));
        if (allow_mesh) component->static_mesh = mesh;
    } else assert(false);
}
static ProcessEvent g_process_event = FakeProcessEvent;
static bool IsReadable(const void* address, size_t) { return address != nullptr; }
static bool IsLive(void* object, const GlobalObjectArray* objects) {
    return objects != nullptr && object == &asset && asset_live;
}
static std::wstring ObjectName(void* object) {
    return object == &asset ? kPillarMesh : L"OtherMesh";
}
static void* ReadReflectedObject(void* object, const wchar_t* name) {
    return wcscmp(name, L"StaticMesh") == 0 ?
        static_cast<UObjectHeader*>(object)->static_mesh : nullptr;
}
static void* FindReflectedFunction(void*, const wchar_t* name) {
    if (wcscmp(name, L"SetStaticMesh") == 0) return mesh_api_available ? &static_mesh_set : nullptr;
    if (wcscmp(name, L"IsVisible") == 0) return &visible_get;
    if (wcscmp(name, L"SetVisibility") == 0) return &visible_set;
    if (wcscmp(name, L"GetCollisionEnabled") == 0) return collision_available ? &collision_get : nullptr;
    if (wcscmp(name, L"SetCollisionEnabled") == 0) return collision_available ? &collision_set : nullptr;
    return nullptr;
}
static int32_t ParameterOffset(void* fn, const wchar_t* name, const wchar_t* type) {
    if (!fn) return -1;
    if (wcscmp(name, L"NewMesh") == 0 && wcscmp(type, L"ObjectProperty") == 0) return 0;
    if (wcscmp(type, L"BoolProperty") != 0) return -1;
    if (wcscmp(name, L"ReturnValue") == 0) return 0;
    if (wcscmp(name, L"bNewVisibility") == 0) return 0;
    if (wcscmp(name, L"bPropagateToChildren") == 0) return 1;
    return -1;
}
static int32_t FindClassPropertyOffset(void*, const wchar_t* name) {
    return wcscmp(name, L"ReturnValue") == 0 || wcscmp(name, L"NewType") == 0 ? 0 : -1;
}
static void Log(const wchar_t* message) { messages.emplace_back(message); }
'''

main = r'''
int main() {
    const GlobalObjectArray objects{};
    // Primary path: detach the actual geometry asset; restore it by pointer.
    UObjectHeader first{&cls, true, 3, &asset};
    Target t1{&first};
    ApplyRemoval(t1, &objects, true, 1);
    assert(first.static_mesh == nullptr && t1.original_static_mesh == &asset);
    assert(t1.mesh_detached && t1.removal_saved && mesh_writes == 1);
    assert(first.visible && first.collision == 3); // Mesh setter owns physics state.
    ApplyRemoval(t1, &objects, true, 2);
    assert(mesh_writes == 1); // Throttled.
    ApplyRemoval(t1, &objects, false, 2000);
    assert(first.static_mesh == &asset && !t1.mesh_detached && !t1.removal_saved);
    assert(mesh_writes == 2);

    // Reassert a missing mesh if Refunct reattaches it while removal is ON.
    ApplyRemoval(t1, &objects, true, 4000);
    first.static_mesh = &asset;
    ApplyRemoval(t1, &objects, true, 6000);
    assert(first.static_mesh == nullptr && mesh_writes == 4);

    // Stale asset: don't write a potentially freed UObject pointer during restore.
    asset_live = false;
    ApplyRemoval(t1, &objects, false, 8000);
    assert(t1.mesh_detached && t1.removal_saved && first.static_mesh == nullptr);
    asset_live = true;
    ApplyRemoval(t1, &objects, false, 11000);
    assert(!t1.mesh_detached && first.static_mesh == &asset);

    // Engine may reject SetStaticMesh. Fallback removes BOTH rendering and collision.
    mesh_api_available = false;
    UObjectHeader second{&cls, true, 3, &asset};
    Target t2{&second};
    ApplyRemoval(t2, &objects, true, 13000);
    assert(second.static_mesh == &asset && !second.visible && second.collision == 0);
    assert(t2.removal_saved && !t2.mesh_detached && t2.original_collision == 3);
    assert(last_propagate == 0);
    ApplyRemoval(t2, &objects, false, 15000);
    assert(second.visible && second.collision == 3 && !t2.removal_saved);

    // Original visibility and nondefault collision are preserved by fallback.
    UObjectHeader third{&cls, false, 1, &asset};
    Target t3{&third};
    ApplyRemoval(t3, &objects, true, 17000);
    assert(!third.visible && third.collision == 0);
    ApplyRemoval(t3, &objects, false, 19000);
    assert(!third.visible && third.collision == 1 && !t3.removal_saved);

    // Collision failure must not hide a solid pillar.
    UObjectHeader fourth{&cls, true, 3, &asset};
    Target t4{&fourth};
    allow_collision = false;
    const int vis_before = visibility_writes;
    ApplyRemoval(t4, &objects, true, 21000);
    assert(fourth.visible && fourth.collision == 3 && t4.removal_saved);
    assert(visibility_writes == vis_before && t4.next_removal_check == 24000);
    allow_collision = true;
    ApplyRemoval(t4, &objects, true, 21001);
    assert(fourth.collision == 3);
    ApplyRemoval(t4, &objects, true, 24000);
    assert(!fourth.visible && fourth.collision == 0);

    // Visibility failure can be retried without losing original state.
    UObjectHeader fifth{&cls, true, 2, &asset};
    Target t5{&fifth};
    allow_visibility = false;
    ApplyRemoval(t5, &objects, true, 26000);
    assert(fifth.visible && fifth.collision == 0 && t5.removal_saved);
    allow_visibility = true;
    ApplyRemoval(t5, &objects, true, 29000);
    assert(!fifth.visible && fifth.collision == 0);
    ApplyRemoval(t5, &objects, false, 31000);
    assert(fifth.visible && fifth.collision == 2 && !t5.removal_saved);

    // Unavailable API leaves an unmodified component rather than hiding it.
    collision_available = false;
    int other_cls = 0;
    UObjectHeader sixth{&other_cls, true, 3, &asset};
    Target t6{&sixth};
    ApplyRemoval(t6, &objects, true, 33000);
    assert(sixth.visible && sixth.collision == 3 && !t6.removal_saved);
    assert(t6.next_removal_check == 36000);

    // Cosmetic visibility must never change collision or detach StaticMesh.
    collision_available = true;
    mesh_api_available = true;
    UObjectHeader seventh{&cls, true, 2, &asset};
    Target t7{&seventh};
    const int collision_before = collision_writes;
    const int mesh_before = mesh_writes;
    ApplyVisibility(t7, false, false, 37000);
    assert(!seventh.visible && seventh.collision == 2 && seventh.static_mesh == &asset);
    assert(t7.visual_saved && t7.visual_original_visible);
    assert(collision_writes == collision_before && mesh_writes == mesh_before);
    ApplyVisibility(t7, true, false, 39000);
    assert(seventh.visible && seventh.collision == 2 && !t7.visual_saved);

    // Original hidden state must remain hidden after a hide/show cycle.
    UObjectHeader eighth{&cls, false, 1, &asset};
    Target t8{&eighth};
    ApplyVisibility(t8, false, false, 40000);
    assert(!eighth.visible && t8.visual_saved && !t8.visual_original_visible);
    ApplyVisibility(t8, true, false, 42000);
    assert(!eighth.visible && !t8.visual_saved && eighth.collision == 1);

    // Failed cosmetic setter is retried without losing the saved baseline.
    UObjectHeader ninth{&cls, true, 3, &asset};
    Target t9{&ninth};
    allow_visibility = false;
    ApplyVisibility(t9, false, false, 43000);
    assert(ninth.visible && t9.visual_saved && t9.next_visibility_check == 46000);
    allow_visibility = true;
    ApplyVisibility(t9, false, false, 43001);
    assert(ninth.visible);
    ApplyVisibility(t9, false, false, 46000);
    assert(!ninth.visible && ninth.collision == 3);
    ApplyVisibility(t9, true, false, 48000);
    assert(ninth.visible && !t9.visual_saved);

    // World removal temporarily takes priority; cosmetic preference survives.
    UObjectHeader tenth{&cls, true, 3, &asset};
    Target t10{&tenth};
    ApplyVisibility(t10, false, false, 49000);
    assert(!tenth.visible && t10.visual_saved);
    ApplyRemoval(t10, &objects, true, 50000);
    assert(tenth.static_mesh == nullptr && t10.mesh_detached);
    ApplyVisibility(t10, true, true, 50001); // Queue visual restore while removed.
    assert(t10.visual_saved && !tenth.visible);
    ApplyRemoval(t10, &objects, false, 52000);
    assert(tenth.static_mesh == &asset && !t10.mesh_detached);
    ApplyVisibility(t10, true, false, 52001);
    assert(tenth.visible && !t10.visual_saved && tenth.collision == 3);

    // Fallback collision removal also preserves cosmetic visibility.
    mesh_api_available = false;
    UObjectHeader eleventh{&cls, true, 3, &asset};
    Target t11{&eleventh};
    ApplyVisibility(t11, false, false, 53000);
    ApplyRemoval(t11, &objects, true, 55000);
    assert(!eleventh.visible && eleventh.collision == 0);
    ApplyVisibility(t11, true, true, 55001);
    assert(t11.visual_saved);
    ApplyRemoval(t11, &objects, false, 57000);
    assert(!eleventh.visible && eleventh.collision == 3);
    ApplyVisibility(t11, true, false, 57001);
    assert(eleventh.visible && eleventh.collision == 3 && !t11.visual_saved);
}
'''

with tempfile.TemporaryDirectory() as directory:
    cpp = Path(directory) / 'removal.cpp'
    exe = Path(directory) / 'removal'
    cpp.write_text(harness + '\n' + code + '\n' + main, encoding='utf-8')
    subprocess.run(['g++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                    str(cpp), '-o', str(exe)], check=True, timeout=30)
    subprocess.run([str(exe)], check=True, timeout=10)
print('PASS: native StaticMesh detach/restore, fallback removal, collision retry and safe UObject checks')

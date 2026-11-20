"""Compile the real map-setter code against a simulated Refunct actor + scene root.

The simulation cannot establish in-game ABI support; it checks that the native
component fallback recovers from a pipe rejecting its generic actor setter.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'src/maps.cpp').read_text(encoding='utf-8')
original_test = (root/'tests/custom_maps_cpp_syntax_test.py').read_text(encoding='utf-8')
prelude = original_test.split("prelude=r'''", 1)[1].split("'''",1)[0]

# Pull the exact functions used by the loader: test their real implementation.
native_block = source[source.index('const GlobalObjectArray* ObjectArray()'):source.index('bool ReadBoundsSize(')]
setter_block = source[source.index('struct SceneTransformApi {'):source.index('bool ResolveMapNativeApi(')]

mock = r'''
namespace refunct {
namespace {
struct MockActor {
    UObjectHeader header{};
    void* root_component{};
    ActorPose pose{};
};
struct MockRoot {
    UObjectHeader header{};
    MockActor* owner{};
};
constexpr uintptr_t kActorClass = 0x11110;
constexpr uintptr_t kRootClass = 0x22220;
struct MockFunction { const wchar_t* name; };
MockFunction get_loc{L"getLoc"}, get_rot{L"getRot"}, get_scale{L"getScale"};
MockFunction set_actor_loc{L"setActorLoc"}, set_actor_rot{L"setActorRot"}, set_actor_scale{L"setActorScale"};
MockFunction set_root_both{L"K2_SetWorldLocationAndRotation"}, set_root_scale{L"SetWorldScale3D"};
bool root_available = true;
bool actor_refuses = true;
MockActor actor{};
MockRoot scene{};
GlobalObjectItem items[2]{};
GlobalObjectArray array{};
std::wstring failure_log;
void Dispatch(void* object, void* function, void* data) {
    if (function == &get_loc) {
        std::memcpy(data, &actor.pose.position, sizeof(Vec3));
    } else if (function == &get_rot) {
        std::memcpy(data, &actor.pose.rotation, sizeof(Rotator));
    } else if (function == &get_scale) {
        std::memcpy(data, &actor.pose.scale, sizeof(Vec3));
    } else if (function == &set_actor_loc) {
        if (!actor_refuses) std::memcpy(&actor.pose.position, data, sizeof(Vec3));
    } else if (function == &set_actor_rot) {
        if (!actor_refuses) std::memcpy(&actor.pose.rotation, data, sizeof(Rotator));
    } else if (function == &set_actor_scale) {
        if (!actor_refuses) std::memcpy(&actor.pose.scale, data, sizeof(Vec3));
    } else if (function == &set_root_both) {
        assert(object == &scene);
        std::memcpy(&actor.pose.position, data, sizeof(Vec3));
        std::memcpy(&actor.pose.rotation, static_cast<char*>(data)+16, sizeof(Rotator));
        // UE4 can canonically represent pitch=90,yaw=90,roll=-90 as
        // pitch=90,yaw=0,roll=-180. This is exactly the same orientation.
        if (std::abs(actor.pose.rotation.pitch - 90.f) < 0.01f &&
            std::abs(actor.pose.rotation.yaw - 90.f) < 0.01f &&
            std::abs(actor.pose.rotation.roll + 90.f) < 0.01f) {
            actor.pose.rotation = {90.f, 0.f, -180.f};
        }
    } else if (function == &set_root_scale) {
        assert(object == &scene);
        std::memcpy(&actor.pose.scale, data, sizeof(Vec3));
    } else {
        assert(false && "Unexpected reflected function");
    }
}
}
void* g_game_module = nullptr;
std::wstring g_storage_dir;
void* g_practice_level_class = nullptr;
std::array<void*,31> g_practice_level_actors{};
bool g_recording_tainted = false, g_pb_cheats_allowed_for_run = false, g_run_pb_eligible = true;
void(*g_process_event)(void*,void*,void*) = Dispatch;
bool IsReadable(const void* ptr, size_t) { return ptr != nullptr; }
bool IsWritable(const void* ptr, size_t) { return ptr != nullptr; }
void* FindReflectedFunction(void* object, const wchar_t* name) {
    if (object != &scene || !root_available) return nullptr;
    if (std::wcscmp(name,L"K2_SetWorldLocationAndRotation") == 0) return &set_root_both;
    if (std::wcscmp(name,L"SetWorldScale3D") == 0) return &set_root_scale;
    return nullptr;
}
int32_t FindClassPropertyOffset(void* object, const wchar_t* name) {
    if (object == reinterpret_cast<void*>(kActorClass) && std::wcscmp(name,L"RootComponent") == 0)
        return offsetof(MockActor,root_component);
    if (object == &set_root_both) {
        if (std::wcscmp(name,L"NewLocation") == 0) return 0;
        if (std::wcscmp(name,L"NewRotation") == 0) return 16;
        if (std::wcscmp(name,L"bTeleport") == 0) return 40;
    }
    if (object == &set_root_scale && std::wcscmp(name,L"NewScale") == 0) return 0;
    return -1;
}
std::wstring UnrealName(decltype(UObjectHeader::name)) { return L"BP_TravelPipe_359"; }
void Log(const wchar_t* msg) { failure_log = msg; }
}

int main() {
    using namespace refunct;
    actor.header.index=0;
    actor.header.class_ptr=reinterpret_cast<void*>(kActorClass);
    actor.root_component=&scene;
    actor.pose.position={10,20,30};
    actor.pose.rotation={0,0,0};
    actor.pose.scale={1,1,1};
    scene.header.index=1;
    scene.header.class_ptr=reinterpret_cast<void*>(kRootClass);
    scene.owner=&actor;
    items[0].object=reinterpret_cast<UObjectHeader*>(&actor);
    items[0].serial_number=101;
    items[1].object=reinterpret_cast<UObjectHeader*>(&scene);
    items[1].serial_number=102;
    array.objects=items;
    array.num_elements=2;
    array.max_elements=2;
    g_game_module=reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(&array)-kGlobalObjectArrayRva);
    NativeApi api{};
    api.get_location=&get_loc; api.get_rotation=&get_rot; api.get_scale=&get_scale;
    api.loc_return=api.rot_return=api.scale_return=0;
    api.set_location=&set_actor_loc; api.set_rotation=&set_actor_rot; api.set_scale=&set_actor_scale;
    api.loc_input=api.rot_input=api.scale_input=0;
    NativeActor pipe{};
    pipe.handle=MakeHandle(&actor,&array); pipe.cluster=6; pipe.kind=4; pipe.element=0;
    assert(ScenePoseSupported(pipe.handle));
    ActorPose target{{110000,20000,30000},{5,7,3},{90,90,-90}};
    assert(SetPose(api, pipe, target) && "Pipe should move via scene component even if actor setter rejects it");
    assert(PoseMatches(api,&actor,target));
    assert(OrientationsMatch(Rotator{90.f, 0.f, -180.f}, Rotator{90.f, 90.f, -90.f}));
    assert(!OrientationsMatch(Rotator{0.f, 0.f, 0.f}, Rotator{90.f, 90.f, -90.f}));
    assert(ScalesMatch(Vec3{100.f,100.f,100.f}, Vec3{100.3f,100.3f,100.3f}));
    assert(!ScalesMatch(Vec3{1.f,1.f,1.f}, Vec3{2.f,2.f,2.f}));
    ActorPose original{{10,20,30},{1,1,1},{0,0,0}};
    assert(SetPose(api,pipe,original) && "Rollback must also restore scene transform");
    root_available=false;
    assert(!ScenePoseSupported(pipe.handle));
    assert(ScenePoseFailureReason(pipe.handle).find(L"scene location/rotation function missing") != std::wstring::npos);
    assert(!SetPose(api,pipe,target) && "Both paths unavailable must fail explicitly");
    assert(failure_log.find(L"section 7") != std::wstring::npos &&
           failure_log.find(L"BP_TravelPipe_359") != std::wstring::npos);
    actor_refuses=false;
    assert(SetPose(api,pipe,target) && "Actor setter should remain as compatible fallback");
    std::puts("PASS: pipe uses root-component transform; rollback; logged failure; actor fallback");
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)/'mock_map.cpp'
    path.write_text('#include <cassert>\n#include <cstdio>\n#include <cstddef>\n'+prelude+ '\n'
                    'namespace refunct { namespace {\n' + r"""
constexpr size_t kNativeParamsSize = 256;
struct ActorHandle { void* actor = nullptr; int32_t index = -1; int32_t serial = 0; void* klass = nullptr; };
struct ActorPose { Vec3 position{}, scale{}; Rotator rotation{}; };
struct NativeActor { ActorHandle handle; int cluster = -1, kind = -1, element = -1; ActorPose original{}; Vec3 original_size{}; };
struct NativeApi {
    void* get_location = nullptr, *get_rotation = nullptr, *get_scale = nullptr;
    void* get_bounds = nullptr, *set_location = nullptr, *set_rotation = nullptr, *set_scale = nullptr;
    int loc_return = -1, rot_return = -1, scale_return = -1;
    int bounds_extent = -1;
    int loc_input = -1, rot_input = -1, scale_input = -1;
};
""" + native_block + setter_block + '\n}}\n'+ mock, encoding='utf-8')
    executable=Path(directory)/'map_test'
    result=subprocess.run(['clang++','-std=c++20','-Wall','-Wextra','-Werror','-Wno-unused-const-variable',
                           '-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
                           '-I',str(root/'include'),str(path),'-o',str(executable)],
                          capture_output=True,text=True,timeout=45)
    if result.returncode:
        print(result.stderr[-10000:]);raise SystemExit(1)
    run=subprocess.run([str(executable)],capture_output=True,text=True,timeout=10)
    print(run.stdout or run.stderr)
    assert run.returncode==0

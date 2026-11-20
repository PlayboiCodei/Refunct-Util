#include "refunct/internal_api.hpp"
#include "refunct/custom_map_format.hpp"
#include <array>
#include <cwchar>
#include <limits>
#include <memory>
#include <vector>

namespace refunct {
namespace {
using custom_maps::Element;
using custom_maps::Map;
constexpr size_t kTotalMapActors = 251 + 18 + 37 + 3 + 9 + 4;
constexpr size_t kMaxMapFiles = 48;
constexpr size_t kNativeParamsSize = 256;

struct ActorHandle {
    void* actor = nullptr;
    int32_t index = -1;
    int32_t serial = 0;
    void* klass = nullptr;
};
struct ActorPose { Vec3 position{}, scale{}; Rotator rotation{}; };
struct NativeActor {
    ActorHandle handle;
    int cluster = -1;
    int kind = -1;
    int element = -1;
    ActorPose original{};
    Vec3 original_size{};
};
struct NativeRoot {
    ActorHandle handle;
    Vec3 source{};
    Vec3 original_location{};
    Rotator rotation{};
    float speed = 0;
};
struct NativeApi {
    void* get_location = nullptr, *get_rotation = nullptr, *get_scale = nullptr;
    void* get_bounds = nullptr, *set_location = nullptr, *set_rotation = nullptr, *set_scale = nullptr;
    int loc_return = -1, rot_return = -1, scale_return = -1;
    int bounds_extent = -1;
    int loc_input = -1, rot_input = -1, scale_input = -1;
};
struct CustomMapState {
    std::vector<std::wstring> files;
    std::vector<NativeActor> actors;
    std::array<NativeRoot, custom_maps::kClusterCount> roots{};
    NativeApi api;
    Map loaded_map;
    std::wstring current_file;
    std::wstring status = L"No custom map loaded";
    size_t cursor = 0;
    bool snapshot = false;
    bool applying = false;
    bool restoring = false;
    bool active = false;
    // Only user-initiated map changes queue a normal R restart after actor work.
    // Internal reapplication, failure rollback, and full-level reload do not.
    bool restart_after_change = false;
    bool awaiting_original_reload = false;
    // If OpenLevel did not complete, never snapshot a potentially modified world.
    bool original_reload_unconfirmed = false;
    int last_resets = -1;
    int restart_reapply_delay = 0;
    // One map request is processed entirely on the game's character tick.
} s_map;

std::wstring MapFolder() {
    return g_storage_dir.empty() ? L"" : g_storage_dir + L"\\Maps";
}
void MapError(const std::wstring& reason) {
    s_map.status = L"Map error: " + reason;
    Log((L"Custom maps: " + reason).c_str());
    Notify(L"Map load failed - check RefunctUtility.log");
}
std::wstring ToWideAscii(const std::string& text) {
    return std::wstring(text.begin(), text.end());
}

const GlobalObjectArray* ObjectArray() {
    if (!g_game_module) return nullptr;
    const uintptr_t base = reinterpret_cast<uintptr_t>(g_game_module);
    const auto* objects = reinterpret_cast<const GlobalObjectArray*>(base + kGlobalObjectArrayRva);
    if (!IsReadable(objects, sizeof(*objects)) || !objects->objects ||
        objects->num_elements <= 0 || objects->num_elements > 500000 ||
        objects->max_elements < objects->num_elements ||
        !IsReadable(objects->objects, static_cast<size_t>(objects->num_elements) *
                       sizeof(GlobalObjectItem))) return nullptr;
    return objects;
}
ActorHandle MakeHandle(void* actor, const GlobalObjectArray* objects) {
    if (!objects || !IsReadable(actor, sizeof(UObjectHeader))) return {};
    const auto* header = static_cast<const UObjectHeader*>(actor);
    if (header->index < 0 || header->index >= objects->num_elements) return {};
    const auto& slot = objects->objects[header->index];
    if (slot.object != actor || (static_cast<uint32_t>(slot.flags) & 0x30000000u)) return {};
    return {actor, header->index, slot.serial_number, header->class_ptr};
}
bool ValidHandle(const ActorHandle& ref) {
    const GlobalObjectArray* objects = ObjectArray();
    if (!objects || !ref.actor || ref.index < 0 || ref.index >= objects->num_elements ||
        !IsReadable(ref.actor, sizeof(UObjectHeader))) return false;
    const auto& slot = objects->objects[ref.index];
    return slot.object == ref.actor && slot.serial_number == ref.serial &&
        !(static_cast<uint32_t>(slot.flags) & 0x30000000u) &&
        static_cast<const UObjectHeader*>(ref.actor)->class_ptr == ref.klass;
}

bool ReadPose(const NativeApi& api, void* actor, ActorPose& out) {
    if (!g_process_event) return false;
    alignas(16) std::array<uint8_t, kNativeParamsSize> data{};
    g_process_event(actor, api.get_location, data.data());
    std::memcpy(&out.position, data.data() + api.loc_return, sizeof(Vec3));
    data.fill(0);
    g_process_event(actor, api.get_rotation, data.data());
    std::memcpy(&out.rotation, data.data() + api.rot_return, sizeof(Rotator));
    data.fill(0);
    g_process_event(actor, api.get_scale, data.data());
    std::memcpy(&out.scale, data.data() + api.scale_return, sizeof(Vec3));
    const float* components = &out.position.x;
    const float* rotation = &out.rotation.pitch;
    const float* scale = &out.scale.x;
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(components[i]) || std::abs(components[i]) > 10000000.f ||
            !std::isfinite(rotation[i]) || std::abs(rotation[i]) > 100000.f ||
            !std::isfinite(scale[i]) || std::abs(scale[i]) > 100000.f) return false;
    return true;
}
bool ReadBoundsSize(const NativeApi& api, void* actor, float yaw, Vec3& result) {
    alignas(16) std::array<uint8_t, kNativeParamsSize> data{};
    g_process_event(actor, api.get_bounds, data.data());
    Vec3 extent{};
    std::memcpy(&extent, data.data() + api.bounds_extent, sizeof(extent));
    if (std::abs(std::remainder(yaw, 180.0f)) > 85.f &&
        std::abs(std::remainder(yaw, 180.0f)) < 95.f) std::swap(extent.x, extent.y);
    result = {extent.x * 2, extent.y * 2, extent.z * 2};
    return std::isfinite(result.x) && std::isfinite(result.y) && std::isfinite(result.z) &&
           result.x > 0.01f && result.y > 0.01f && result.z > 0.01f &&
           result.x < 10000000.f && result.y < 10000000.f && result.z < 10000000.f;
}
// Refunct TAS applies map transforms through the actor's RootComponent, not
// through AActor::K2_SetActorLocation. In particular BP_TravelPipe_C can reject
// the generic actor setter while its scene component remains movable.
// All reflected parameter offsets and component handles are validated first.
struct SceneTransformApi {
    void* set_location_rotation = nullptr;
    void* set_location = nullptr;
    void* set_rotation = nullptr;
    void* set_scale = nullptr;
    int loc_input = -1, rot_input = -1, scale_input = -1;
    int teleport_combined = -1, teleport_location = -1, teleport_rotation = -1;
};

bool ValidParamOffset(int offset, size_t bytes) {
    return offset >= 0 && static_cast<size_t>(offset) <= kNativeParamsSize - bytes;
}

// Unreal may return a different (but equivalent) FRotator triple after a
// 90-degree pitch (Euler gimbal lock). Compare the *actual orientation*, not
// pitch/yaw/roll independently. The basis matches UE4 FRotationMatrix::Make.
struct RotationBasis {
    Vec3 forward{}, right{}, up{};
};
RotationBasis ToRotationBasis(const Rotator& angles) {
    constexpr double kRad = 3.14159265358979323846 / 180.0;
    const double p = static_cast<double>(angles.pitch) * kRad;
    const double y = static_cast<double>(angles.yaw) * kRad;
    const double r = static_cast<double>(angles.roll) * kRad;
    const double sp = std::sin(p), cp = std::cos(p);
    const double sy = std::sin(y), cy = std::cos(y);
    const double sr = std::sin(r), cr = std::cos(r);
    return {{static_cast<float>(cp * cy), static_cast<float>(cp * sy), static_cast<float>(sp)},
            {static_cast<float>(sr * sp * cy - cr * sy),
             static_cast<float>(sr * sp * sy + cr * cy), static_cast<float>(-sr * cp)},
            {static_cast<float>(-cr * sp * cy - sr * sy),
             static_cast<float>(-cr * sp * sy + sr * cy), static_cast<float>(cr * cp)}};
}
bool DirectionsMatch(Vec3 a, Vec3 b) {
    // Less than 2 degrees of angular error, including the gimbal-lock case.
    constexpr float kMinDot = 0.999390827f; // cos(2 degrees)
    return a.x * b.x + a.y * b.y + a.z * b.z >= kMinDot;
}
bool OrientationsMatch(const Rotator& actual, const Rotator& desired) {
    const auto a = ToRotationBasis(actual);
    const auto b = ToRotationBasis(desired);
    return DirectionsMatch(a.forward, b.forward) &&
           DirectionsMatch(a.right, b.right) && DirectionsMatch(a.up, b.up);
}
bool ScalesMatch(const Vec3& actual, const Vec3& desired) {
    // Near-zero scale remains an absolute comparison; large map objects are
    // compared relatively to avoid rejecting normal UE4 float roundoff.
    const auto scaleWithinTolerance = [](float a, float b) {
        const float epsilon = 0.2f + 0.005f * std::max(std::abs(a), std::abs(b));
        return std::abs(a - b) <= epsilon;
    };
    return scaleWithinTolerance(actual.x, desired.x) && scaleWithinTolerance(actual.y, desired.y) &&
           scaleWithinTolerance(actual.z, desired.z);
}
bool PositionsMatch(const Vec3& actual, const Vec3& desired) {
    return std::abs(actual.x - desired.x) < 4.f &&
           std::abs(actual.y - desired.y) < 4.f &&
           std::abs(actual.z - desired.z) < 4.f;
}
bool PoseMatches(const NativeApi& api, void* actor, const ActorPose& desired) {
    ActorPose actual{};
    return ReadPose(api, actor, actual) &&
           PositionsMatch(actual.position, desired.position) &&
           OrientationsMatch(actual.rotation, desired.rotation) &&
           ScalesMatch(actual.scale, desired.scale);
}

bool ResolveSceneRoot(const ActorHandle& actor, ActorHandle& scene) {
    if (!ValidHandle(actor)) return false;
    const int offset = FindClassPropertyOffset(actor.klass, L"RootComponent");
    if (offset < 0 || offset > 0x4000 ||
        !IsReadable(static_cast<const uint8_t*>(actor.actor) + offset, sizeof(void*))) return false;
    void* component = nullptr;
    std::memcpy(&component, static_cast<const uint8_t*>(actor.actor) + offset, sizeof(component));
    scene = MakeHandle(component, ObjectArray());
    return scene.actor && ValidHandle(scene);
}

bool ResolveSceneTransformApi(void* component, SceneTransformApi& api) {
    if (!component) return false;
    api.set_location_rotation = FindReflectedFunction(component, L"K2_SetWorldLocationAndRotation");
    if (api.set_location_rotation) {
        api.loc_input = FindClassPropertyOffset(api.set_location_rotation, L"NewLocation");
        api.rot_input = FindClassPropertyOffset(api.set_location_rotation, L"NewRotation");
        api.teleport_combined = FindClassPropertyOffset(api.set_location_rotation, L"bTeleport");
        if (!ValidParamOffset(api.loc_input, sizeof(Vec3)) ||
            !ValidParamOffset(api.rot_input, sizeof(Rotator)))
            api.set_location_rotation = nullptr;
    }
    if (!api.set_location_rotation) {
        // Older UE4 builds may expose the two Blueprint setters separately.
        api.set_location = FindReflectedFunction(component, L"K2_SetWorldLocation");
        api.set_rotation = FindReflectedFunction(component, L"K2_SetWorldRotation");
        if (!api.set_location || !api.set_rotation) return false;
        api.loc_input = FindClassPropertyOffset(api.set_location, L"NewLocation");
        api.rot_input = FindClassPropertyOffset(api.set_rotation, L"NewRotation");
        api.teleport_location = FindClassPropertyOffset(api.set_location, L"bTeleport");
        api.teleport_rotation = FindClassPropertyOffset(api.set_rotation, L"bTeleport");
        if (!ValidParamOffset(api.loc_input, sizeof(Vec3)) ||
            !ValidParamOffset(api.rot_input, sizeof(Rotator))) return false;
    }
    api.set_scale = FindReflectedFunction(component, L"SetWorldScale3D");
    if (!api.set_scale) return false;
    // Refunct v9.30's SetWorldScale3D Blueprint function names its
    // FVector parameter "NewScale" (verified against the TAS map editor).
    // Other UE4 builds may expose the alias "NewScale3D".
    api.scale_input = FindClassPropertyOffset(api.set_scale, L"NewScale");
    if (!ValidParamOffset(api.scale_input, sizeof(Vec3)))
        api.scale_input = FindClassPropertyOffset(api.set_scale, L"NewScale3D");
    return ValidParamOffset(api.scale_input, sizeof(Vec3));
}

bool ScenePoseSupported(const ActorHandle& actor) {
    ActorHandle root{};
    SceneTransformApi scene_api{};
    return ResolveSceneRoot(actor, root) && ResolveSceneTransformApi(root.actor, scene_api);
}

// Only called after a preflight failure; keep the happy path allocation-free.
std::wstring ScenePoseFailureReason(const ActorHandle& actor) {
    ActorHandle root{};
    if (!ResolveSceneRoot(actor, root)) return L"RootComponent missing or invalid";
    if (!FindReflectedFunction(root.actor, L"K2_SetWorldLocationAndRotation") &&
        (!FindReflectedFunction(root.actor, L"K2_SetWorldLocation") ||
         !FindReflectedFunction(root.actor, L"K2_SetWorldRotation")))
        return L"scene location/rotation function missing";
    void* scale = FindReflectedFunction(root.actor, L"SetWorldScale3D");
    if (!scale) return L"SetWorldScale3D function missing";
    if (!ValidParamOffset(FindClassPropertyOffset(scale, L"NewScale"), sizeof(Vec3)) &&
        !ValidParamOffset(FindClassPropertyOffset(scale, L"NewScale3D"), sizeof(Vec3)))
        return L"NewScale parameter missing";
    return L"scene transform parameter layout unsupported";
}

void ApplySceneTeleportFlag(std::array<uint8_t, kNativeParamsSize>& data, int offset) {
    if (ValidParamOffset(offset, sizeof(uint8_t)))
        data[static_cast<size_t>(offset)] = 1; // teleport, no collision sweep
}

bool SetPoseViaScene(const NativeApi& api, const ActorHandle& actor, const ActorPose& pose) {
    ActorHandle scene{};
    SceneTransformApi scene_api{};
    if (!ResolveSceneRoot(actor, scene) ||
        !ResolveSceneTransformApi(scene.actor, scene_api)) return false;
    alignas(16) std::array<uint8_t, kNativeParamsSize> data{};
    if (scene_api.set_location_rotation) {
        std::memcpy(data.data() + scene_api.loc_input, &pose.position, sizeof(Vec3));
        std::memcpy(data.data() + scene_api.rot_input, &pose.rotation, sizeof(Rotator));
        ApplySceneTeleportFlag(data, scene_api.teleport_combined);
        g_process_event(scene.actor, scene_api.set_location_rotation, data.data());
    } else {
        std::memcpy(data.data() + scene_api.loc_input, &pose.position, sizeof(Vec3));
        ApplySceneTeleportFlag(data, scene_api.teleport_location);
        g_process_event(scene.actor, scene_api.set_location, data.data());
        data.fill(0);
        std::memcpy(data.data() + scene_api.rot_input, &pose.rotation, sizeof(Rotator));
        ApplySceneTeleportFlag(data, scene_api.teleport_rotation);
        g_process_event(scene.actor, scene_api.set_rotation, data.data());
    }
    data.fill(0);
    std::memcpy(data.data() + scene_api.scale_input, &pose.scale, sizeof(Vec3));
    g_process_event(scene.actor, scene_api.set_scale, data.data());
    return PoseMatches(api, actor.actor, pose);
}

bool SetPoseViaActor(const NativeApi& api, const ActorHandle& actor, const ActorPose& pose) {
    if (!ValidHandle(actor)) return false;
    alignas(16) std::array<uint8_t, kNativeParamsSize> data{};
    std::memcpy(data.data() + api.loc_input, &pose.position, sizeof(Vec3));
    g_process_event(actor.actor, api.set_location, data.data());
    data.fill(0);
    std::memcpy(data.data() + api.rot_input, &pose.rotation, sizeof(Rotator));
    g_process_event(actor.actor, api.set_rotation, data.data());
    data.fill(0);
    std::memcpy(data.data() + api.scale_input, &pose.scale, sizeof(Vec3));
    g_process_event(actor.actor, api.set_scale, data.data());
    return PoseMatches(api, actor.actor, pose);
}

bool SetPose(const NativeApi& api, const NativeActor& item, const ActorPose& pose) {
    if (!ValidHandle(item.handle)) return false;
    // Pipes, lifts and jump pads use component transforms in the TAS editor.
    // Keep the established actor path first for platforms/cubes/buttons.
    const bool scene_first = item.kind >= 3;
    if (scene_first) {
        if (SetPoseViaScene(api, item.handle, pose) ||
            SetPoseViaActor(api, item.handle, pose)) return true;
    } else if (SetPoseViaActor(api, item.handle, pose) ||
               SetPoseViaScene(api, item.handle, pose)) return true;

    // Diagnostic emitted only on a failed map application, not per frame.
    const auto& header = *static_cast<const UObjectHeader*>(item.handle.actor);
    std::wstring name = UnrealName(header.name);
    if (name.empty()) name = L"<unnamed>";
    ActorPose actual{};
    const bool readable = ReadPose(api, item.handle.actor, actual);
    const std::wstring detail = readable
        ? L"; position=" + std::wstring(PositionsMatch(actual.position, pose.position) ? L"ok" : L"wrong") +
          L", orientation=" + std::wstring(OrientationsMatch(actual.rotation, pose.rotation) ? L"ok" : L"wrong") +
          L", scale=" + std::wstring(ScalesMatch(actual.scale, pose.scale) ? L"ok" : L"wrong") +
          L"; target PRY=(" + std::to_wstring(pose.rotation.pitch) + L"," +
          std::to_wstring(pose.rotation.yaw) + L"," + std::to_wstring(pose.rotation.roll) +
          L"), actual PRY=(" + std::to_wstring(actual.rotation.pitch) + L"," +
          std::to_wstring(actual.rotation.yaw) + L"," + std::to_wstring(actual.rotation.roll) +
          L"); target scale=(" + std::to_wstring(pose.scale.x) + L"," +
          std::to_wstring(pose.scale.y) + L"," + std::to_wstring(pose.scale.z) +
          L"), actual scale=(" + std::to_wstring(actual.scale.x) + L"," +
          std::to_wstring(actual.scale.y) + L"," + std::to_wstring(actual.scale.z) + L")"
        : L"; actor pose unreadable";
    Log((L"Custom maps: transform mismatch for " + name + L" (section " +
         std::to_wstring(item.cluster + 1) + L", kind " + std::to_wstring(item.kind) +
         L", element " + std::to_wstring(item.element) + L"); target XYZ=(" +
         std::to_wstring(pose.position.x) + L"," + std::to_wstring(pose.position.y) +
         L"," + std::to_wstring(pose.position.z) + L"), actual XYZ=" +
         (readable ? L"(" + std::to_wstring(actual.position.x) + L"," +
          std::to_wstring(actual.position.y) + L"," + std::to_wstring(actual.position.z) +
          L")" : L"unavailable") + L", scene component=" +
         (ScenePoseSupported(item.handle) ? L"available" : L"unavailable") +
         detail).c_str());
    return false;
}

bool ResolveMapNativeApi(void* actor, NativeApi& api) {
    if (!g_process_event || !IsReadable(actor, sizeof(UObjectHeader))) return false;
    api.get_location = FindReflectedFunction(actor, L"K2_GetActorLocation");
    api.get_rotation = FindReflectedFunction(actor, L"K2_GetActorRotation");
    api.get_scale = FindReflectedFunction(actor, L"GetActorScale3D");
    api.get_bounds = FindReflectedFunction(actor, L"GetActorBounds");
    api.set_location = FindReflectedFunction(actor, L"K2_SetActorLocation");
    api.set_rotation = FindReflectedFunction(actor, L"K2_SetActorRotation");
    api.set_scale = FindReflectedFunction(actor, L"SetActorScale3D");
    if (!api.get_location || !api.get_rotation || !api.get_scale || !api.get_bounds ||
        !api.set_location || !api.set_rotation || !api.set_scale) return false;
    api.loc_return = FindClassPropertyOffset(api.get_location, L"ReturnValue");
    api.rot_return = FindClassPropertyOffset(api.get_rotation, L"ReturnValue");
    api.scale_return = FindClassPropertyOffset(api.get_scale, L"ReturnValue");
    api.bounds_extent = FindClassPropertyOffset(api.get_bounds, L"BoxExtent");
    api.loc_input = FindClassPropertyOffset(api.set_location, L"NewLocation");
    api.rot_input = FindClassPropertyOffset(api.set_rotation, L"NewRotation");
    api.scale_input = FindClassPropertyOffset(api.set_scale, L"NewScale3D");
    for (int offset : {api.loc_return, api.rot_return, api.scale_return, api.bounds_extent,
                       api.loc_input, api.rot_input, api.scale_input})
        if (offset < 0 || offset > static_cast<int>(kNativeParamsSize - sizeof(Vec3))) return false;
    return true;
}

bool ReadObjectList(const ActorHandle& level, int offset, int expected_count,
                    std::vector<ActorHandle>& output) {
    if (!ValidHandle(level) || offset < 0 || offset > 0x4000 ||
        !IsReadable(static_cast<const uint8_t*>(level.actor) + offset,
                    sizeof(UObjectPointerArray))) return false;
    UObjectPointerArray items{};
    std::memcpy(&items, static_cast<const uint8_t*>(level.actor) + offset, sizeof(items));
    if (items.count != expected_count || items.capacity < items.count ||
        items.capacity > 10000 || (items.count &&
        (!items.data || !IsReadable(items.data, sizeof(void*) * static_cast<size_t>(items.count)))))
        return false;
    const auto* objects = ObjectArray();
    for (int i = 0; i < items.count; ++i) {
        const auto ref = MakeHandle(items.data[i], objects);
        if (!ref.actor) return false;
        output.push_back(ref);
    }
    return true;
}

// The TAS map version 1 stores non-array actors (lifts/pipes/pads) in
// independently indexed lists. These assignments match the Refunct TAS
// map-editor's native BP actor names, not guesses from proximity to a section.
int MiscCluster(int kind, const std::wstring& name, float x) {
    if (kind == 3) {
        if (name == L"BP_Lift_C_1") return 5;
        if (name == L"BP_Mover7") return 7;
        if (name == L"BP_Mover6") return 8;
    } else if (kind == 4) {
        if (name == L"BP_TravelPipe_C_6" || name == L"BP_TravelPipe_C_5") return 4;
        if (name == L"BP_TravelPipe_359") return 6;
        if (name == L"BP_TravelPipe_C_3" || name == L"BP_TravelPipe_C_2") return 9;
        if (name == L"BP_Pipe4" || name == L"BP_TravelPipe_C_1") return 18;
        if (name == L"BP_TravelPipe2_855") return 23;
        if (name == L"BP_TravelPipe_1816") return 25;
    } else if (kind == 5) {
        if (name == L"BP_Jumppad_414" && std::abs(x - 250.f) < 15.f) return 23;
        if (name == L"BP_Jumppad2_530") return 24;
        if (name == L"BP_Jumppad_414" && std::abs(x - 2075.f) < 15.f) return 24;
        if (name == L"BP_Jumppad_514") return 26;
    }
    return -1;
}

bool ReadSource(const NativeRoot& root, int offset, Vec3& source) {
    if (!ValidHandle(root.handle) || offset < 0 || offset > 0x4000 ||
        !IsReadable(static_cast<const uint8_t*>(root.handle.actor) + offset, sizeof(Vec3))) return false;
    std::memcpy(&source, static_cast<const uint8_t*>(root.handle.actor) + offset, sizeof(source));
    return std::isfinite(source.z) && std::abs(source.z) < 10000000.f;
}
bool WriteRoot(const NativeRoot& root, int source_offset, int speed_offset,
               const Vec3& source, float speed) {
    if (!ValidHandle(root.handle) || source_offset < 0 || speed_offset < 0 ||
        source_offset > 0x4000 || speed_offset > 0x4000) return false;
    auto* data = static_cast<uint8_t*>(root.handle.actor);
    if (!IsWritable(data + source_offset, sizeof(Vec3)) ||
        !IsWritable(data + speed_offset, sizeof(float))) return false;
    std::memcpy(data + source_offset, &source, sizeof(source));
    std::memcpy(data + speed_offset, &speed, sizeof(speed));
    // A successful memcpy does not guarantee native state matches our intent.
    Vec3 verified_source{};
    float verified_speed = 0.f;
    std::memcpy(&verified_source, data + source_offset, sizeof(verified_source));
    std::memcpy(&verified_speed, data + speed_offset, sizeof(verified_speed));
    return std::memcmp(&verified_source, &source, sizeof(source)) == 0 &&
           std::memcmp(&verified_speed, &speed, sizeof(speed)) == 0;
}
int s_source_offset = -1, s_speed_offset = -1;

bool BuildSnapshot(const Map& target) {
    if (!g_practice_level_class || !g_practice_level_actors[0])
        if (!DiscoverPracticeButtons()) { MapError(L"native level roots not ready"); return false; }
    const auto* objects = ObjectArray();
    if (!objects) { MapError(L"global object array unavailable"); return false; }
    NativeApi api{};
    if (!ResolveMapNativeApi(g_practice_level_actors[0], api)) {
        MapError(L"required native actor transform functions not available"); return false;
    }
    const int source_offset = FindClassPropertyOffset(g_practice_level_class, L"SourcePosition");
    const int speed_offset = FindClassPropertyOffset(g_practice_level_class, L"Speed");
    const std::array<int,3> list_offsets = {
        FindClassPropertyOffset(g_practice_level_class, L"FertileLands"),
        FindClassPropertyOffset(g_practice_level_class, L"Collectibles"),
        FindClassPropertyOffset(g_practice_level_class, L"Buttons")
    };
    if (source_offset < 0 || speed_offset < 0 ||
        std::any_of(list_offsets.begin(), list_offsets.end(), [](int x){return x < 0;})) {
        MapError(L"required native level properties not found"); return false;
    }
    std::array<NativeRoot, custom_maps::kClusterCount> roots{};
    std::array<std::array<std::vector<ActorHandle>, 6>, custom_maps::kClusterCount> per_cluster;
    for (size_t cluster = 0; cluster < roots.size(); ++cluster) {
        auto& root = roots[cluster];
        root.handle = MakeHandle(g_practice_level_actors[cluster], objects);
        ActorPose pose{};
        if (!root.handle.actor || !ReadPose(api, root.handle.actor, pose) ||
            !ReadSource(root, source_offset, root.source)) {
            MapError(L"invalid native section root " + std::to_wstring(cluster+1)); return false;
        }
        root.original_location = pose.position;
        root.rotation = pose.rotation;
        const auto* ptr = static_cast<const uint8_t*>(root.handle.actor) + speed_offset;
        if (!IsReadable(ptr, sizeof(float))) return false;
        std::memcpy(&root.speed, ptr, sizeof(root.speed));
        if (!std::isfinite(root.speed) || root.speed <= 0.f || root.speed > 10000000.f) {
            MapError(L"invalid native section rise speed"); return false;
        }
        for (int kind = 0; kind < 3; ++kind) {
            if (!ReadObjectList(root.handle, list_offsets[kind],
                static_cast<int>(target.clusters[cluster].elements[kind].size()),
                per_cluster[cluster][kind])) {
                MapError(L"native actor count differs from map in section " +
                         std::to_wstring(cluster+1)); return false;
            }
        }
    }
    std::unordered_map<void*, int> class_kind;
    for (int32_t i = 0; i < objects->num_elements; ++i) {
        const auto& slot = objects->objects[i];
        if (!slot.object || static_cast<uint32_t>(slot.flags) & 0x30000000u ||
            !IsReadable(slot.object, sizeof(UObjectHeader))) continue;
        void* klass = slot.object->class_ptr;
        if (!klass || !IsReadable(klass, sizeof(UObjectHeader))) continue;
        auto it = class_kind.find(klass);
        if (it == class_kind.end()) {
            const auto cls = UnrealName(static_cast<const UObjectHeader*>(klass)->name);
            const int kind = cls == L"BP_Lift_C" ? 3 :
                             cls == L"BP_TravelPipe_C" ? 4 :
                             cls == L"BP_Jumppad_C" ? 5 : -1;
            it = class_kind.emplace(klass, kind).first;
        }
        if (it->second < 3) continue;
        const std::wstring name = UnrealName(slot.object->name);
        if (name.rfind(L"Default__", 0) == 0) continue;
        ActorPose pose{};
        if (!ReadPose(api, slot.object, pose)) continue;
        const int cluster = MiscCluster(it->second, name, pose.position.x);
        if (cluster < 0) continue;
        per_cluster[cluster][it->second].push_back(MakeHandle(slot.object, objects));
    }
    // Pipes are indexed in their section by native X/Y/Z; the TAS exporter
    // explicitly sorts these before assigning the numbered JSON elements.
    for (size_t cluster = 0; cluster < roots.size(); ++cluster) {
        auto& pipes = per_cluster[cluster][4];
        std::sort(pipes.begin(), pipes.end(), [&](const ActorHandle& a, const ActorHandle& b) {
            ActorPose left{}, right{};
            if (!ReadPose(api,a.actor,left) || !ReadPose(api,b.actor,right)) return a.index < b.index;
            if (left.position.x != right.position.x) return left.position.x < right.position.x;
            if (left.position.y != right.position.y) return left.position.y < right.position.y;
            return left.position.z < right.position.z;
        });
        for (int kind = 3; kind < 6; ++kind)
            if (per_cluster[cluster][kind].size() != target.clusters[cluster].elements[kind].size()) {
                MapError(L"native lift/pipe/jump-pad count differs in section " +
                         std::to_wstring(cluster+1)); return false;
            }
    }
    std::vector<NativeActor> actors;
    actors.reserve(kTotalMapActors);
    for (size_t cluster = 0; cluster < roots.size(); ++cluster) {
        for (int kind = 0; kind < 6; ++kind) {
            for (size_t index = 0; index < per_cluster[cluster][kind].size(); ++index) {
                auto ref = per_cluster[cluster][kind][index];
                NativeActor actor{};
                actor.handle = ref;
                actor.cluster = static_cast<int>(cluster);
                actor.kind = kind;
                actor.element = static_cast<int>(index);
                if (!ValidHandle(ref) || !ReadPose(api, ref.actor, actor.original) ||
                    !ReadBoundsSize(api, ref.actor, actor.original.rotation.yaw,
                                    actor.original_size)) {
                    MapError(L"cannot capture baseline actor transform in section " +
                             std::to_wstring(cluster+1)); return false;
                }
                actors.push_back(actor);
            }
        }
    }
    if (actors.size() != kTotalMapActors) {
        MapError(L"unexpected native map actor count"); return false;
    }
    s_map.actors = std::move(actors);
    s_map.roots = roots;
    s_map.api = api;
    s_source_offset = source_offset;
    s_speed_offset = speed_offset;
    s_map.snapshot = true;
    return true;
}

bool CurrentSnapshotValid() {
    if (!s_map.snapshot || s_map.actors.size() != kTotalMapActors) return false;
    for (size_t i = 0; i < s_map.roots.size(); ++i)
        if (!ValidHandle(s_map.roots[i].handle) ||
            g_practice_level_actors[i] != s_map.roots[i].handle.actor) return false;
    // Validating every actor is cheap only when loading or restarting a map.
    for (const auto& actor : s_map.actors) if (!ValidHandle(actor.handle)) return false;
    return true;
}

void MarkMapAsCheat() {
    g_recording_tainted = true;
    if (!g_pb_cheats_allowed_for_run) g_run_pb_eligible = false;
}

bool BeginApplyRoots(bool restore, const Map* requested_map = nullptr) {
    // Snapshot the *current* native fields, not the pristine map. A failed map
    // switch must preserve the previously active map's root positions/speeds.
    struct RootFields { Vec3 source{}; float speed = 0.f; };
    std::array<RootFields, custom_maps::kClusterCount> previous{};
    for (size_t i = 0; i < s_map.roots.size(); ++i) {
        const auto& root = s_map.roots[i];
        if (!ValidHandle(root.handle) || s_source_offset < 0 || s_speed_offset < 0 ||
            s_source_offset > 0x4000 || s_speed_offset > 0x4000) return false;
        const auto* bytes = static_cast<const uint8_t*>(root.handle.actor);
        if (!IsReadable(bytes + s_source_offset, sizeof(Vec3)) ||
            !IsReadable(bytes + s_speed_offset, sizeof(float)) ||
            !IsWritable(bytes + s_source_offset, sizeof(Vec3)) ||
            !IsWritable(bytes + s_speed_offset, sizeof(float))) return false;
        std::memcpy(&previous[i].source, bytes + s_source_offset, sizeof(Vec3));
        std::memcpy(&previous[i].speed, bytes + s_speed_offset, sizeof(float));
    }
    const Map& map = requested_map ? *requested_map : s_map.loaded_map;
    for (size_t i = 0; i < s_map.roots.size(); ++i) {
        auto source = s_map.roots[i].source;
        float speed = s_map.roots[i].speed;
        if (!restore) {
            source.z = map.clusters[i].z;
            speed = map.clusters[i].rise_speed;
        }
        if (WriteRoot(s_map.roots[i], s_source_offset, s_speed_offset, source, speed)) continue;
        // Include the failed root: a setter may have changed SourcePosition
        // before failing to update Speed. Rollback is best-effort if the
        // engine actually destroyed a root during this operation.
        bool recovered = true;
        for (size_t j = 0; j <= i; ++j)
            if (!WriteRoot(s_map.roots[j], s_source_offset, s_speed_offset,
                           previous[j].source, previous[j].speed)) recovered = false;
        if (!recovered) Log(L"Custom maps: native root rollback incomplete; reload the level");
        return false;
    }
    return true;
}

bool ApplyNextActor(const NativeActor& item, bool restore) {
    if (!ValidHandle(item.handle) || item.cluster < 0 ||
        static_cast<size_t>(item.cluster) >= s_map.roots.size()) return false;
    const auto& root_handle = s_map.roots[item.cluster].handle;
    if (!ValidHandle(root_handle) ||
        g_practice_level_actors[item.cluster] != root_handle.actor) return false;
    ActorPose target = item.original;
    ActorPose root_pose{};
    if (!ReadPose(s_map.api, s_map.roots[item.cluster].handle.actor, root_pose)) return false;
    if (restore) {
        // An automatic section rise can happen while the custom map is active.
        // Restore the original actor layout relative to its current level root,
        // not at an obsolete world-Z from before the section was raised.
        const auto& root = s_map.roots[item.cluster];
        target.position.x += root_pose.position.x - root.original_location.x;
        target.position.y += root_pose.position.y - root.original_location.y;
        target.position.z += root_pose.position.z - root.original_location.z;
        target.rotation.pitch += root_pose.rotation.pitch - root.rotation.pitch;
        target.rotation.yaw += root_pose.rotation.yaw - root.rotation.yaw;
        target.rotation.roll += root_pose.rotation.roll - root.rotation.roll;
    } else {
        const auto& e = s_map.loaded_map.clusters[item.cluster].elements[item.kind][item.element];
        target.position = {e.x, e.y, e.z + root_pose.position.z};
        target.rotation = {e.pitch + root_pose.rotation.pitch,
                           e.yaw + root_pose.rotation.yaw,
                           e.roll + root_pose.rotation.roll};
        target.scale = {item.original.scale.x * e.sizex / item.original_size.x,
                        item.original.scale.y * e.sizey / item.original_size.y,
                        item.original.scale.z * e.sizez / item.original_size.z};
    }
    for (float component : {target.scale.x, target.scale.y, target.scale.z})
        if (!std::isfinite(component) || component == 0.f || std::abs(component) > 100000.f)
            return false;
    return SetPose(s_map.api, item, target);
}

bool AllowedMapName(const wchar_t* name) {
    if (!name || !*name || wcslen(name) > 180 || name[0] == L'.') return false;
    if (wcschr(name, L'\\') || wcschr(name, L'/') || wcschr(name, L':')) return false;
    const wchar_t* extension = wcsrchr(name, L'.');
    return !extension || !_wcsicmp(extension, L".json") ||
           !_wcsicmp(extension, L".map") || !_wcsicmp(extension, L".txt");
}

bool ReadMapFile(const std::wstring& filename, Map& map) {
    if (!AllowedMapName(filename.c_str())) return false;
    const std::wstring path = MapFolder() + L"\\" + filename;
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) { MapError(L"could not open " + filename); return false; }
    struct Closer { HANDLE fd; ~Closer() { if (fd != INVALID_HANDLE_VALUE) CloseHandle(fd); } } close{handle};
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle, &size) || size.QuadPart <= 0 ||
        size.QuadPart > static_cast<LONGLONG>(custom_maps::kMapMaxBytes)) {
        MapError(L"map file too large or empty"); return false;
    }
    std::string text(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    if (!ReadFile(handle, text.data(), static_cast<DWORD>(text.size()), &read, nullptr) ||
        read != text.size()) { MapError(L"failed to read map file"); return false; }
    std::string error;
    if (!custom_maps::Decode(text, map, error)) {
        MapError(L"invalid map " + filename + L": " + ToWideAscii(error)); return false;
    }
    return true;
}
} // namespace

void RefreshCustomMapFiles() {
    s_map.files.clear();
    const std::wstring folder = MapFolder();
    if (folder.empty()) { MapError(L"storage folder not initialized"); return; }
    if (!EnsureDirectory(folder)) { MapError(L"cannot create Maps folder"); return; }
    WIN32_FIND_DATAW found{};
    const HANDLE search = FindFirstFileW((folder + L"\\*").c_str(), &found);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if ((found.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
                !AllowedMapName(found.cFileName)) continue;
            s_map.files.emplace_back(found.cFileName);
        } while (FindNextFileW(search, &found));
        FindClose(search);
    }
    std::sort(s_map.files.begin(), s_map.files.end(), [](const auto& a, const auto& b) {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    });
    if (s_map.files.size() > kMaxMapFiles) s_map.files.resize(kMaxMapFiles);
}
size_t CustomMapFileCount() { return s_map.files.size(); }
std::wstring CustomMapFileLabel(size_t index) {
    return index < s_map.files.size() ? s_map.files[index] : L"";
}
std::wstring CustomMapStatus() { return s_map.status; }
bool CustomMapActive() { return s_map.active || (s_map.applying && !s_map.restoring); }
bool CustomMapApplying() { return s_map.applying; }

bool RequestCustomMapLoad(size_t index) {
    if (index >= s_map.files.size() || s_map.applying ||
        s_map.awaiting_original_reload) return false;
    if (s_map.original_reload_unconfirmed) {
        MapError(L"previous full-level reload was not confirmed; reload the level before loading a map");
        return false;
    }
    Map parsed;
    if (!ReadMapFile(s_map.files[index], parsed)) return false;
    if (!CurrentSnapshotValid()) {
        // Never capture a new "original" baseline from a custom-mapped world.
        // Without the original handles we cannot reliably undo old transforms.
        if (s_map.active) {
            MapError(L"custom map snapshot expired; reload the level before switching maps");
            return false;
        }
        s_map.snapshot = false;
        s_map.actors.clear();
        if (!BuildSnapshot(parsed)) return false;
    } else {
        for (const auto& actor : s_map.actors)
            if (static_cast<size_t>(actor.element) >=
                parsed.clusters[actor.cluster].elements[actor.kind].size()) {
                MapError(L"new map has a different native actor layout"); return false;
            }
    }
    // Reject impossible transforms before changing the game's first actor.
    // Their sizes are mapped from the pristine snapshot (not from a previously
    // loaded custom map), so switching maps never compounds scale changes.
    for (const auto& actor : s_map.actors) {
        const auto& element = parsed.clusters[actor.cluster].elements[actor.kind][actor.element];
        for (float scale : {
                actor.original.scale.x * element.sizex / actor.original_size.x,
                actor.original.scale.y * element.sizey / actor.original_size.y,
                actor.original.scale.z * element.sizez / actor.original_size.z}) {
            if (!std::isfinite(scale) || scale == 0.f || std::abs(scale) > 100000.f) {
                MapError(L"map requests an unsafe actor scale"); return false;
            }
        }
    }
    // Pipes are the first objects whose generic AActor setters can be refused
    // by UE4. Fail before modifying any section if their scene API is missing.
    for (const auto& actor : s_map.actors) {
        if (actor.kind == 4 && !ScenePoseSupported(actor.handle)) {
            MapError(L"pipe scene-component transform unavailable in section " +
                     std::to_wstring(actor.cluster + 1) + L" (" +
                     ScenePoseFailureReason(actor.handle) + L")");
            return false;
        }
    }
    // Keep the previous map selection and its native roots intact if the new
    // map cannot be applied. The operation itself has a rollback guard.
    if (!BeginApplyRoots(false, &parsed)) {
        MapError(L"cannot update section source locations"); return false;
    }
    s_map.loaded_map = std::move(parsed);
    s_map.current_file = s_map.files[index];
    s_map.cursor = 0;
    s_map.applying = true;
    s_map.restoring = false;
    s_map.active = false;
    s_map.restart_after_change = true;
    const auto* state = PracticeLevelState();
    s_map.last_resets = state ? state->resets : -1;
    s_map.restart_reapply_delay = 0;
    s_map.status = L"Loading " + s_map.current_file + L"...";
    MarkMapAsCheat();
    Notify(L"Applying custom map (assisted run)");
    return true;
}
// Actor snapshots cannot be trusted after a full Unreal level transition.
// Queue the existing reflected OpenLevel path instead of touching stale actors.
bool QueueOriginalWorldReload(const wchar_t* reason) {
    Log((std::wstring(L"Custom maps: ") + reason +
         L"; requesting full level reload").c_str());
    if (!RequestFullLevelReload()) {
        // Recovery is required even when the reload request was rejected.
        // Never treat the partly modified world as an original baseline.
        s_map.original_reload_unconfirmed = true;
        MapError(L"original map restore needs a full level reload, but it is unavailable");
        return false;
    }
    s_map.awaiting_original_reload = true;
    // OpenLevel already resets the run; never queue an additional R restart.
    s_map.restart_after_change = false;
    s_map.restart_reapply_delay = 0;
    s_map.status = L"Reloading original world...";
    MarkMapAsCheat();
    return true;
}

bool RequestCustomMapRestore() {
    if (s_map.applying || s_map.awaiting_original_reload) return false;
    if (s_map.original_reload_unconfirmed)
        return QueueOriginalWorldReload(L"previous full-level reload was not confirmed");
    if (!CurrentSnapshotValid()) {
        if (!s_map.snapshot && !s_map.active && s_map.current_file.empty() &&
            !s_map.original_reload_unconfirmed) {
            s_map.status = L"Original world already active";
            Notify(s_map.status);
            return true;
        }
        // An expired snapshot must be recovered by the engine, not by writes
        // to the previous world's root components.
        return QueueOriginalWorldReload(L"original world snapshot expired");
    }
    if (!BeginApplyRoots(true))
        return QueueOriginalWorldReload(L"cannot restore native section roots");
    s_map.cursor = 0;
    s_map.applying = true;
    s_map.restoring = true;
    s_map.restart_after_change = true;
    s_map.status = L"Restoring original world...";
    MarkMapAsCheat();
    Notify(L"Restoring original world");
    return true;
}

// Called after RestartRun succeeds and immediately before reflected OpenLevel.
// The old world's global actor objects can be destroyed during map travel.
void OnCustomMapFullLevelReloadStart() {
    s_map.original_reload_unconfirmed |= s_map.active || s_map.applying ||
        s_map.awaiting_original_reload || !s_map.current_file.empty();
    s_map.applying = false;
    s_map.restoring = false;
    s_map.active = false;
    s_map.restart_after_change = false;
    s_map.snapshot = false;
    s_map.actors.clear();
    s_map.roots = {};
    s_map.api = {};
    s_source_offset = -1;
    s_speed_offset = -1;
    s_map.loaded_map = Map{};
    s_map.current_file.clear();
    s_map.cursor = 0;
    s_map.last_resets = -1;
    s_map.restart_reapply_delay = 0;
    s_map.status = s_map.awaiting_original_reload
        ? L"Reloading original world..." : L"No custom map loaded";
}

// A different UWorld must be observed before claiming a successful restore.
// Timeout or unsupported OpenLevel cannot silently confirm restoration.
void OnCustomMapFullLevelReloadResult(bool succeeded) {
    if (succeeded) s_map.original_reload_unconfirmed = false;
    else if (s_map.awaiting_original_reload) s_map.original_reload_unconfirmed = true;
    if (!s_map.awaiting_original_reload) {
        if (!succeeded && s_map.original_reload_unconfirmed) {
            s_map.status = L"Map error: full level reload not confirmed";
            Log((L"Custom maps: " + s_map.status).c_str());
        }
        return;
    }
    s_map.awaiting_original_reload = false;
    s_map.status = succeeded ? L"Original world restored (level reloaded)"
                             : L"Map error: original world reload not confirmed";
    Log((L"Custom maps: " + s_map.status).c_str());
    Notify(succeeded ? L"Original world restored" : L"Original world reload failed - see log");
}

void UpdateCustomMapLoad() {
    if (s_map.awaiting_original_reload || s_map.original_reload_unconfirmed) return;
    if (!s_map.applying) {
        if (!s_map.active) return;
        const LevelState* state = PracticeLevelState();
        if (state && s_map.last_resets >= 0 && state->resets != s_map.last_resets) {
            s_map.last_resets = state->resets;
            s_map.restart_reapply_delay = 2; // allow native reset/actor initialization
        }
        if (s_map.restart_reapply_delay <= 0) return;
        if (--s_map.restart_reapply_delay > 0) return;
        if (!CurrentSnapshotValid()) {
            s_map.snapshot = false;
            s_map.active = false;
            s_map.actors.clear();
            s_map.restart_after_change = false;
            s_map.original_reload_unconfirmed = true;
            s_map.status = L"World reloaded - select your map again";
            return;
        }
        if (!BeginApplyRoots(false)) {
            s_map.active = false;
            MapError(L"cannot reapply map after native restart");
            return;
        }
        s_map.cursor = 0;
        s_map.applying = true;
        s_map.status = L"Reapplying " + s_map.current_file + L" after restart";
    }
    // Full 322-actor validation happens before starting the operation.
    // During the frame-batched apply, check only the root identity and the
    // specific actor being updated, never rescan every UObject each tick.
    if (!s_map.snapshot || !ValidHandle(s_map.roots[0].handle) ||
        g_practice_level_actors[0] != s_map.roots[0].handle.actor) {
        s_map.applying = false;
        s_map.active = false;
        s_map.restart_after_change = false;
        s_map.original_reload_unconfirmed = true;
        MapError(L"world actors changed during map load; reload the level");
        return;
    }
    LARGE_INTEGER start{}, frequency{}, now{};
    const bool budget_available = QueryPerformanceFrequency(&frequency) &&
                                  QueryPerformanceCounter(&start) && frequency.QuadPart > 0;
    // Apply a small batch to avoid a 322-actor material/transform spike.
    int processed = 0;
    while (s_map.cursor < s_map.actors.size() && processed < 8) {
        if (!ApplyNextActor(s_map.actors[s_map.cursor], s_map.restoring)) {
            if (!s_map.restoring) {
                // A failed actor setter cannot leave the rest of the world in
                // an unexplained half-applied state. Roll back the baseline.
                Log((L"Custom maps: actor transform failed at index " +
                     std::to_wstring(s_map.cursor) + L", starting rollback").c_str());
                s_map.restoring = true;
                s_map.restart_after_change = false; // failure rollback must not queue a restart
                s_map.cursor = 0;
                s_map.status = L"Map transform failed - restoring original world";
                if (!BeginApplyRoots(true)) {
                    s_map.applying = false;
                    s_map.active = false;
                    QueueOriginalWorldReload(L"cannot restore level roots");
                }
            } else {
                s_map.applying = false;
                s_map.active = false;
                QueueOriginalWorldReload(L"native rollback failed");
            }
            return;
        }
        ++s_map.cursor;
        ++processed;
        if (budget_available && QueryPerformanceCounter(&now) &&
            (now.QuadPart - start.QuadPart) * 1000000 / frequency.QuadPart >= 850) break;
    }
    if (s_map.cursor < s_map.actors.size()) return;
    s_map.applying = false;
    s_map.active = !s_map.restoring;
    s_map.status = s_map.active
        ? L"Loaded: " + s_map.current_file : L"Original world restored";
    Log((L"Custom maps: " + s_map.status).c_str());
    Notify(s_map.active ? L"Custom map loaded" : L"Original world restored");
    if (!s_map.active) {
        s_map.loaded_map = Map{};
        s_map.current_file.clear();
    }
    if (s_map.restart_after_change) {
        s_map.restart_after_change = false;
        // CharacterTickHook executes its normal R path later in this tick.
        g_restart_requested = true;
        Log(L"Custom maps: switch completed; normal R restart queued");
    }
}
} // namespace refunct

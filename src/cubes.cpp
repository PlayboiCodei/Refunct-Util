#include "refunct/internal_api.hpp"
#include "refunct/features/cubes.hpp"
#include "refunct/ghost_replay_cube_contact.hpp"

namespace refunct {
namespace {
constexpr wchar_t kPickupEvent[] =
    L"BndEvt__Trigger_K2Node_ComponentBoundEvent_24_ComponentBeginOverlapSignature__DelegateSignature";
constexpr size_t kArgumentCapacity = 512;
constexpr float kWaterPlaneZ = 0.0f; // Section roots rise to Z=0 on v9.30.

struct CubeActor {
    void* actor = nullptr;
    void* mesh = nullptr;
    void* trigger = nullptr;
    bool original_visible = false;
    uint8_t original_collision = 0;
    bool original_saved = false;
};

struct CubeApi {
    void* is_visible = nullptr;
    void* set_visible = nullptr;
    void* get_collision = nullptr;
    void* set_collision = nullptr;
    void* get_location = nullptr;
    void* pickup = nullptr;
    int visible_result = -1;
    int visibility = -1;
    int propagate = -1;
    int collision_result = -1;
    int collision_type = -1;
    int location_result = -1;
    int picked_offset = -1;
    int overlapped_component = -1;
    int other_actor = -1;
    int other_component = -1;
    int other_body_index = -1;
    int updated_primitive = -1;
};

struct CubeWorldState {
    std::array<CubeActor, kTotalCubes> actors{};
    CubeApi api{};
    MyCharacter* player = nullptr;
    void* cube_class = nullptr;
    int resets = -1;
    int last_level = -1;
    size_t next = 0;
    CubeWorldAction action = CubeWorldAction::RestoreDefault;
    CubeWorldAction mode = CubeWorldAction::RestoreDefault;
    bool ready = false;
    bool pending = false;
    int modified = 0;
    ULONGLONG next_refresh = 0;
};
CubeWorldState s_cubes{};

bool OffsetOK(int offset, size_t bytes = 1) {
    return offset >= 0 && static_cast<size_t>(offset) + bytes <= kArgumentCapacity;
}

bool ReadPickedUp(const CubeActor& cube, bool& picked, const CubeApi& api = s_cubes.api) {
    const auto offset = api.picked_offset;
    if (!cube.actor || offset < 0 || offset > 0x2000) return false;
    const auto* ptr = static_cast<const uint8_t*>(cube.actor) + offset;
    if (!IsReadable(ptr, 1) || (*ptr != 0 && *ptr != 1)) return false;
    picked = *ptr != 0;
    return true;
}

bool ComponentVisibility(void* component, bool& value) {
    const auto& api = s_cubes.api;
    if (!IsReadable(component, sizeof(UObjectHeader)) ||
        !api.is_visible || !OffsetOK(api.visible_result)) return false;
    alignas(16) std::array<uint8_t, kArgumentCapacity> args{};
    g_process_event(component, api.is_visible, args.data());
    if (args[api.visible_result] > 1) return false;
    value = args[api.visible_result] != 0;
    return true;
}

bool SetComponentVisibility(void* component, bool value) {
    const auto& api = s_cubes.api;
    bool current = false;
    if (!ComponentVisibility(component, current)) return false;
    if (current == value) return true;
    alignas(16) std::array<uint8_t, kArgumentCapacity> args{};
    args[api.visibility] = value ? 1 : 0;
    args[api.propagate] = 0;
    g_process_event(component, api.set_visible, args.data());
    return ComponentVisibility(component, current) && current == value;
}

bool ComponentCollision(void* component, uint8_t& value, const CubeApi& api = s_cubes.api) {
    if (!IsReadable(component, sizeof(UObjectHeader)) ||
        !api.get_collision || !OffsetOK(api.collision_result)) return false;
    alignas(16) std::array<uint8_t, kArgumentCapacity> args{};
    g_process_event(component, api.get_collision, args.data());
    value = args[api.collision_result];
    return value <= 3; // Unreal ECollisionEnabled::Type: No, Query, Physics, QueryAndPhysics.
}

bool SetComponentCollision(void* component, uint8_t value) {
    const auto& api = s_cubes.api;
    uint8_t current = 0;
    if (value > 3 || !ComponentCollision(component, current)) return false;
    if (current == value) return true;
    alignas(16) std::array<uint8_t, kArgumentCapacity> args{};
    args[api.collision_type] = value;
    g_process_event(component, api.set_collision, args.data());
    return ComponentCollision(component, current) && current == value;
}

bool IsAboveWater(const CubeActor& cube, bool& above) {
    const auto& api = s_cubes.api;
    if (!api.get_location || !OffsetOK(api.location_result, sizeof(Vec3))) return false;
    alignas(16) std::array<uint8_t, kArgumentCapacity> args{};
    g_process_event(cube.actor, api.get_location, args.data());
    Vec3 position{};
    std::memcpy(&position, args.data() + api.location_result, sizeof(position));
    if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
        !std::isfinite(position.z) || std::abs(position.z) > 100000.0f) return false;
    above = position.z >= kWaterPlaneZ;
    return true;
}

bool ResolvePickupApi(const CubeActor& cube, MyCharacter* player, CubeApi& api) {
    if (!g_process_event || !player || !IsReadable(player->movement, sizeof(UObjectHeader))) return false;
    api.get_collision = FindReflectedFunction(cube.trigger, L"GetCollisionEnabled");
    api.pickup = FindReflectedFunction(cube.actor, kPickupEvent);
    if (!api.get_collision || !api.pickup) return false;
    api.collision_result = FindClassPropertyOffset(api.get_collision, L"ReturnValue");
    api.picked_offset = FindClassPropertyOffset(static_cast<UObjectHeader*>(cube.actor)->class_ptr, L"IsPickedUp");
    api.overlapped_component = FindClassPropertyOffset(api.pickup, L"OverlappedComponent");
    api.other_actor = FindClassPropertyOffset(api.pickup, L"OtherActor");
    api.other_component = FindClassPropertyOffset(api.pickup, L"OtherComp");
    api.other_body_index = FindClassPropertyOffset(api.pickup, L"OtherBodyIndex");
    api.updated_primitive = FindClassPropertyOffset(reinterpret_cast<UObjectHeader*>(player->movement)->class_ptr, L"UpdatedPrimitive");
    return OffsetOK(api.collision_result) &&
        api.picked_offset >= 0 && api.picked_offset < 0x2000 &&
        OffsetOK(api.overlapped_component, sizeof(void*)) &&
        OffsetOK(api.other_actor, sizeof(void*)) &&
        OffsetOK(api.other_component, sizeof(void*)) &&
        OffsetOK(api.other_body_index, sizeof(int32_t)) &&
        api.updated_primitive >= 0 && api.updated_primitive < 0x2000;
}

bool ResolveApi(const CubeActor& cube, MyCharacter* player, CubeApi& api) {
    if (!ResolvePickupApi(cube, player, api)) return false;
    api.is_visible = FindReflectedFunction(cube.mesh, L"IsVisible");
    api.set_visible = FindReflectedFunction(cube.mesh, L"SetVisibility");
    api.set_collision = FindReflectedFunction(cube.trigger, L"SetCollisionEnabled");
    api.get_location = FindReflectedFunction(cube.actor, L"K2_GetActorLocation");
    if (!api.is_visible || !api.set_visible || !api.set_collision || !api.get_location) return false;
    api.visible_result = FindClassPropertyOffset(api.is_visible, L"ReturnValue");
    api.visibility = FindClassPropertyOffset(api.set_visible, L"bNewVisibility");
    api.propagate = FindClassPropertyOffset(api.set_visible, L"bPropagateToChildren");
    api.collision_type = FindClassPropertyOffset(api.set_collision, L"NewType");
    api.location_result = FindClassPropertyOffset(api.get_location, L"ReturnValue");
    return OffsetOK(api.visible_result) && OffsetOK(api.visibility) &&
        OffsetOK(api.propagate) && api.visibility != api.propagate &&
        OffsetOK(api.collision_type) && OffsetOK(api.location_result, sizeof(Vec3));
}

bool DiscoverCubeActors(std::array<CubeActor, kTotalCubes>& candidate, void*& cube_class, bool require_mesh) {
    if (!g_practice_level_class || !g_practice_level_actors[0]) {
        if (!DiscoverPracticeButtons()) return false;
    }
    if (!g_practice_level_class || !g_practice_level_actors[0]) return false;
    const int offset = FindClassPropertyOffset(g_practice_level_class, L"Collectibles");
    if (offset < 0 || offset > 0x2000) return false;
    candidate = {};
    cube_class = nullptr;
    size_t found = 0;
    for (void* root : g_practice_level_actors) {
        if (!IsReadable(root, sizeof(UObjectHeader)) ||
            static_cast<UObjectHeader*>(root)->class_ptr != g_practice_level_class) return false;
        const auto* array_ptr = static_cast<const uint8_t*>(root) + offset;
        if (!IsReadable(array_ptr, sizeof(UObjectPointerArray))) return false;
        const auto& array = *reinterpret_cast<const UObjectPointerArray*>(array_ptr);
        if (array.count < 0 || array.count > kTotalCubes ||
            array.capacity < array.count || array.capacity > 10000 ||
            (array.count && (!array.data || !IsReadable(array.data, sizeof(void*) * array.count))))
            return false;
        for (int i = 0; i < array.count; ++i) {
            if (found >= candidate.size()) return false;
            void* actor = array.data[i];
            if (!IsReadable(actor, sizeof(UObjectHeader))) return false;
            auto* klass = static_cast<UObjectHeader*>(actor)->class_ptr;
            if (!IsReadable(klass, sizeof(UObjectHeader))) return false;
            if (!cube_class) {
                if (UnrealName(static_cast<UObjectHeader*>(klass)->name) != L"BP_PowerCore_C")
                    return false;
                cube_class = klass;
            }
            if (klass != cube_class) return false;
            candidate[found++].actor = actor;
        }
    }
    if (found != kTotalCubes || !cube_class) return false;
    const int mesh_offset = FindClassPropertyOffset(cube_class, L"Mesh");
    const int trigger_offset = FindClassPropertyOffset(cube_class, L"Trigger");
    if ((require_mesh && (mesh_offset < 0 || mesh_offset > 0x2000)) ||
        trigger_offset < 0 || trigger_offset > 0x2000) return false;
    for (CubeActor& cube : candidate) {
        const uint8_t* base = static_cast<const uint8_t*>(cube.actor);
        if (!IsReadable(base + trigger_offset, sizeof(void*))) return false;
        if (require_mesh) {
            if (!IsReadable(base + mesh_offset, sizeof(void*))) return false;
            cube.mesh = *reinterpret_cast<void* const*>(base + mesh_offset);
        }
        cube.trigger = *reinterpret_cast<void* const*>(base + trigger_offset);
        if (require_mesh && (!IsReadable(cube.mesh, sizeof(UObjectHeader)) ||
            !IsReadable(cube.trigger, sizeof(UObjectHeader)))) return false;
    }
    return true;
}

bool PrepareCubeWorld(MyCharacter* player) {
    if (!player || !IsReadable(player, sizeof(MyCharacter)) ||
        !IsReadable(player->movement, sizeof(UObjectHeader))) return false;
    const auto* state = PracticeLevelState();
    if (!state) return false;
    if (s_cubes.ready && s_cubes.player == player && s_cubes.resets == state->resets)
        return true;
    s_cubes = {};
    std::array<CubeActor, kTotalCubes> candidate{};
    void* cube_class = nullptr;
    if (!DiscoverCubeActors(candidate, cube_class, true)) return false;
    CubeApi api{};
    if (!ResolveApi(candidate[0], player, api)) return false;
    s_cubes.api = api;
    s_cubes.actors = candidate;
    s_cubes.cube_class = cube_class;
    s_cubes.player = player;
    s_cubes.resets = state->resets;
    s_cubes.last_level = state->level;
    s_cubes.ready = true;
    for (auto& cube : s_cubes.actors) {
        bool visible = false;
        uint8_t collision = 0;
        bool picked = false;
        if (!ReadPickedUp(cube, picked) ||
            !ComponentVisibility(cube.mesh, visible) ||
            !ComponentCollision(cube.trigger, collision)) {
            s_cubes = {};
            return false;
        }
        cube.original_saved = true;
        cube.original_visible = visible;
        cube.original_collision = collision;
    }
    Log(L"World cubes: validated all 18 BP_PowerCore_C actors and native pickup API");
    return true;
}

bool TriggerNativePickup(const CubeActor& cube, MyCharacter* player, const CubeApi& api = s_cubes.api) {
    if (!player || !IsReadable(player, sizeof(MyCharacter)) ||
        !IsReadable(player->movement, sizeof(UObjectHeader)) || !g_process_event || !api.pickup) return false;
    auto* primitive_addr = static_cast<uint8_t*>(static_cast<void*>(player->movement)) + api.updated_primitive;
    if (!IsReadable(primitive_addr, sizeof(void*))) return false;
    void* primitive = *reinterpret_cast<void**>(primitive_addr);
    if (!IsReadable(primitive, sizeof(UObjectHeader))) return false;
    alignas(16) std::array<uint8_t, kArgumentCapacity> params{};
    // Match the validated TAS overlap invocation on BP_PowerCore_C.
    std::memcpy(params.data() + api.overlapped_component, &primitive, sizeof(void*));
    std::memcpy(params.data() + api.other_actor, &cube.actor, sizeof(void*));
    std::memcpy(params.data() + api.other_component, &cube.trigger, sizeof(void*));
    const int32_t body_index = 0;
    std::memcpy(params.data() + api.other_body_index, &body_index, sizeof(body_index));
    g_process_event(cube.actor, api.pickup, params.data());
    return true;
}

bool ApplyCubeAction(CubeActor& cube, CubeWorldAction action, MyCharacter* player) {
    if (!IsReadable(cube.actor, sizeof(UObjectHeader)) ||
        static_cast<UObjectHeader*>(cube.actor)->class_ptr != s_cubes.cube_class ||
        !IsReadable(cube.mesh, sizeof(UObjectHeader)) ||
        !IsReadable(cube.trigger, sizeof(UObjectHeader))) return false;
    bool picked = false;
    if (!ReadPickedUp(cube, picked)) return false;
    if (action == CubeWorldAction::CollectAll) {
        if (picked) return true;
        if (!TriggerNativePickup(cube, player)) return false;
        ++s_cubes.modified;
        return true;
    }
    // Never reanimate already collected cubes: that would diverge from
    // native LevelState.cubes and allow duplicate pickup events.
    if (picked) return true;
    bool visible = false;
    uint8_t collision = 0;
    if (!ComponentVisibility(cube.mesh, visible) ||
        !ComponentCollision(cube.trigger, collision)) return false;
    bool desired_visible = cube.original_visible;
    uint8_t desired_collision = cube.original_collision;
    if (action == CubeWorldAction::DisableAll) {
        desired_visible = false;
        desired_collision = 0;
    } else if (action == CubeWorldAction::ShowAll) {
        desired_visible = true;
        desired_collision = 1; // QueryOnly; trigger overlap works, no physics body.
    } else if (action == CubeWorldAction::ShowAboveWater) {
        bool above = false;
        if (!IsAboveWater(cube, above)) return false;
        desired_visible = above;
        desired_collision = above ? 1 : 0;
    }
    // Disable collisions before hiding; show mesh before enabling overlaps.
    bool ok = desired_visible
        ? SetComponentVisibility(cube.mesh, true) &&
          SetComponentCollision(cube.trigger, desired_collision)
        : SetComponentCollision(cube.trigger, desired_collision) &&
          SetComponentVisibility(cube.mesh, false);
    if (ok && (visible != desired_visible || collision != desired_collision))
        ++s_cubes.modified;
    return ok;
}

void BeginBatch(CubeWorldAction action) {
    s_cubes.action = action;
    s_cubes.next = 0;
    s_cubes.modified = 0;
    s_cubes.pending = true;
}
} // namespace

bool RequestCubeWorldAction(CubeWorldAction action, MyCharacter* player) {
    if (!PrepareCubeWorld(player)) {
        Notify(L"Cubes unavailable: native actors/reflection not ready (see log)");
        Log(L"World cubes: failed validation; no cube state modified");
        return false;
    }
    if (s_cubes.pending) {
        Notify(L"Finish the current cube action first");
        return false;
    }
    // Restore is a reversal of our visibility overrides. Collection is
    // irreversible without a native Refunct restart and is never faked.
    if (action != CubeWorldAction::RestoreDefault) {
        g_recording_tainted = true;
        g_run_pb_eligible = false;
        MarkCheatAction(CheatAction::Cubes);
    }
    s_cubes.mode = action == CubeWorldAction::CollectAll
        ? CubeWorldAction::RestoreDefault : action;
    BeginBatch(action);
    Notify(L"Cube action queued: processing one native cube per game tick");
    return true;
}

void UpdateWorldCubes(MyCharacter* player) {
    if (!s_cubes.ready) return;
    const auto* state = PracticeLevelState();
    if (!state || player != s_cubes.player || state->resets != s_cubes.resets) {
        ResetWorldCubeCache();
        return;
    }
    if (!s_cubes.pending && s_cubes.mode != CubeWorldAction::RestoreDefault &&
        state->level != s_cubes.last_level) {
        // Blueprint spawning and section movement can change pickup visibility.
        // Refresh only when game progression reaches a new level.
        s_cubes.last_level = state->level;
        BeginBatch(s_cubes.mode);
    }
    if (!s_cubes.pending) return;
    if (s_cubes.next >= s_cubes.actors.size()) {
        s_cubes.pending = false;
        if (s_cubes.action == CubeWorldAction::RestoreDefault)
            s_cubes.mode = CubeWorldAction::RestoreDefault;
        Notify(L"Cube action completed (" + std::to_wstring(s_cubes.modified) +
               L" cubes processed)");
        return;
    }
    // One cube per frame bounds Blueprint ProcessEvent and overlap work.
    CubeActor& cube = s_cubes.actors[s_cubes.next];
    if (!ApplyCubeAction(cube, s_cubes.action, player)) {
        Log((L"World cubes: native action failed for cube " +
             std::to_wstring(s_cubes.next + 1)).c_str());
        Notify(L"Cube action stopped: native actor state changed (see log)");
        s_cubes.pending = false;
        s_cubes.mode = CubeWorldAction::RestoreDefault;
        return;
    }
    ++s_cubes.next;
}

bool IsWorldCubeOverrideActive() {
    return s_cubes.ready && (s_cubes.pending || s_cubes.mode != CubeWorldAction::RestoreDefault);
}

void ResetWorldCubeCache() { s_cubes = {}; }

namespace {
using ghost_replay_cube_contact::Point;
using ghost_replay_cube_contact::Region;
struct CubeIdentity { void* object=nullptr; int index=-1; int serial=0; };
const GlobalObjectArray* ReplayCubeObjects() {
    if (!g_game_module) return nullptr;
    const auto* objects=reinterpret_cast<const GlobalObjectArray*>(
        reinterpret_cast<uintptr_t>(g_game_module)+kGlobalObjectArrayRva);
    if (!IsReadable(objects,sizeof(*objects)) || !objects->objects ||
        objects->num_elements<=0 || objects->num_elements>objects->max_elements ||
        objects->num_elements>500000) return nullptr;
    return objects;
}
CubeIdentity ReplayCubeIdentity(const GlobalObjectArray* objects, void* object) {
    if (!objects || !IsReadable(object,sizeof(UObjectHeader))) return {};
    const int index=static_cast<UObjectHeader*>(object)->index;
    if (index<0 || index>=objects->num_elements) return {};
    const auto* slot=objects->objects+index;
    if (!IsReadable(slot,sizeof(*slot)) || slot->object!=object ||
        (static_cast<uint32_t>(slot->flags)&0x30000000u)) return {};
    return {object,index,slot->serial_number};
}
bool ReplayCubeLive(const GlobalObjectArray* objects,const CubeIdentity& identity) {
    const auto current=ReplayCubeIdentity(objects,identity.object);
    return current.object && current.index==identity.index && current.serial==identity.serial;
}
struct ReplayCube {
    CubeActor cube{};
    CubeIdentity actor{},trigger{};
    CubeApi api{};
    void* location=nullptr;
    void* sphere=nullptr;
    void* bounds=nullptr;
    int location_offset=-1,sphere_offset=-1,origin_offset=-1,extent_offset=-1;
    int collision_only=-1;
    Region region{};
    bool done=false;
    bool attempted=false; // no duplicate native dispatch, even if Blueprint defers confirmation
};
struct ReplayCubeCache {
    MyCharacter* player=nullptr;
    int resets=-1;
    ULONGLONG started=0,next_retry=0;
    bool discovered=false,ready=false,unavailable=false,issued=false;
    size_t cursor=0;
    int valid=0,confirmed=0;
    std::array<ReplayCube,kTotalCubes> entries{};
};
ReplayCubeCache s_replay_cubes{};

bool ReadReplayCubeRegion(const ReplayCube& entry, Region& result, bool live) {
    alignas(16) std::array<uint8_t,kArgumentCapacity> args{};
    if (entry.sphere && entry.location) {
        g_process_event(entry.cube.trigger,entry.location,args.data());
        std::memcpy(&result.center,args.data()+entry.location_offset,sizeof(Vec3));
        args.fill(0);
        g_process_event(entry.cube.trigger,entry.sphere,args.data());
        float radius=0;
        std::memcpy(&radius,args.data()+entry.sphere_offset,sizeof(radius));
        result.extent={radius,radius,radius};
        result.sphere=true;
        return ghost_replay_cube_contact::Valid(result);
    }
    if (!entry.bounds) return false;
    // During preparation include hidden components (sections may still be submerged).
    // For a live contact require the collision bounds, not a decorative mesh alone.
    if (live) args[entry.collision_only]=1;
    g_process_event(entry.cube.actor,entry.bounds,args.data());
    std::memcpy(&result.center,args.data()+entry.origin_offset,sizeof(Vec3));
    std::memcpy(&result.extent,args.data()+entry.extent_offset,sizeof(Vec3));
    result.sphere=false;
    return ghost_replay_cube_contact::Valid(result);
}

bool PrepareReplayCube(ReplayCube& entry,MyCharacter* player,const GlobalObjectArray* objects) {
    entry.actor=ReplayCubeIdentity(objects,entry.cube.actor);
    entry.trigger=ReplayCubeIdentity(objects,entry.cube.trigger);
    if (!entry.actor.object || !entry.trigger.object || !ResolvePickupApi(entry.cube,player,entry.api)) return false;
    entry.location=FindReflectedFunction(entry.cube.trigger,L"K2_GetComponentLocation");
    entry.sphere=FindReflectedFunction(entry.cube.trigger,L"GetScaledSphereRadius");
    if (entry.location && entry.sphere) {
        entry.location_offset=FindClassPropertyOffset(entry.location,L"ReturnValue");
        entry.sphere_offset=FindClassPropertyOffset(entry.sphere,L"ReturnValue");
        if (OffsetOK(entry.location_offset,sizeof(Vec3)) && OffsetOK(entry.sphere_offset,sizeof(float)) &&
            ReadReplayCubeRegion(entry,entry.region,false)) return true;
    }
    entry.sphere=nullptr;
    entry.bounds=FindReflectedFunction(entry.cube.actor,L"GetActorBounds");
    if (!entry.bounds) return false;
    entry.origin_offset=FindClassPropertyOffset(entry.bounds,L"Origin");
    entry.extent_offset=FindClassPropertyOffset(entry.bounds,L"BoxExtent");
    entry.collision_only=FindClassPropertyOffset(entry.bounds,L"bOnlyCollidingComponents");
    if (!OffsetOK(entry.origin_offset,sizeof(Vec3)) || !OffsetOK(entry.extent_offset,sizeof(Vec3)) ||
        !OffsetOK(entry.collision_only) || std::abs(entry.origin_offset-entry.extent_offset)<static_cast<int>(sizeof(Vec3)) ||
        (entry.collision_only>=entry.origin_offset && entry.collision_only<entry.origin_offset+static_cast<int>(sizeof(Vec3))) ||
        (entry.collision_only>=entry.extent_offset && entry.collision_only<entry.extent_offset+static_cast<int>(sizeof(Vec3)))) return false;
    return ReadReplayCubeRegion(entry,entry.region,false);
}
} // namespace

bool GhostReplayCubesChanged() { return s_replay_cubes.issued; }
void ResetGhostReplayCubes() {
    if (s_replay_cubes.issued)
        Log((L"Ghost replay: collected "+std::to_wstring(s_replay_cubes.confirmed)+L" native cubes (confirmed)").c_str());
    s_replay_cubes={};
}

GhostReplayCubeStatus PrepareGhostReplayCubes(MyCharacter* player) {
    auto& replay=s_replay_cubes;
    const auto* progress=PracticeLevelState();
    if (!player || !progress || !g_process_event || !IsReadable(player,sizeof(MyCharacter)) ||
        !IsReadable(player->movement,sizeof(UObjectHeader))) return GhostReplayCubeStatus::Unavailable;
    const auto now=GetTickCount64();
    if (replay.player!=player || replay.resets!=progress->resets) {
        ResetGhostReplayCubes();
        replay.player=player;replay.resets=progress->resets;replay.started=now;
    }
    if (replay.unavailable) return GhostReplayCubeStatus::Unavailable;
    if (replay.ready) return GhostReplayCubeStatus::Ready;
    if (!replay.discovered) {
        if (now<replay.next_retry) return GhostReplayCubeStatus::Pending;
        replay.next_retry=now+1000;
        std::array<CubeActor,kTotalCubes> actors{};
        void* klass=nullptr;
        if (!ReplayCubeObjects() || !DiscoverCubeActors(actors,klass,false)) {
            if (now-replay.started<5000) return GhostReplayCubeStatus::Pending;
            replay.unavailable=true;
            Log(L"Ghost replay: cube actor layout unavailable; replay continues");
            return GhostReplayCubeStatus::Unavailable;
        }
        for (size_t i=0;i<actors.size();++i) replay.entries[i].cube=actors[i];
        replay.discovered=true;
    }
    // One cube's reflected geometry per preparation tick. No global scans in playback.
    auto& entry=replay.entries[replay.cursor++];
    if (PrepareReplayCube(entry,player,ReplayCubeObjects())) ++replay.valid;
    else entry.done=true;
    if (replay.cursor<replay.entries.size()) return GhostReplayCubeStatus::Pending;
    replay.ready=replay.valid>0;
    replay.unavailable=!replay.ready;
    Log((L"Ghost replay: cached "+std::to_wstring(replay.valid)+L"/18 cube pickup regions").c_str());
    return replay.ready?GhostReplayCubeStatus::Ready:GhostReplayCubeStatus::Unavailable;
}

void TouchGhostReplayCubes(MyCharacter* player,const Vec3& previous,const Vec3& current,bool continuous) {
    auto& replay=s_replay_cubes;
    if (!replay.ready || player!=replay.player || !g_process_event) return;
    const auto* progress=PracticeLevelState();
    if (!progress || progress->resets!=replay.resets) return;
    const Point end{current.x,current.y,current.z};
    const Point start=ghost_replay_cube_contact::Continuous({previous.x,previous.y,previous.z},end,continuous)
        ?Point{previous.x,previous.y,previous.z}:end;
    const GlobalObjectArray* objects=nullptr;
    for (auto& entry:replay.entries) {
        if (entry.done || entry.attempted || !ghost_replay_cube_contact::NearXY(start,end,entry.region)) continue;
        if (!objects) objects=ReplayCubeObjects();
        if (!objects || !ReplayCubeLive(objects,entry.actor) || !ReplayCubeLive(objects,entry.trigger)) continue;
        bool picked=false;
        if (!ReadPickedUp(entry.cube,picked,entry.api)) continue;
        if (picked) {entry.done=true;continue;}
        uint8_t collision=0;
        if (!ComponentCollision(entry.cube.trigger,collision,entry.api) || !(collision==1 || collision==3)) continue;
        Region live{};
        if (!ReadReplayCubeRegion(entry,live,true) || !ghost_replay_cube_contact::Crossed(start,end,live)) continue;
        const int count_before=progress->cubes;
        // Mark before calling the Blueprint so cleanup also covers deferred state changes.
        entry.attempted=true;
        replay.issued=true;
        if (!TriggerNativePickup(entry.cube,player,entry.api)) {
            Log(L"Ghost replay: native cube pickup parameters unavailable");
            continue;
        }
        objects=nullptr; // native dispatch may have changed the registry
        progress=PracticeLevelState();
        if (!progress || progress->resets!=replay.resets) return;
        if (progress->cubes>count_before ||
            (ReplayCubeLive(ReplayCubeObjects(),entry.actor) && ReadPickedUp(entry.cube,picked,entry.api) && picked)) {
            ++replay.confirmed;
            Log((L"Ghost replay: native cube pickup confirmed ("+std::to_wstring(replay.confirmed)+L")").c_str());
        } else Log(L"Ghost replay: cube pickup dispatched; native confirmation pending or unavailable");
    }
}

} // namespace refunct

#include "refunct/internal_api.hpp"
#include "refunct/features/tiles.hpp"
#include "refunct/ghost_replay_tile_contact.hpp"
#include "refunct/colour_work_budget.hpp"
#include <commdlg.h>
#include <cwchar>
#include <initializer_list>
#include <random>
#pragma comment(lib, "Comdlg32.lib")

namespace refunct {
namespace {

// The TAS project's native platform action uses ALiftBase's
// AddBasedCharacter/RemoveBasedCharacter pair on BP_IslandChunk_C. These
// actors are stored in BP_LevelRoot_C::FertileLands, not in the button list.
// A count-only LevelState edit would not update the tile materials.
struct TileBatch {
    bool active = false;
    bool include_underwater = false;
    bool deselect = false;
    int changed = 0;
    int resets = -1;
    int initial_count = 0;
    int count = 0;
    int cursor = 0;
    void* player = nullptr;
    void* tile_class = nullptr;
    // Verified on Refunct v9.30: Color is the only native tile actor byte
    // that changed on selection; material[0] switched from desaturated to coloured.
    int32_t color_offset = -1;
    int32_t normal_materials_offset = -1;
    int32_t base_mesh_offset = -1;
    std::array<void*, kPracticeClusterCount> level_roots{};
    std::array<void*, kTotalColourableTiles> actors{};
};
TileBatch s_tiles;

// Loaded lazily after the existing storage system creates settings.ini.
// The contact hook and menu callbacks execute on Refunct's game thread.
bool s_disable_tile_selection_on_touch_loaded = false;
bool s_disable_tile_selection_on_touch = false;
bool s_disable_button_tile_selection_loaded = false;
bool s_disable_button_tile_selection = false;

// A button press can colour a tile in Blueprint code rather than through
// ALiftBase::AddBasedCharacter. Record genuine native tile state before the
// button contact and reverse only 0 -> selected transitions after progression.
struct ButtonTileGuard {
    bool pending = false;
    unsigned button_callback_depth = 0;
    bool button_progress_observed = false;
    int before_buttons = -1;
    int resets = -1;
    MyCharacter* player = nullptr;
    ULONGLONG started_at = 0;
    TileBatch tiles{};
    std::array<int32_t, kTotalColourableTiles> old_colours{};
};
ButtonTileGuard s_button_tile_guard{};
// World object layout is invariant during a run. Do not re-reflect all 31
// FertileLands arrays and 251 tile classes on every real button press.
TileBatch s_button_tile_layout{};
bool s_button_tile_layout_ready = false;
ULONGLONG s_button_tile_layout_retry_at = 0;

// UE4 can deliver a tile touch just BEFORE the button contact on the same
// physics step. Preserve a tiny history of pre-contact Color=0 tiles so the
// button transaction also catches that event ordering without scanning the
// entire map each frame.
struct RecentTileTouch {
    void* actor = nullptr;
    MyCharacter* player = nullptr;
    int resets = -1;
    ULONGLONG at = 0;
};
std::array<RecentTileTouch, 8> s_recent_tile_touches{};
size_t s_recent_tile_cursor = 0;

void LoadButtonTileSetting() {
    if (s_disable_button_tile_selection_loaded || g_settings_path.empty()) return;
    s_disable_button_tile_selection = GetPrivateProfileIntW(
        L"TileInteraction", L"DisableButtonTileSelection", 0,
        g_settings_path.c_str()) != 0;
    s_disable_button_tile_selection_loaded = true;
}

void LoadTileTouchSetting() {
    if (s_disable_tile_selection_on_touch_loaded || g_settings_path.empty()) return;
    s_disable_tile_selection_on_touch = GetPrivateProfileIntW(
        L"TileInteraction", L"DisableSelectionOnTouch", 0,
        g_settings_path.c_str()) != 0;
    s_disable_tile_selection_on_touch_loaded = true;
}

bool TileActorValid(void* actor, void* expected_class) {
    if (!actor || !IsReadable(actor, sizeof(UObjectHeader))) return false;
    const auto* header = static_cast<const UObjectHeader*>(actor);
    return header->class_ptr && IsReadable(header->class_ptr, sizeof(UObjectHeader)) &&
           (expected_class ? header->class_ptr == expected_class
                           : UnrealName(static_cast<const UObjectHeader*>(
                                 header->class_ptr)->name) == L"BP_IslandChunk_C");
}

bool ReadFertileLandList(void* level_root, int32_t offset,
                         UObjectPointerArray& out) {
    if (!level_root || offset < 0 || offset > 0x4000 ||
        !IsReadable(static_cast<const uint8_t*>(level_root) + offset,
                    sizeof(UObjectPointerArray))) return false;
    std::memcpy(&out, static_cast<const uint8_t*>(level_root) + offset,
                sizeof(out));
    return out.count >= 0 && out.count <= kTotalColourableTiles &&
           out.capacity >= out.count && out.capacity <= 10000 &&
           (out.count == 0 || (out.data &&
               IsReadable(out.data, sizeof(void*) * static_cast<size_t>(out.count))));
}

bool FindFertileLandsOffset(int32_t& offset) {
    if (!g_practice_level_class ||
        !IsReadable(g_practice_level_class, sizeof(UStructHeader))) return false;
    // Check the reflected type as well as the offset to avoid treating an
    // unrelated Blueprint field as a UObject pointer array.
    void* cls = g_practice_level_class;
    for (int depth = 0; depth < 16 && cls &&
         IsReadable(cls, sizeof(UStructHeader)); ++depth) {
        void* field = static_cast<const UStructHeader*>(cls)->children;
        for (int i = 0; i < 4096 && field &&
             IsReadable(field, sizeof(UFieldHeader)); ++i) {
            auto* entry = static_cast<UFieldHeader*>(field);
            if (UnrealName(entry->object.name) == L"FertileLands") {
                if (!entry->object.class_ptr ||
                    !IsReadable(entry->object.class_ptr, sizeof(UObjectHeader)) ||
                    UnrealName(static_cast<const UObjectHeader*>(
                        entry->object.class_ptr)->name) != L"ArrayProperty") return false;
                offset = FindClassPropertyOffset(g_practice_level_class,
                                                L"FertileLands");
                return offset >= 0;
            }
            field = entry->next;
        }
        cls = static_cast<const UStructHeader*>(cls)->super_struct;
    }
    return false;
}

void StopTileBatch(const std::wstring& why) {
    const std::wstring operation = s_tiles.deselect ? L"Deselect" : L"Select";
    Log((L"World tiles: " + operation + L" stopped after " +
         std::to_wstring(s_tiles.cursor) + L"/" +
         std::to_wstring(s_tiles.count) + L" actors: " + why).c_str());
    // A native restart can invalidate the level state here. Do not read it
    // just to estimate progress; report that this batch is not atomic.
    Notify(operation + L" tiles stopped: " + why +
           (s_tiles.cursor > 0 ? L" (earlier changes may remain)" : L""));
    s_tiles = {};
}


void* ReadObjectProperty(void* object, const wchar_t* name) {
    if (!IsReadable(object, sizeof(UObjectHeader))) return nullptr;
    void* cls = static_cast<const UObjectHeader*>(object)->class_ptr;
    // BaseMesh is read for every coloured tile. Its UProperty offset is
    // constant for a given Blueprint class; re-reflect only if that class
    // changes (e.g. after a game-content reload).
    struct BaseMeshOffsetCache {
        void* owner_class = nullptr;
        int32_t offset = -1;
    };
    static BaseMeshOffsetCache base_mesh{};
    int32_t offset = -1;
    if (wcscmp(name, L"BaseMesh") == 0) {
        if (base_mesh.owner_class != cls || base_mesh.offset < 0) {
            base_mesh = {cls, FindClassPropertyOffset(cls, L"BaseMesh")};
        }
        offset = base_mesh.offset;
    } else {
        offset = FindClassPropertyOffset(cls, name);
    }
    if (offset < 0 || offset > 0x3FFC ||
        !IsReadable(static_cast<const uint8_t*>(object) + offset, sizeof(void*)))
        return nullptr;
    void* result{};
    std::memcpy(&result, static_cast<const uint8_t*>(object) + offset,
                sizeof(result));
    return IsReadable(result, sizeof(UObjectHeader)) ? result : nullptr;
}

void* ReadMeshMaterial(void* mesh, int32_t element_index = 0) {
    if (!mesh || !g_process_event || element_index < 0 || element_index > 3 ||
        !IsReadable(mesh, sizeof(UObjectHeader)))
        return nullptr;
    struct GetMaterialCache {
        void* owner_class = nullptr;
        void* function = nullptr;
    };
    static GetMaterialCache cached{};
    void* cls = static_cast<const UObjectHeader*>(mesh)->class_ptr;
    if (cached.owner_class != cls ||
        !IsReadable(cached.function, sizeof(UStructHeader))) {
        cached = {cls, nullptr};
        void* fn = FindReflectedFunction(mesh, L"GetMaterial");
        if (!IsReadable(fn, sizeof(UStructHeader))) return nullptr;
        // Validate once per class: GetMaterial(int32 ElementIndex) -> MaterialInterface*.
        bool has_index = false;
        bool has_result = false;
        void* field = static_cast<const UStructHeader*>(fn)->children;
        for (int n = 0; n < 32 && IsReadable(field, sizeof(UFieldHeader)); ++n) {
            const auto* prop = static_cast<const UFieldHeader*>(field);
            const auto field_name = UnrealName(prop->object.name);
            const auto type = IsReadable(prop->object.class_ptr, sizeof(UObjectHeader))
                ? UnrealName(static_cast<const UObjectHeader*>(prop->object.class_ptr)->name)
                : L"";
            if (!IsReadable(field, 0x40)) return nullptr;
            int32_t offset{};
            std::memcpy(&offset, static_cast<const uint8_t*>(field) + 0x3C,
                        sizeof(offset));
            if (field_name == L"ElementIndex" && type == L"IntProperty" && offset == 0)
                has_index = true;
            if (field_name == L"ReturnValue" && type == L"ObjectProperty" && offset == 4)
                has_result = true;
            field = prop->next;
        }
        if (!has_index || !has_result) {
            Log(L"Tile materials: GetMaterial signature could not be verified");
            return nullptr;
        }
        cached.function = fn;
    }
    if (!cached.function) return nullptr;
    alignas(16) std::array<uint8_t, 128> args{};
    std::memcpy(args.data(), &element_index, sizeof(element_index));
    g_process_event(mesh, cached.function, args.data());
    void* material{};
    std::memcpy(&material, args.data() + 4, sizeof(material));
    return IsReadable(material, sizeof(UObjectHeader)) ? material : nullptr;
}

// In the captured tile-state report, ONLY BP_IslandChunk_C::Color changed
// (0 -> 3), while BaseMesh material[0] changed to a coloured material.
// Do not guess UFunction signatures: validate every reflected property before
// changing state. This is a controlled reverse of those two observed changes,
// NOT a call to the inert K2_OnReset event.
bool PropertyMatches(void* cls, const wchar_t* property_name,
                     const wchar_t* property_type, int32_t& offset) {
    offset = -1;
    if (!IsReadable(cls, sizeof(UStructHeader))) return false;
    for (int depth = 0; depth < 16 && IsReadable(cls, sizeof(UStructHeader)); ++depth) {
        void* field = static_cast<const UStructHeader*>(cls)->children;
        for (int n = 0; n < 2048 && IsReadable(field, sizeof(UFieldHeader)); ++n) {
            const auto* prop = static_cast<const UFieldHeader*>(field);
            if (UnrealName(prop->object.name) == property_name) {
                if (!IsReadable(prop->object.class_ptr, sizeof(UObjectHeader)) ||
                    UnrealName(static_cast<const UObjectHeader*>(prop->object.class_ptr)->name)
                        != property_type) return false;
                offset = FindClassPropertyOffset(cls, property_name);
                return offset >= 0 && offset < 0x4000;
            }
            field = prop->next;
        }
        cls = static_cast<const UStructHeader*>(cls)->super_struct;
    }
    return false;
}

// Direct native state reversal is intentionally restricted to writable pages.
// Check the entire field before touching materials so a changed game build
// fails safely rather than crashing on a read-only/protected allocation.
bool WritableTileField(const void* address, size_t size) {
    if (!address || size == 0) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(address, &mbi, sizeof(mbi)) != sizeof(mbi) ||
        mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const DWORD protection = mbi.Protect & 0xFF;
    if (protection != PAGE_READWRITE && protection != PAGE_WRITECOPY &&
        protection != PAGE_EXECUTE_READWRITE &&
        protection != PAGE_EXECUTE_WRITECOPY) return false;
    const uintptr_t start = reinterpret_cast<uintptr_t>(address);
    const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    if (start < base) return false;
    const size_t offset = static_cast<size_t>(start - base);
    return offset <= mbi.RegionSize && size <= mbi.RegionSize - offset;
}

bool ReadTileColor(void* actor, int32_t color_offset, int32_t& out,
                   void* expected_class = nullptr) {
    // A verified cached class pointer avoids resolving the Unreal class name
    // hundreds of times during a single button-contact snapshot.
    if (!TileActorValid(actor, expected_class) ||
        color_offset < 0 || color_offset > 0x3FFC ||
        !IsReadable(static_cast<const uint8_t*>(actor) + color_offset, sizeof(out)))
        return false;
    std::memcpy(&out, static_cast<const uint8_t*>(actor) + color_offset,
                sizeof(out));
    return out >= 0 && out <= 8;
}

bool ReadNormalTileMaterials(void* actor, int32_t normal_offset,
                             std::array<void*, 2>& result) {
    UObjectPointerArray array{};
    if (!ReadFertileLandList(actor, normal_offset, array) || array.count != 2)
        return false;
    for (int i = 0; i < 2; ++i) {
        void* material = array.data[i];
        if (!IsReadable(material, sizeof(UObjectHeader))) return false;
        const auto* header = static_cast<const UObjectHeader*>(material);
        if (!IsReadable(header->class_ptr, sizeof(UObjectHeader))) return false;
        const auto name = UnrealName(
            static_cast<const UObjectHeader*>(header->class_ptr)->name);
        if (name.find(L"Material") == std::wstring::npos) return false;
        result[i] = material;
    }
    return true;
}

bool ResolveTileDeselectAbi(TileBatch& batch) {
    if (!batch.tile_class || !batch.count || !g_process_event ||
        !PropertyMatches(batch.tile_class, L"Color", L"IntProperty", batch.color_offset) ||
        !PropertyMatches(batch.tile_class, L"BaseMesh", L"ObjectProperty", batch.base_mesh_offset) ||
        !PropertyMatches(batch.tile_class, L"NormalMaterials", L"ArrayProperty",
                         batch.normal_materials_offset))
        return false;
    // Pin the validated addresses to the offsets observed in the actual log.
    if (batch.color_offset != 0x02C4 || batch.base_mesh_offset != 0x02BC ||
        batch.normal_materials_offset != 0x02C8) return false;

    void* mesh = ReadObjectProperty(batch.actors[0], L"BaseMesh");
    if (!mesh || !ReadMeshMaterial(mesh, 0) || !ReadMeshMaterial(mesh, 1))
        return false;
    void* set_material = FindReflectedFunction(mesh, L"SetMaterial");
    if (!IsReadable(set_material, sizeof(UStructHeader))) return false;
    int32_t element = -1, material = -1;
    return PropertyMatches(set_material, L"ElementIndex", L"IntProperty", element) &&
           PropertyMatches(set_material, L"Material", L"ObjectProperty", material) &&
           element == 0 && material == 4;
}

// Cache the immutable native tile layout (actors + verified field offsets).
// This is prewarmed during the ordinary game tick when the option is enabled,
// rather than doing expensive reflection on the button-contact callback.
// Shared read-only tile enumeration. Replay needs Color and native contact,
// not the material-write ABI required by the independent deselection feature.
bool CollectNativeTileLayout(MyCharacter* player, const LevelState& state,
                             TileBatch& candidate) {
    if (!player || !g_practice_level_class) return false;
    int32_t offset = -1;
    if (!FindFertileLandsOffset(offset)) return false;

    candidate = {};
    candidate.player = player;
    candidate.resets = state.resets;
    for (int section = 0; section < kPracticeClusterCount; ++section) {
        void* root = g_practice_level_actors[section];
        if (!IsReadable(root, sizeof(UObjectHeader)) ||
            static_cast<const UObjectHeader*>(root)->class_ptr != g_practice_level_class)
            return false;
        candidate.level_roots[section] = root;
        UObjectPointerArray list{};
        if (!ReadFertileLandList(root, offset, list)) return false;
        for (int i = 0; i < list.count; ++i) {
            void* actor = list.data[i];
            if (candidate.count >= kTotalColourableTiles ||
                !TileActorValid(actor, candidate.tile_class) ||
                std::find(candidate.actors.begin(),
                          candidate.actors.begin() + candidate.count, actor) !=
                    candidate.actors.begin() + candidate.count)
                return false;
            if (!candidate.tile_class)
                candidate.tile_class = static_cast<const UObjectHeader*>(actor)->class_ptr;
            candidate.actors[candidate.count++] = actor;
        }
    }
    if (candidate.count != kTotalColourableTiles) return false;
    return true;
}

bool PrepareButtonTileLayout(MyCharacter* player) {
    const LevelState* state = PracticeLevelState();
    if (!state || !player || player != g_player ||
        state->buttons < 0 || state->buttons > kRefunctButtonCount ||
        state->platforms < 0 || state->platforms > kTotalColourableTiles ||
        !g_practice_level_class || !g_process_event || s_tiles.active)
        return false;
    if (s_button_tile_layout_ready) {
        if (s_button_tile_layout.player == player &&
            s_button_tile_layout.resets == state->resets &&
            s_button_tile_layout.tile_class &&
            s_button_tile_layout.count == kTotalColourableTiles) {
            bool same_roots = true;
            for (int i = 0; i < kPracticeClusterCount; ++i) {
                if (s_button_tile_layout.level_roots[i] != g_practice_level_actors[i]) {
                    same_roots = false;
                    break;
                }
            }
            if (same_roots) return true;
        }
        s_button_tile_layout_ready = false;
    }
    const ULONGLONG now = GetTickCount64();
    if (now < s_button_tile_layout_retry_at) return false;
    s_button_tile_layout_retry_at = now + 1000;
    TileBatch candidate{};
    if (!CollectNativeTileLayout(player, *state, candidate) ||
        !ResolveTileDeselectAbi(candidate)) return false;
    s_button_tile_layout = candidate;
    s_button_tile_layout_ready = true;
    return true;
}

// Spectator replay has no physics body. Translate only recorded footstep
// intersections into the SAME native contact pair used by real gameplay.
// Cache tile actor bounds once at startup, but read live bounds for candidate
// contacts because button activation physically raises the section under them.
struct GhostReplayTileCache {
    MyCharacter* player = nullptr;
    int resets = -1;
    ULONGLONG started = 0;
    int collected = 0;
    int valid_regions = 0;
    int mesh_regions = 0;
    ULONGLONG next_layout_retry = 0;
    bool unavailable = false;
    bool contact_issued = false;
    TileBatch layout{};
    void* bounds_fn = nullptr;
    int origin_offset = -1, extent_offset = -1;
    std::array<ghost_replay_tile_contact::Bounds, kTotalColourableTiles> regions{};
    struct MeshApi {
        void* cls = nullptr;
        void* local = nullptr;
        void* location = nullptr;
        void* rotation = nullptr;
        void* scale = nullptr;
        int lo=-1, hi=-1, position=-1, angle=-1, scaling=-1;
        bool ready = false;
    } mesh_api{};
    struct MeshRegion {
        void* component = nullptr;
        void* component_class = nullptr;
        void* location_fn = nullptr;
        int location_offset = -1;
        ghost_replay_tile_contact::Point offset{};
        ghost_replay_tile_contact::Point extent{};
    };
    std::array<MeshRegion, kTotalColourableTiles> mesh_regions_cache{};
    std::array<bool, kTotalColourableTiles> attempted{};
    std::array<uint8_t, kTotalColourableTiles> contact_attempts{};
    std::array<ULONGLONG, kTotalColourableTiles> retry_after{};
    int contact_count = 0;
};
GhostReplayTileCache s_ghost_replay_tiles{};

// Resolve output parameters from live reflection; no guessed component offsets.
// GetLocalBounds is a mesh-space AABB. Refunct raises the owning section by
// translation, so cache its transformed extent/offset and refresh the mesh
// world location only for nearby candidates.
bool ReplayTileVectorParam(void* function, const wchar_t* name, int& offset) {
    return function && PropertyMatches(function, name, L"StructProperty", offset) &&
        offset >= 0 && offset <= 512 - static_cast<int>(sizeof(Vec3));
}
bool ReplayTileVectorCall(void* object, void* function, int offset, Vec3& value) {
    if (!g_process_event || !IsReadable(object, sizeof(UObjectHeader)) || !function ||
        offset < 0 || offset > 512 - static_cast<int>(sizeof(Vec3))) return false;
    alignas(16) std::array<uint8_t, 512> args{};
    g_process_event(object, function, args.data());
    std::memcpy(&value, args.data() + offset, sizeof(value));
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
bool PrepareReplayTileMesh(void* actor, int index,
                           ghost_replay_tile_contact::Bounds& out) {
    auto& replay = s_ghost_replay_tiles;
    if (!g_process_event || replay.layout.base_mesh_offset < 0 ||
        !TileActorValid(actor,replay.layout.tile_class)) return false;
    void* mesh = ReadObjectProperty(actor, L"BaseMesh");
    if (!IsReadable(mesh, sizeof(UObjectHeader))) return false;
    auto* cls = static_cast<const UObjectHeader*>(mesh)->class_ptr;
    if (!IsReadable(cls, sizeof(UObjectHeader))) return false;
    auto& api = replay.mesh_api;
    if (api.cls != cls) {
        api = {};
        api.cls = cls;
        api.local = FindReflectedFunction(mesh,L"GetLocalBounds");
        api.location = FindReflectedFunction(mesh,L"K2_GetComponentLocation");
        api.rotation = FindReflectedFunction(mesh,L"K2_GetComponentRotation");
        api.scale = FindReflectedFunction(mesh,L"K2_GetComponentScale");
        api.ready = ReplayTileVectorParam(api.local,L"Min",api.lo) &&
            ReplayTileVectorParam(api.local,L"Max",api.hi) &&
            std::abs(api.lo-api.hi) >= static_cast<int>(sizeof(Vec3)) &&
            ReplayTileVectorParam(api.location,L"ReturnValue",api.position) &&
            ReplayTileVectorParam(api.rotation,L"ReturnValue",api.angle) &&
            ReplayTileVectorParam(api.scale,L"ReturnValue",api.scaling);
    }
    if (!api.ready) return false;
    alignas(16) std::array<uint8_t, 512> args{};
    g_process_event(mesh, api.local, args.data());
    Vec3 minimum{}, maximum{}, pos{}, rot{}, factor{};
    std::memcpy(&minimum,args.data()+api.lo,sizeof(minimum));
    std::memcpy(&maximum,args.data()+api.hi,sizeof(maximum));
    if (!ReplayTileVectorCall(mesh,api.location,api.position,pos) ||
        !ReplayTileVectorCall(mesh,api.rotation,api.angle,rot) ||
        !ReplayTileVectorCall(mesh,api.scale,api.scaling,factor)) return false;
    ghost_replay_tile_contact::Bounds local_world{};
    if (!ghost_replay_tile_contact::TransformMeshBounds(
            {minimum.x,minimum.y,minimum.z},{maximum.x,maximum.y,maximum.z},
            {rot.x,rot.y,rot.z},{factor.x,factor.y,factor.z},local_world)) return false;
    out = local_world;
    out.center.x += pos.x; out.center.y += pos.y; out.center.z += pos.z;
    if (!ghost_replay_tile_contact::Valid(out)) return false;
    replay.mesh_regions_cache[index] = {mesh,cls,api.location,api.position,
                                       local_world.center,local_world.extent};
    return true;
}
bool ReadGhostReplayTileBounds(void* actor, int index,
                               ghost_replay_tile_contact::Bounds& result) {
    const auto& cache = s_ghost_replay_tiles;
    if (!g_process_event || !TileActorValid(actor, cache.layout.tile_class)) return false;
    const auto& mesh = cache.mesh_regions_cache[index];
    if (mesh.component) {
        // Check component identity before invoking a cached UFunction.
        if (ReadObjectProperty(actor,L"BaseMesh") != mesh.component ||
            !IsReadable(mesh.component,sizeof(UObjectHeader)) ||
            static_cast<const UObjectHeader*>(mesh.component)->class_ptr != mesh.component_class)
            return false;
        Vec3 pos{};
        if (!ReplayTileVectorCall(mesh.component,mesh.location_fn,mesh.location_offset,pos))
            return false;
        result = {{pos.x+mesh.offset.x,pos.y+mesh.offset.y,pos.z+mesh.offset.z},mesh.extent};
        return ghost_replay_tile_contact::Valid(result);
    }
    // Older engine builds can omit mesh getters. The existing actor-bounds
    // API is a conservative fallback, verified individually per tile.
    if (!cache.bounds_fn || cache.origin_offset < 0 || cache.extent_offset < 0)
        return false;
    alignas(16) std::array<uint8_t, 512> args{};
    g_process_event(actor, cache.bounds_fn, args.data());
    std::memcpy(&result.center, args.data()+cache.origin_offset, sizeof(Vec3));
    std::memcpy(&result.extent, args.data()+cache.extent_offset, sizeof(Vec3));
    return ghost_replay_tile_contact::Valid(result);
}

// Snapshot only the mutable Color values at contact. The cached actor map is
// revalidated against the current player, reset ID and 31 section roots.
bool SnapshotButtonTiles(ButtonTileGuard& guard, MyCharacter* player) {
    const LevelState* state = PracticeLevelState();
    if (!state || state->buttons < 0 ||
        state->buttons >= kRefunctButtonCount || s_tiles.active ||
        !PrepareButtonTileLayout(player)) return false;
    const TileBatch& candidate = s_button_tile_layout;
    int selected = 0;
    for (int i = 0; i < candidate.count; ++i) {
        int32_t colour = -1;
        if (!ReadTileColor(candidate.actors[i], candidate.color_offset, colour,
                           candidate.tile_class))
            return false;
        guard.old_colours[i] = colour;
        if (colour != 0) ++selected;
    }
    // If a previous third-party edit left the counter inconsistent, do not
    // attempt to modify tile state based on a potentially invalid snapshot.
    if (selected != state->platforms) return false;
    guard.tiles = candidate;
    guard.before_buttons = state->buttons;
    guard.resets = state->resets;
    guard.player = player;
    guard.started_at = GetTickCount64();
    // A tile selected just before the button contact would otherwise look
    // selected in this snapshot. Treat only very recent native tile touches
    // (same player and map) as pre-button transitions.
    constexpr ULONGLONG kSameContactWindowMs = 100;
    for (const auto& touch : s_recent_tile_touches) {
        if (!touch.actor || touch.player != player ||
            touch.resets != state->resets || guard.started_at < touch.at ||
            guard.started_at - touch.at > kSameContactWindowMs) continue;
        const auto last = candidate.actors.begin() + candidate.count;
        const auto it = std::find(candidate.actors.begin(), last, touch.actor);
        if (it != last) {
            const size_t index = static_cast<size_t>(it - candidate.actors.begin());
            if (guard.old_colours[index] != 0) guard.old_colours[index] = 0;
        }
    }
    guard.pending = true;
    guard.button_progress_observed = false;
    return true;
}

bool SetMeshMaterialVerified(void* mesh, int32_t index, void* material) {
    if (!mesh || !material || !g_process_event || index < 0 || index > 1)
        return false;
    if (!IsReadable(mesh, sizeof(UObjectHeader))) return false;
    struct SetMaterialCache { void* owner_class = nullptr; void* function = nullptr; };
    static SetMaterialCache cached{};
    void* cls = static_cast<const UObjectHeader*>(mesh)->class_ptr;
    if (cached.owner_class != cls ||
        !IsReadable(cached.function, sizeof(UStructHeader))) {
        cached = {cls, nullptr};
        void* fn = FindReflectedFunction(mesh, L"SetMaterial");
        if (!IsReadable(fn, sizeof(UStructHeader))) return false;
        int32_t element_offset = -1, material_offset = -1;
        if (!PropertyMatches(fn, L"ElementIndex", L"IntProperty", element_offset) ||
            !PropertyMatches(fn, L"Material", L"ObjectProperty", material_offset) ||
            element_offset != 0 || material_offset != 4) return false;
        cached.function = fn;
    }
    alignas(16) std::array<uint8_t, 128> args{};
    std::memcpy(args.data(), &index, sizeof(index));
    std::memcpy(args.data() + 4, &material, sizeof(material));
    g_process_event(mesh, cached.function, args.data());
    return ReadMeshMaterial(mesh, index) == material;
}

enum class TileDeselectResult { Skipped, Reset, Failed };

TileDeselectResult DeselectOneTile(void* actor, const TileBatch& batch,
                                    const LevelState& before) {
    int32_t color = -1;
    if (!ReadTileColor(actor, batch.color_offset, color)) return TileDeselectResult::Failed;
    if (!color) return TileDeselectResult::Skipped;
    if (before.platforms <= 0) return TileDeselectResult::Failed;
    LevelState* writable_state = const_cast<LevelState*>(PracticeLevelState());
    if (!writable_state ||
        !WritableTileField(static_cast<const uint8_t*>(actor) + batch.color_offset,
                           sizeof(int32_t)) ||
        !WritableTileField(&writable_state->platforms,
                           sizeof(writable_state->platforms)))
        return TileDeselectResult::Failed;
    std::array<void*, 2> normal{};
    if (!ReadNormalTileMaterials(actor, batch.normal_materials_offset, normal))
        return TileDeselectResult::Failed;
    void* mesh = ReadObjectProperty(actor, L"BaseMesh");
    if (!mesh) return TileDeselectResult::Failed;
    std::array<void*, 2> previous{};
    for (int slot = 0; slot < 2; ++slot) {
        previous[slot] = ReadMeshMaterial(mesh, slot);
        if (!previous[slot]) return TileDeselectResult::Failed;
    }

    // First make the actual visible mesh uncoloured. If either slot refuses
    // the normal material, restore both original materials and leave game
    // progress/Color intact rather than creating a false deselection.
    if (!SetMeshMaterialVerified(mesh, 0, normal[0]) ||
        !SetMeshMaterialVerified(mesh, 1, normal[1])) {
        SetMeshMaterialVerified(mesh, 0, previous[0]);
        SetMeshMaterialVerified(mesh, 1, previous[1]);
        return TileDeselectResult::Failed;
    }
    LevelState* state = const_cast<LevelState*>(PracticeLevelState());
    if (!state || state != writable_state ||
        !WritableTileField(static_cast<const uint8_t*>(actor) + batch.color_offset,
                           sizeof(int32_t)) ||
        !WritableTileField(&state->platforms, sizeof(state->platforms)) ||
        state->level != before.level || state->buttons != before.buttons ||
        state->cubes != before.cubes || state->resets != before.resets ||
        state->platforms != before.platforms) {
        SetMeshMaterialVerified(mesh, 0, previous[0]);
        SetMeshMaterialVerified(mesh, 1, previous[1]);
        return TileDeselectResult::Failed;
    }
    // The log showed this is the only mutable actor byte after normal
    // selection. Reset it only after the visual materials are verified.
    const int32_t uncoloured = 0;
    std::memcpy(static_cast<uint8_t*>(actor) + batch.color_offset,
                &uncoloured, sizeof(uncoloured));
    int32_t confirmed = -1;
    if (!ReadTileColor(actor, batch.color_offset, confirmed) || confirmed != 0) {
        std::memcpy(static_cast<uint8_t*>(actor) + batch.color_offset,
                    &color, sizeof(color));
        SetMeshMaterialVerified(mesh, 0, previous[0]);
        SetMeshMaterialVerified(mesh, 1, previous[1]);
        return TileDeselectResult::Failed;
    }
    // Selection normally increments LevelState.platforms. On an intentionally
    // reversed selection we also reverse that increment. This is NOT a
    // counter-only shortcut: BOTH native state and both materials are reset
    // and checked before the counter is touched.
    state->platforms = before.platforms - 1;
    if (state->platforms != before.platforms - 1) {
        // A counter mismatch must not leave Color/materials deselected while
        // the native progress counter still reports a coloured tile.
        state->platforms = before.platforms;
        std::memcpy(static_cast<uint8_t*>(actor) + batch.color_offset,
                    &color, sizeof(color));
        const bool slot0_restored = SetMeshMaterialVerified(mesh, 0, previous[0]);
        const bool slot1_restored = SetMeshMaterialVerified(mesh, 1, previous[1]);
        if (!slot0_restored || !slot1_restored)
            Log(L"World tiles: failed to restore materials after counter mismatch");
        return TileDeselectResult::Failed;
    }
    return TileDeselectResult::Reset;
}

// ---------------------------------------------------------------------------
// VISUAL-ONLY TILE COLOUR PRESETS
//
// Refunct's BP_IslandChunk_C owns eight 2-slot ActivatedMaterials arrays.
// The selected-state inspector verified that a tile's Color IntProperty is
// its gameplay state and that the displayed material changes on selection.
// We change the *displayed materials only*, never Color or LevelState.
// This leaves selection/deselection and the native tile counter untouched.
// ---------------------------------------------------------------------------
struct TilePaletteRuntime {
    bool cached = false;
    bool restore_defaults = false;
    MyCharacter* player = nullptr;
    int resets = -1;
    int cursor = 0;
    int count = 0;
    int failures = 0;
    void* tile_class = nullptr;
    int32_t normal_materials_offset = -1;
    std::array<int32_t, 9> material_offsets{}; // 0 unused, 1..8 are palettes.
    std::array<void*, kPracticeClusterCount> roots{};
    std::array<void*, kTotalColourableTiles> actors{};
    // UE owns these MIDs while attached to their mesh; never reuse a detached MID.
    struct DynamicSlot {
        void* instance = nullptr;
        void* source = nullptr;
        FNameView tint_parameter{};
        uint32_t last_rgb = 0;
        bool has_colour = false;
    };
    std::array<std::array<DynamicSlot, 2>, kTotalColourableTiles> dynamic_slots{};
    // Tracks *native* selected-state transitions, not visual palette changes.
    struct RandomColourState {
        bool observed = false;
        bool was_selected = false;
        bool assigned = false;
        uint32_t rgb = 0;
    };
    std::array<RandomColourState, kTotalColourableTiles> random_colours{};
    // A native Color check is enough when neither the game nor the user has
    // changed this tile. Avoid repeatedly calling reflected UE4 materials.
    std::array<int32_t, kTotalColourableTiles> applied_native_colours{};
    std::array<uint32_t, kTotalColourableTiles> applied_revisions{};
    int random_scan_cursor = 0;
    // Buttons and jump pads can activate tiles without native tile contact.
    // Observe native progress on CharacterTick, then check cached Color fields
    // in bounded batches; no UObject discovery or materials in physics hooks.
    int observed_button_count = -1;
    int observed_platform_count = -1;
    int progress_refresh_cursor = 0;
    int progress_refresh_remaining = 0;
    int progress_refresh_passes = 0;
};

TilePaletteRuntime s_tile_palette;
uint32_t s_tile_visual_revision = 1;
int s_tile_colour_dirty_remaining = 0;

void InvalidateTileVisuals() {
    // Schedule a fast, time-budgeted pass over cached tiles on the next tick.
    s_tile_colour_dirty_remaining = kTotalColourableTiles;
    ++s_tile_visual_revision;
    if (s_tile_visual_revision == 0) {
        s_tile_visual_revision = 1;
        s_tile_palette.applied_revisions.fill(0);
    }
}

// Contacts are queued in the native hook but tinted later on the game tick.
// Never create MIDs or reflect UFunctions while inside the physics callback.
struct NativeTileContact { void* actor = nullptr; uint8_t attempts = 0; };
std::array<NativeTileContact, 24> s_recent_native_tile_contacts{};
int s_recent_native_tile_count = 0;
std::array<int, 2> s_tile_palette_choices{{-1, -1}}; // 0=selected, 1=unselected.

struct CustomColourSetting {
    bool loaded = false;
    bool enabled = false;
    uint32_t rgb = 0;
};
std::array<CustomColourSetting, 2> s_custom_colours{};
std::array<CustomColourSetting, 2> s_wall_colours{}; // Selected and unselected wall faces.
bool s_random_tile_rgb_loaded = false;
bool s_random_tile_rgb_enabled = false;

bool RandomTileRGBEnabled() {
    if (!s_random_tile_rgb_loaded && !g_settings_path.empty()) {
        s_random_tile_rgb_enabled = GetPrivateProfileIntW(
            L"TileColours", L"RandomSelectedRGB", 0,
            g_settings_path.c_str()) != 0;
        s_random_tile_rgb_loaded = true;
    }
    return s_random_tile_rgb_enabled;
}

// Colours are random per activation, not random per frame. Use vivid hues
// so a random colour remains recognizable against the level's grey tiles.
uint32_t GenerateRandomTileRGB() {
    static std::mt19937 rng = [] {
        LARGE_INTEGER clock{};
        QueryPerformanceCounter(&clock);
        const uint64_t ticks = GetTickCount64();
        std::seed_seq seed{
            static_cast<uint32_t>(clock.QuadPart),
            static_cast<uint32_t>(clock.QuadPart >> 32),
            static_cast<uint32_t>(ticks),
            static_cast<uint32_t>(ticks >> 32),
            static_cast<uint32_t>(GetCurrentProcessId())
        };
        return std::mt19937(seed);
    }();
    const float hue = static_cast<float>(std::uniform_int_distribution<int>(0, 359)(rng));
    const float saturation = static_cast<float>(std::uniform_int_distribution<int>(78, 100)(rng)) / 100.0f;
    const float brightness = static_cast<float>(std::uniform_int_distribution<int>(88, 100)(rng)) / 100.0f;
    const float chroma = brightness * saturation;
    const float hue_sector = hue / 60.0f;
    const float secondary = chroma * (1.0f - std::abs(std::fmod(hue_sector, 2.0f) - 1.0f));
    const float match = brightness - chroma;
    float red = 0, green = 0, blue = 0;
    switch (static_cast<int>(hue_sector)) {
        case 0: red = chroma; green = secondary; break;
        case 1: red = secondary; green = chroma; break;
        case 2: green = chroma; blue = secondary; break;
        case 3: green = secondary; blue = chroma; break;
        case 4: red = secondary; blue = chroma; break;
        default: red = chroma; blue = secondary; break;
    }
    const auto channel = [match](float value) -> uint32_t {
        return static_cast<uint32_t>(std::clamp(std::lround((value + match) * 255.0f), 0L, 255L));
    };
    return channel(red) << 16 | channel(green) << 8 | channel(blue);
}
constexpr uint32_t kPickerNoResult = 0xFFFFFFFFu;
constexpr uint32_t kPickerCancelled = 0x01000000u;
constexpr uint32_t kPickerError = 0x01000001u;
std::atomic<uint32_t> s_picker_result{kPickerNoResult};
std::atomic<int> s_picker_target{0};
std::atomic<bool> s_picker_open{false};
std::atomic<HWND> s_picker_owner{nullptr};
std::atomic<uint32_t> s_picker_start_rgb{0};

std::wstring HexString(uint32_t colour) {
    wchar_t buf[8]{};
    swprintf_s(buf, L"#%06X", colour & 0xFFFFFFu);
    return buf;
}

bool ParseHex(std::wstring text, uint32_t& out) {
    if (text.size() == 7 && text.front() == L'#') text.erase(0, 1);
    if (text.size() != 6) return false;
    out = 0;
    for (wchar_t c : text) {
        const int digit = c >= L'0' && c <= L'9' ? c - L'0' :
                          c >= L'A' && c <= L'F' ? c - L'A' + 10 :
                          c >= L'a' && c <= L'f' ? c - L'a' + 10 : -1;
        if (digit < 0) return false;
        out = (out << 4) | static_cast<uint32_t>(digit);
    }
    return true;
}

CustomColourSetting& CustomColour(bool selected) {
    auto& setting = s_custom_colours[selected ? 0 : 1];
    if (setting.loaded) return setting;
    setting.loaded = true;
    setting.rgb = selected ? 0x00FF00u : 0x808080u;
    if (g_settings_path.empty()) return setting;
    wchar_t buf[64]{};
    GetPrivateProfileStringW(L"TileColours", selected ? L"SelectedHex" : L"UnselectedHex",
                             selected ? L"#00FF00" : L"#808080", buf, 64,
                             g_settings_path.c_str());
    uint32_t parsed{};
    if (ParseHex(buf, parsed)) setting.rgb = parsed;
    setting.enabled = GetPrivateProfileIntW(L"TileColours",
                         selected ? L"SelectedCustom" : L"UnselectedCustom", 0,
                         g_settings_path.c_str()) != 0;
    return setting;
}

void SaveCustomColour(bool selected) {
    if (g_settings_path.empty()) return;
    const auto& setting = CustomColour(selected);
    const wchar_t* name = selected ? L"SelectedHex" : L"UnselectedHex";
    const wchar_t* toggle = selected ? L"SelectedCustom" : L"UnselectedCustom";
    const auto hex = HexString(setting.rgb);
    if (!QueueSettingsValue(L"TileColours", name, hex.c_str()) ||
        !QueueSettingsValue(L"TileColours", toggle,
                                   setting.enabled ? L"1" : L"0"))
        Log(L"Tile colours: failed to write HEX preference");
}

// The concrete panel material covers the wall faces of BP_IslandChunk_C.
// Each state gets an independent tint; we *read* native Color to select the
// setting, but never modify tile progression or the native Color field here.
CustomColourSetting& WallColour(bool selected) {
    auto& setting = s_wall_colours[selected ? 0 : 1];
    if (setting.loaded) return setting;
    setting.loaded = true;
    setting.rgb = 0x808080u;
    if (g_settings_path.empty()) return setting;

    // Migration: an existing single WallColours.Hex/CustomEnabled applies to
    // both states until the user changes either of the new independent values.
    wchar_t legacy_hex[64]{};
    GetPrivateProfileStringW(L"WallColours", L"Hex", L"#808080", legacy_hex,
                             64, g_settings_path.c_str());
    wchar_t buf[64]{};
    GetPrivateProfileStringW(L"WallColours",
        selected ? L"SelectedHex" : L"UnselectedHex", legacy_hex,
        buf, 64, g_settings_path.c_str());
    uint32_t parsed{};
    if (ParseHex(buf, parsed)) setting.rgb = parsed;
    const UINT legacy_enabled = GetPrivateProfileIntW(
        L"WallColours", L"CustomEnabled", 0, g_settings_path.c_str());
    setting.enabled = GetPrivateProfileIntW(L"WallColours",
        selected ? L"SelectedCustom" : L"UnselectedCustom", legacy_enabled,
        g_settings_path.c_str()) != 0;
    return setting;
}

void SaveWallColour(bool selected) {
    if (g_settings_path.empty()) return;
    const auto& setting = WallColour(selected);
    const auto hex = HexString(setting.rgb);
    if (!QueueSettingsValue(L"WallColours",
            selected ? L"SelectedHex" : L"UnselectedHex", hex.c_str()) ||
        !QueueSettingsValue(L"WallColours",
            selected ? L"SelectedCustom" : L"UnselectedCustom",
            setting.enabled ? L"1" : L"0"))
        Log(L"Wall colours: could not save selected/unselected preference");
}

// A standard Win32 colour picker is a separate modal UI, not a renderer hook.
// It runs on its own thread; only UpdateTileColourPresets touches UE objects.
DWORD WINAPI ColourPickerThread(LPVOID) {
    COLORREF swatches[16]{};
    const uint32_t colour = s_picker_start_rgb.load(std::memory_order_relaxed);
    CHOOSECOLORW choice{};
    choice.lStructSize = sizeof(choice);
    choice.hwndOwner = s_picker_owner.load(std::memory_order_relaxed);
    choice.rgbResult = RGB((colour >> 16) & 0xFF, (colour >> 8) & 0xFF, colour & 0xFF);
    choice.lpCustColors = swatches;
    choice.Flags = CC_RGBINIT | CC_FULLOPEN | CC_ANYCOLOR;
    if (ChooseColorW(&choice)) {
        const uint32_t rgb = (static_cast<uint32_t>(GetRValue(choice.rgbResult)) << 16) |
                             (static_cast<uint32_t>(GetGValue(choice.rgbResult)) << 8) |
                              static_cast<uint32_t>(GetBValue(choice.rgbResult));
        s_picker_result.store(rgb, std::memory_order_release);
    } else {
        s_picker_result.store(CommDlgExtendedError() ? kPickerError : kPickerCancelled,
                              std::memory_order_release);
    }
    s_picker_open.store(false, std::memory_order_release);
    return 0;
}

int CurrentTilePaletteChoice(bool selected) {
    const int slot = selected ? 0 : 1;
    if (s_tile_palette_choices[slot] >= 0) return s_tile_palette_choices[slot];
    s_tile_palette_choices[slot] = 0;
    if (!g_settings_path.empty()) {
        const UINT stored = GetPrivateProfileIntW(
            L"TileColours", selected ? L"SelectedPreset" : L"UnselectedPreset",
            0, g_settings_path.c_str());
        if (stored <= 8) s_tile_palette_choices[slot] = static_cast<int>(stored);
    }
    return s_tile_palette_choices[slot];
}

void SnapshotRandomTileBaseline(TilePaletteRuntime& runtime) {
    for (int i = 0; i < runtime.count; ++i) {
        auto& record = runtime.random_colours[i];
        record = {};
        int32_t native_colour = -1;
        if (ReadTileColor(runtime.actors[i], 0x02C4, native_colour)) {
            record.observed = true;
            record.was_selected = native_colour != 0;
        }
    }
}

// This is also called by our select/deselect batches while the palette
// scanner is paused, preventing a missed 0 -> selected transition.
void RecordRandomTileTransition(void* actor, bool is_selected) {
    if (!RandomTileRGBEnabled() || !s_tile_palette.cached) return;
    const auto last = s_tile_palette.actors.begin() + s_tile_palette.count;
    const auto it = std::find(s_tile_palette.actors.begin(), last, actor);
    if (it == last) return;
    const size_t index = static_cast<size_t>(it - s_tile_palette.actors.begin());
    auto& record = s_tile_palette.random_colours[index];
    if (!is_selected) {
        const bool changed = record.was_selected || record.assigned;
        record.observed = true;
        record.was_selected = false;
        record.assigned = false;
        if (changed) s_tile_palette.applied_revisions[index] = 0;
    } else if (!record.was_selected) {
        record.observed = true;
        record.was_selected = true;
        record.assigned = true;
        record.rgb = GenerateRandomTileRGB();
        s_tile_palette.applied_revisions[index] = 0;
    }
}

bool BuildTilePaletteCache(MyCharacter* player, const LevelState* state) {
    if (!player || !state || !g_practice_level_class) return false;
    int32_t fertile_offset = -1;
    if (!FindFertileLandsOffset(fertile_offset)) return false;
    TilePaletteRuntime candidate{};
    candidate.restore_defaults = s_tile_palette.restore_defaults;
    candidate.player = player;
    candidate.resets = state->resets;
    candidate.observed_button_count = state->buttons;
    candidate.observed_platform_count = state->platforms;
    candidate.material_offsets.fill(-1);
    candidate.applied_native_colours.fill(-1);
    for (int section = 0; section < kPracticeClusterCount; ++section) {
        void* root = g_practice_level_actors[section];
        if (!root || !IsReadable(root, sizeof(UObjectHeader)) ||
            static_cast<const UObjectHeader*>(root)->class_ptr != g_practice_level_class)
            return false;
        candidate.roots[section] = root;
        UObjectPointerArray array{};
        if (!ReadFertileLandList(root, fertile_offset, array)) return false;
        for (int index = 0; index < array.count; ++index) {
            void* actor = array.data[index];
            if (!TileActorValid(actor, candidate.tile_class) ||
                candidate.count >= kTotalColourableTiles) return false;
            if (!candidate.tile_class)
                candidate.tile_class = static_cast<const UObjectHeader*>(actor)->class_ptr;
            if (std::find(candidate.actors.begin(),
                          candidate.actors.begin() + candidate.count, actor)
                    != candidate.actors.begin() + candidate.count)
                return false;
            candidate.actors[candidate.count++] = actor;
        }
    }
    if (candidate.count != kTotalColourableTiles || !candidate.tile_class)
        return false;
    // Resolve the original neutral materials and all eight native palettes.
    if (!PropertyMatches(candidate.tile_class, L"NormalMaterials", L"ArrayProperty",
                         candidate.normal_materials_offset)) return false;
    // We deliberately don't invent shader or material parameter names.
    for (int palette = 1; palette <= 8; ++palette) {
        const std::wstring name = L"ActivatedMaterials" + std::to_wstring(palette);
        int32_t offset = -1;
        if (!PropertyMatches(candidate.tile_class, name.c_str(),
                             L"ArrayProperty", offset) || offset > 0x3FF4)
            return false;
        candidate.material_offsets[palette] = offset;
    }
    s_tile_palette = candidate;
    s_tile_palette.cached = true;
    s_recent_native_tile_count = 0;
    if (RandomTileRGBEnabled()) SnapshotRandomTileBaseline(s_tile_palette);
    return true;
}

// Older Refunct builds may expose CreateAndSetMaterialInstanceDynamic* instead
// of the modern CreateDynamicMaterialInstance Blueprint function.  The original
// HEX code accepted only one exact UFunction layout, causing HEX, the picker,
// and Random RGB to silently share the same failure.  Resolve the actual
// reflected function rather than guessing a fixed RVA or calling convention.
void LogColourFunctionSignatureOnce(const wchar_t* label, void* function) {
    static std::vector<std::wstring> printed;
    const std::wstring key = std::wstring(label) + L"@" +
        std::to_wstring(reinterpret_cast<uintptr_t>(function));
    if (std::find(printed.begin(), printed.end(), key) != printed.end())
        return;
    printed.push_back(key);
    std::wstring output = L"Tile RGB ABI: ";
    output += label;
    if (!IsReadable(function, sizeof(UStructHeader))) {
        output += L" -> function absent";
        Log(output.c_str());
        return;
    }
    output += L" ->";
    void* field = static_cast<const UStructHeader*>(function)->children;
    for (int i = 0; i < 24 && IsReadable(field, sizeof(UFieldHeader)); ++i) {
        const auto* prop = static_cast<const UFieldHeader*>(field);
        const auto name = UnrealName(prop->object.name);
        std::wstring type = L"?";
        if (IsReadable(prop->object.class_ptr, sizeof(UObjectHeader))) {
            type = UnrealName(static_cast<const UObjectHeader*>(prop->object.class_ptr)->name);
        }
        const int32_t offset = FindClassPropertyOffset(function, name.c_str());
        output += L" [" + name + L":" + type + L"@" + std::to_wstring(offset) + L"]";
        field = prop->next;
    }
    Log(output.c_str());
}

void LogCustomColourIssueOnce(const wchar_t* reason) {
    static std::vector<std::wstring> reported;
    if (std::find(reported.begin(), reported.end(), reason) != reported.end())
        return;
    reported.emplace_back(reason);
    const std::wstring message = std::wstring(L"Tile RGB: ") + reason;
    Log(message.c_str());
    // Do not claim that applying a HEX/picker/random colour succeeded when
    // its native material API is unavailable.
    Notify(message);
}

// A MIC's vector parameter entry starts with FMaterialParameterInfo::Name
// (FName, 8 bytes in Refunct x86).  Its name is discovered at runtime rather
// than assuming the shader accepts "Tint" or "BaseColor".
bool ReadMaterialTintName(void* source, FNameView& out_name) {
    if (!IsReadable(source, sizeof(UObjectHeader))) return false;
    void* cls = static_cast<const UObjectHeader*>(source)->class_ptr;
    int32_t offset = -1;
    if (!PropertyMatches(cls, L"VectorParameterValues", L"ArrayProperty", offset)) {
        LogCustomColourIssueOnce(L"source material has no VectorParameterValues field");
        return false;
    }
    if (!IsReadable(static_cast<const uint8_t*>(source) + offset,
                    sizeof(UObjectPointerArray))) return false;
    UObjectPointerArray array{};
    std::memcpy(&array, static_cast<const uint8_t*>(source) + offset, sizeof(array));
    // Sampled stock materials each expose one vector override. Other slots
    // are left using their original material, never blindly modified.
    if (array.count != 1 || !array.data ||
        !IsReadable(array.data, sizeof(FNameView))) return false;
    std::memcpy(&out_name, array.data, sizeof(out_name));
    const std::wstring name = UnrealName(out_name);
    if (name.empty() || name == L"None" || name.size() > 100) {
        LogCustomColourIssueOnce(L"stock material vector parameter name unreadable");
        return false;
    }
    static std::vector<std::wstring> names;
    if (std::find(names.begin(), names.end(), name) == names.end()) {
        names.push_back(name);
        Log((L"Tile RGB ABI: native material vector parameter = " + name).c_str());
    }
    return true;
}

bool SetMidColour(void* mid, const FNameView& parameter, uint32_t rgb) {
    if (!IsReadable(mid, sizeof(UObjectHeader)) || !g_process_event) return false;
    struct TintCallCache {
        void* owner_class = nullptr;
        void* function = nullptr;
        int32_t name_offset = -1;
        int32_t colour_offset = -1;
    };
    static TintCallCache cached{};
    void* cls = static_cast<const UObjectHeader*>(mid)->class_ptr;
    if (cached.owner_class != cls ||
        !IsReadable(cached.function, sizeof(UStructHeader))) {
        cached = {};
        cached.owner_class = cls;
        void* function = FindReflectedFunction(mid, L"SetVectorParameterValue");
        if (!function) function = FindReflectedFunction(mid, L"K2_SetVectorParameterValue");
        int32_t name_offset = -1, color_offset = -1;
        if (!IsReadable(function, sizeof(UStructHeader)) ||
            !PropertyMatches(function, L"ParameterName", L"NameProperty", name_offset) ||
            !PropertyMatches(function, L"Value", L"StructProperty", color_offset) ||
            name_offset < 0 || color_offset < 0 ||
            name_offset + static_cast<int>(sizeof(parameter)) > 96 ||
            color_offset + static_cast<int>(sizeof(LinearColor)) > 96) {
            LogColourFunctionSignatureOnce(L"SetVectorParameterValue", function);
            LogCustomColourIssueOnce(L"dynamic material colour setter unavailable/signature mismatch");
            return false;
        }
        cached = {cls, function, name_offset, color_offset};
    }
    // HEX is sRGB; UE's FLinearColor is linear RGB.
    auto component = [](uint32_t v) -> float {
        const float x = static_cast<float>(v) / 255.0f;
        return x <= 0.04045f ? x / 12.92f :
               std::pow((x + 0.055f) / 1.055f, 2.4f);
    };
    const LinearColor tint{
        component((rgb >> 16) & 0xFF), component((rgb >> 8) & 0xFF),
        component(rgb & 0xFF), 1.0f
    };
    alignas(16) std::array<uint8_t, 96> params{};
    std::memcpy(params.data() + cached.name_offset, &parameter, sizeof(parameter));
    std::memcpy(params.data() + cached.colour_offset, &tint, sizeof(tint));
    g_process_event(mid, cached.function, params.data());
    return true;
}

// Invoke only fully-validated, engine-reflected material factories.  Support
// the older Unreal 4 Blueprint entry points used by historical game builds.
void* InvokeMIDFactory(void* mesh, const wchar_t* function_name,
                       int slot, void* material, bool needs_parent,
                       const wchar_t* parent_property, bool needs_name) {
    if (!IsReadable(mesh, sizeof(UObjectHeader))) return nullptr;
    const int kind = wcscmp(function_name, L"CreateDynamicMaterialInstance") == 0 ? 0 :
        wcscmp(function_name, L"CreateAndSetMaterialInstanceDynamicFromMaterial") == 0 ? 1 :
        wcscmp(function_name, L"CreateAndSetMaterialInstanceDynamic") == 0 ? 2 : -1;
    if (kind < 0) return nullptr;
    struct FactoryCallCache {
        void* owner_class = nullptr;
        void* function = nullptr;
        bool checked = false;
        int32_t index_offset = -1;
        int32_t parent_offset = -1;
        int32_t result_offset = -1;
    };
    static std::array<FactoryCallCache, 3> calls{};
    auto& call = calls[kind];
    void* cls = static_cast<const UObjectHeader*>(mesh)->class_ptr;
    if (!call.checked || call.owner_class != cls ||
        (call.function && !IsReadable(call.function, sizeof(UStructHeader)))) {
        call = {};
        call.checked = true;
        call.owner_class = cls;
        void* function = FindReflectedFunction(mesh, function_name);
        if (!IsReadable(function, sizeof(UStructHeader))) return nullptr;
        int32_t index_offset = -1, parent_offset = -1;
        int32_t name_offset = -1, result_offset = -1;
        if (!PropertyMatches(function, L"ElementIndex", L"IntProperty", index_offset) ||
            !PropertyMatches(function, L"ReturnValue", L"ObjectProperty", result_offset) ||
            index_offset < 0 || index_offset + 4 > 96 ||
            result_offset < 0 || result_offset + 4 > 96 ||
            (needs_parent &&
             (!PropertyMatches(function, parent_property, L"ObjectProperty", parent_offset) ||
              parent_offset < 0 || parent_offset + 4 > 96)) ||
            (needs_name &&
             (!PropertyMatches(function, L"OptionalName", L"NameProperty", name_offset) ||
              name_offset < 0 || name_offset + 8 > 96))) {
            LogColourFunctionSignatureOnce(function_name, function);
            return nullptr;
        }
        call.function = function;
        call.index_offset = index_offset;
        call.parent_offset = parent_offset;
        call.result_offset = result_offset;
    }
    if (!call.function) return nullptr;
    alignas(16) std::array<uint8_t, 96> args{};
    std::memcpy(args.data() + call.index_offset, &slot, sizeof(slot));
    if (needs_parent)
        std::memcpy(args.data() + call.parent_offset, &material, sizeof(material));
    g_process_event(mesh, call.function, args.data());
    void* mid = nullptr;
    std::memcpy(&mid, args.data() + call.result_offset, sizeof(mid));
    if (!IsReadable(mid, sizeof(UObjectHeader))) return nullptr;
    // A factory's returned object must be the actual material on this slot.
    // If it isn't attached, don't keep/use an untracked MID pointer.
    if (ReadMeshMaterial(mesh, slot) != mid) return nullptr;
    void* mid_class = static_cast<const UObjectHeader*>(mid)->class_ptr;
    if (!IsReadable(mid_class, sizeof(UObjectHeader)) ||
        UnrealName(static_cast<const UObjectHeader*>(mid_class)->name).find(L"MaterialInstanceDynamic")
            == std::wstring::npos) return nullptr;
    return mid;
}

void* CreateTileMID(void* mesh, int slot, void* material) {
    if (!mesh || !material || !g_process_event) return nullptr;
    // Modern UE4 entry point, followed by old Blueprint-compatible variants.
    static std::vector<std::wstring> working_factories;
    auto success = [&](void* mid, const wchar_t* name) -> void* {
        if (mid && std::find(working_factories.begin(), working_factories.end(), name) ==
                       working_factories.end()) {
            working_factories.emplace_back(name);
            Log((std::wstring(L"Tile RGB ABI: dynamic material factory = ") + name).c_str());
        }
        return mid;
    };
    void* original_material = ReadMeshMaterial(mesh, slot);
    if (void* mid = InvokeMIDFactory(mesh, L"CreateDynamicMaterialInstance", slot,
                                     material, true, L"SourceMaterial", true))
        return success(mid, L"CreateDynamicMaterialInstance");
    if (void* mid = InvokeMIDFactory(mesh, L"CreateAndSetMaterialInstanceDynamicFromMaterial",
                                     slot, material, true, L"Parent", false))
        return success(mid, L"CreateAndSetMaterialInstanceDynamicFromMaterial");
    // The one-argument legacy factory uses the currently attached material.
    // Put the stock source in the slot before calling it, but only when we
    // have verified SetMaterial (it is already used by working tile presets).
    if (ReadMeshMaterial(mesh, slot) != material &&
        !SetMeshMaterialVerified(mesh, slot, material)) return nullptr;
    if (void* mid = InvokeMIDFactory(mesh, L"CreateAndSetMaterialInstanceDynamic",
                                     slot, material, false, nullptr, false))
        return success(mid, L"CreateAndSetMaterialInstanceDynamic");
    // Factory failure must not change the tile's visible original material.
    if (original_material && ReadMeshMaterial(mesh, slot) != original_material)
        SetMeshMaterialVerified(mesh, slot, original_material);
    LogColourFunctionSignatureOnce(L"CreateDynamicMaterialInstance",
        FindReflectedFunction(mesh, L"CreateDynamicMaterialInstance"));
    LogColourFunctionSignatureOnce(L"CreateAndSetMaterialInstanceDynamicFromMaterial",
        FindReflectedFunction(mesh, L"CreateAndSetMaterialInstanceDynamicFromMaterial"));
    LogColourFunctionSignatureOnce(L"CreateAndSetMaterialInstanceDynamic",
        FindReflectedFunction(mesh, L"CreateAndSetMaterialInstanceDynamic"));
    LogCustomColourIssueOnce(L"cannot create a per-tile dynamic material (see ABI log)");
    return nullptr;
}

// The tile-state inspector found M_Concrete_RectanglePanel on the large wall
// faces of BP_IslandChunk_C. It is excluded from *tile* tinting, but explicitly
// handled by the independent wall tint below. Shared stock assets stay intact.
bool IsConcretePanelMaterial(void* material) {
    if (!IsReadable(material, sizeof(UObjectHeader))) return false;
    const std::wstring name = UnrealName(
        static_cast<const UObjectHeader*>(material)->name);
    return name.rfind(L"M_Concrete_RectanglePanel", 0) == 0;
}

// Apply a wall-only RGB override to one concrete material slot. MIDs are
// created per chunk/slot, never by mutating a shared MaterialInstanceConstant.
// The same dynamic_slots cache is safe: a slot is either concrete (wall) or
// non-concrete (tile), and the source material is always validated.
bool ApplyWallMaterialSlot(void* mesh, int actor_index, int slot,
                           void* source, bool selected) {
    if (!IsConcretePanelMaterial(source) || actor_index < 0 ||
        actor_index >= s_tile_palette.count || slot < 0 || slot > 1) return false;
    auto& cache = s_tile_palette.dynamic_slots[actor_index][slot];
    void* current = ReadMeshMaterial(mesh, slot);
    if (!current) return false;
    const auto& wall = WallColour(selected);
    if (!wall.enabled) {
        if (current != source && !SetMeshMaterialVerified(mesh, slot, source))
            return false;
        cache = {};
        return true;
    }
    const bool reuse = cache.instance && cache.source == source &&
                       cache.instance == current;
    FNameView parameter = reuse ? cache.tint_parameter : FNameView{};
    if (!reuse && !ReadMaterialTintName(source, parameter)) return false;
    if (!reuse) {
        cache = {};
        void* mid = CreateTileMID(mesh, slot, source);
        if (!mid) {
            LogCustomColourIssueOnce(L"could not create per-wall material instance");
            return false;
        }
        cache.instance = mid;
        cache.source = source;
        cache.tint_parameter = parameter;
    }
    if (!cache.has_colour || cache.last_rgb != wall.rgb) {
        if (!SetMidColour(cache.instance, parameter, wall.rgb)) return false;
        cache.last_rgb = wall.rgb;
        cache.has_colour = true;
    }
    return true;
}

// On enabling Random RGB, already-selected tiles keep their existing colour;
// their wall material still needs independent updating.
bool ApplyOnlyWallMaterials(void* actor, int actor_index, int32_t offset,
                            bool selected) {
    std::array<void*, 2> materials{};
    if (!ReadNormalTileMaterials(actor, offset, materials)) return false;
    void* mesh = ReadObjectProperty(actor, L"BaseMesh");
    if (!mesh) return false;
    for (int slot = 0; slot < 2; ++slot) {
        if (IsConcretePanelMaterial(materials[slot]) &&
            !ApplyWallMaterialSlot(mesh, actor_index, slot, materials[slot], selected))
            return false;
    }
    return true;
}

bool ApplyCustomTileMaterial(void* actor, int actor_index, int32_t material_offset,
                             uint32_t rgb, bool selected) {
    if (actor_index < 0 || actor_index >= s_tile_palette.count) return false;
    std::array<void*, 2> sources{};
    if (!ReadNormalTileMaterials(actor, material_offset, sources)) return false;
    void* mesh = ReadObjectProperty(actor, L"BaseMesh");
    if (!mesh) return false;
    bool any_tinted_slot = false;
    for (int slot = 0; slot < 2; ++slot) {
        void* current = ReadMeshMaterial(mesh, slot);
        if (!current) return false;
        // The concrete slot belongs to Wall Colours. Tile HEX/Random RGB
        // must never override its independent appearance.
        if (IsConcretePanelMaterial(sources[slot])) {
            if (!ApplyWallMaterialSlot(mesh, actor_index, slot, sources[slot], selected))
                return false;
            continue;
        }
        auto& cached = s_tile_palette.dynamic_slots[actor_index][slot];
        const bool reuse = cached.instance && cached.source == sources[slot] &&
                           cached.instance == current;
        FNameView parameter = reuse ? cached.tint_parameter : FNameView{};
        if (!reuse && !ReadMaterialTintName(sources[slot], parameter)) {
            // Not every material slot has a tint vector. Keep non-colour slots stock.
            if (current != sources[slot] &&
                !SetMeshMaterialVerified(mesh, slot, sources[slot])) return false;
            continue;
        }
        any_tinted_slot = true;
        if (!reuse) {
            // A native tile change replaces the MID; make a new one instead
            // of using a detached pointer that may have been garbage collected.
            cached = {};
            void* mid = CreateTileMID(mesh, slot, sources[slot]);
            if (!mid) {
                static bool reported = false;
                if (!reported) {
                    Log(L"Tile RGB: dynamic material creation failed; see ABI details above");
                    reported = true;
                }
                return false;
            }
            cached.instance = mid;
            cached.source = sources[slot];
            cached.tint_parameter = parameter;
        }
        if (!cached.has_colour || cached.last_rgb != rgb) {
            if (!SetMidColour(cached.instance, parameter, rgb)) {
                static bool reported = false;
                if (!reported) {
                    Log(L"Tile RGB: dynamic material tinting failed; see ABI details above");
                    reported = true;
                }
                return false;
            }
            cached.last_rgb = rgb;
            cached.has_colour = true;
        }
    }
    if (!any_tinted_slot) {
        static bool reported = false;
        if (!reported) {
            Log(L"Tile RGB: no safely tintable tile surface found; "
                L"concrete wall materials preserved");
            reported = true;
        }
    }
    // Keeping the stock material is an intentional safe fallback, not a
    // gameplay or build failure. The tile remains selectable/deselectable.
    return true;
}

bool ApplyTilePaletteVisual(void* actor, int selected_preset, int unselected_preset) {
    int32_t native_colour = -1;
    if (!ReadTileColor(actor, 0x02C4, native_colour)) return false;
    const bool selected = native_colour != 0;
    if (selected && RandomTileRGBEnabled()) {
        const auto last = s_tile_palette.actors.begin() + s_tile_palette.count;
        const auto it = std::find(s_tile_palette.actors.begin(), last, actor);
        if (it == last) return false;
        const int index = static_cast<int>(it - s_tile_palette.actors.begin());
        const auto& random = s_tile_palette.random_colours[index];
        // Turning Random RGB on must not recolour already-selected tiles.
        if (!random.assigned)
            return ApplyOnlyWallMaterials(actor, index,
                s_tile_palette.material_offsets[native_colour], selected);
        return ApplyCustomTileMaterial(actor, index,
                  s_tile_palette.material_offsets[native_colour], random.rgb, selected);
    }
    const auto& custom = CustomColour(selected);
    const int desired_preset = selected ? selected_preset : unselected_preset;
    // Custom RGB uses the stock tile material rather than layering a tint
    // over whichever palette was last selected in the menu.
    const int actual_palette = custom.enabled || desired_preset == 0
        ? native_colour : desired_preset;
    if (actual_palette < 0 || actual_palette > 8) return false;

    // For native unselected state (Color==0), 0 maps to NormalMaterials.
    // All visual overrides preserve the actual Color field and level counter.
    const int32_t material_offset = actual_palette == 0
        ? s_tile_palette.normal_materials_offset
        : s_tile_palette.material_offsets[actual_palette];
    if (custom.enabled) {
        const auto first = std::find(s_tile_palette.actors.begin(),
            s_tile_palette.actors.begin() + s_tile_palette.count, actor);
        if (first == s_tile_palette.actors.begin() + s_tile_palette.count) return false;
        return ApplyCustomTileMaterial(actor,
                  static_cast<int>(first - s_tile_palette.actors.begin()),
                  material_offset, custom.rgb, selected);
    }
    std::array<void*, 2> materials{};
    if (!ReadNormalTileMaterials(actor, material_offset, materials)) return false;
    void* mesh = ReadObjectProperty(actor, L"BaseMesh");
    if (!mesh) return false;
    const std::array<void*, 2> old{
        ReadMeshMaterial(mesh, 0), ReadMeshMaterial(mesh, 1)
    };
    if (!old[0] || !old[1]) return false;
    const auto it = std::find(s_tile_palette.actors.begin(),
                              s_tile_palette.actors.begin() + s_tile_palette.count, actor);
    if (it == s_tile_palette.actors.begin() + s_tile_palette.count) return false;
    const int actor_index = static_cast<int>(it - s_tile_palette.actors.begin());
    // Apply the native palette on tile surfaces and the independent wall
    // colour on concrete slots. A failure rolls back both visible slots.
    for (int slot = 0; slot < 2; ++slot) {
        bool ok = false;
        if (IsConcretePanelMaterial(materials[slot])) {
            ok = ApplyWallMaterialSlot(mesh, actor_index, slot, materials[slot], selected);
        } else {
            ok = ReadMeshMaterial(mesh, slot) == materials[slot] ||
                 SetMeshMaterialVerified(mesh, slot, materials[slot]);
            if (ok) s_tile_palette.dynamic_slots[actor_index][slot] = {};
        }
        if (!ok) {
            SetMeshMaterialVerified(mesh, 0, old[0]);
            SetMeshMaterialVerified(mesh, 1, old[1]);
            s_tile_palette.dynamic_slots[actor_index] = {};
            return false;
        }
    }
    return true;
}

void DetectRandomTileSelections(int selected_preset, int unselected_preset) {
    if (!RandomTileRGBEnabled() || !s_tile_palette.cached) return;
    int immediate_applies = 0;
    // Creating multiple new MIDs in one tick can stall rendering. Events are
    // handled first; the fallback scanner handles at most one extra MID here.
    constexpr int kMaxImmediateApplies = 1;
    // Random RGB used to VirtualQuery all 251 actors every 50 ms. Sweep a
    // bounded subset instead; every tile is checked within ~400 ms.
    constexpr int kRandomChecksPerUpdate = 32;
    for (int checked = 0; checked < kRandomChecksPerUpdate; ++checked) {
        if (s_tile_palette.count <= 0) break;
        const int i = s_tile_palette.random_scan_cursor;
        s_tile_palette.random_scan_cursor =
            (s_tile_palette.random_scan_cursor + 1) % s_tile_palette.count;
        void* actor = s_tile_palette.actors[i];
        int32_t native_colour = -1;
        if (!ReadTileColor(actor, 0x02C4, native_colour,
                           s_tile_palette.tile_class)) continue;
        const bool is_selected = native_colour != 0;
        auto& record = s_tile_palette.random_colours[i];
        if (!record.observed) {
            record.observed = true;
            record.was_selected = is_selected;
            continue;
        }
        if (!is_selected) {
            record.was_selected = false;
            record.assigned = false;
        } else if (!record.was_selected) {
            record.was_selected = true;
            record.assigned = true;
            record.rgb = GenerateRandomTileRGB();
            s_tile_palette.applied_revisions[i] = 0;
            if (immediate_applies < kMaxImmediateApplies) {
                ++immediate_applies;
                if (ApplyTilePaletteVisual(actor, selected_preset, unselected_preset)) {
                    s_tile_palette.applied_native_colours[i] = native_colour;
                    s_tile_palette.applied_revisions[i] = s_tile_visual_revision;
                }
            } else {
                // Finish the remaining newly detected colours one at a time
                // on subsequent ticks, rather than bursting MIDs here.
                NotifyNativeTileContactCompleted(actor, s_tile_palette.player);
            }
        }
    }
}

// Process one contact per frame without any reflection inside the native hook.
// Blueprint may write the Color field on a subsequent frame, so retry a few
// times before handing it back to the periodic native-state scan.
void ProcessRecentNativeTileContacts(int selected_preset, int unselected_preset) {
    if (!s_tile_palette.cached || s_recent_native_tile_count == 0) return;
    NativeTileContact pending = s_recent_native_tile_contacts[0];
    for (int i = 1; i < s_recent_native_tile_count; ++i)
        s_recent_native_tile_contacts[i - 1] = s_recent_native_tile_contacts[i];
    --s_recent_native_tile_count;

    const auto last = s_tile_palette.actors.begin() + s_tile_palette.count;
    const auto it = std::find(s_tile_palette.actors.begin(), last, pending.actor);
    if (it == last) return;
    const int index = static_cast<int>(it - s_tile_palette.actors.begin());
    int32_t native_colour = -1;
    if (!ReadTileColor(pending.actor, 0x02C4, native_colour,
                       s_tile_palette.tile_class)) return;

    bool newly_selected = false;
    if (RandomTileRGBEnabled()) {
        const auto& record = s_tile_palette.random_colours[index];
        newly_selected = native_colour != 0 && record.observed &&
                         !record.was_selected;
        RecordRandomTileTransition(pending.actor, native_colour != 0);
    }
    const bool dirty = newly_selected ||
        s_tile_palette.applied_native_colours[index] != native_colour ||
        s_tile_palette.applied_revisions[index] != s_tile_visual_revision;
    if (dirty && ApplyTilePaletteVisual(pending.actor, selected_preset, unselected_preset)) {
        s_tile_palette.applied_native_colours[index] = native_colour;
        s_tile_palette.applied_revisions[index] = s_tile_visual_revision;
    }
    if (native_colour == 0 && pending.attempts < 3 &&
        s_recent_native_tile_count < static_cast<int>(s_recent_native_tile_contacts.size())) {
        ++pending.attempts;
        s_recent_native_tile_contacts[s_recent_native_tile_count++] = pending;
    }
}

// A jump pad may colour its supporting tile through Blueprint without a
// BP_IslandChunk contact. Watch the native tile count as well as buttons to
// catch both routes without changing gameplay or increasing idle scan rate.
bool TilePaletteProgressChanged(const LevelState& state) {
    return (state.buttons >= 0 && state.buttons <= kRefunctButtonCount &&
            state.buttons != s_tile_palette.observed_button_count) ||
           (state.platforms >= 0 && state.platforms <= kTotalColourableTiles &&
            state.platforms != s_tile_palette.observed_platform_count);
}

void ObserveTilePaletteProgress(const LevelState& state) {
    const bool valid_buttons = state.buttons >= 0 && state.buttons <= kRefunctButtonCount;
    const bool valid_platforms = state.platforms >= 0 && state.platforms <= kTotalColourableTiles;
    const bool button_advanced = valid_buttons &&
        s_tile_palette.observed_button_count >= 0 &&
        state.buttons > s_tile_palette.observed_button_count;
    const bool platforms_changed = valid_platforms &&
        s_tile_palette.observed_platform_count >= 0 &&
        state.platforms != s_tile_palette.observed_platform_count;
    if (valid_buttons) s_tile_palette.observed_button_count = state.buttons;
    if (valid_platforms) s_tile_palette.observed_platform_count = state.platforms;
    if (!button_advanced && !platforms_changed) return;
    // Finish an in-flight pass instead of resetting to index zero on every
    // new tile: fast movement must not starve the later cached actors.
    if (s_tile_palette.progress_refresh_remaining == 0) {
        s_tile_palette.progress_refresh_cursor = 0;
        s_tile_palette.progress_refresh_remaining = s_tile_palette.count;
    }
    // Retain the three-pass button refresh; two passes catch deferred tile
    // activation. A new event during a pass guarantees another complete pass.
    s_tile_palette.progress_refresh_passes = (std::max)(
        s_tile_palette.progress_refresh_passes, button_advanced ? 3 : 2);
}

// Check only cached Color fields, then enqueue changed tiles for the existing
// one-material-per-tick path. No UObject scan or material calls here.
void RefreshTilesAfterNativeProgress() {
    if (!s_tile_palette.cached || s_tile_palette.progress_refresh_remaining <= 0) return;
    constexpr int kMaxChecksPerTick = 96;
    ColourWorkBudget budget(750);
    const bool random_rgb = RandomTileRGBEnabled();
    for (int checked = 0;
         checked < kMaxChecksPerTick && s_tile_palette.progress_refresh_remaining > 0;
         ++checked) {
        if (checked > 0 && budget.expired()) break;
        const int index = s_tile_palette.progress_refresh_cursor++;
        --s_tile_palette.progress_refresh_remaining;
        if (index < 0 || index >= s_tile_palette.count) continue;
        void* actor = s_tile_palette.actors[index];
        int32_t native_colour = -1;
        if (!ReadTileColor(actor, 0x02C4, native_colour,
                           s_tile_palette.tile_class)) continue;
        const bool newly_selected = random_rgb &&
            s_tile_palette.random_colours[index].observed &&
            s_tile_palette.random_colours[index].was_selected != (native_colour != 0);
        const int32_t last_applied = s_tile_palette.applied_native_colours[index];
        // Newly active tiles may not yet have received their initial palette.
        const bool native_changed = last_applied >= 0
            ? last_applied != native_colour : native_colour != 0;
        if (newly_selected || native_changed)
            NotifyNativeTileContactCompleted(actor, s_tile_palette.player);
    }
    // Catch deferred Blueprint updates without an unlimited scan in one tick.
    if (s_tile_palette.progress_refresh_remaining == 0) {
        if (--s_tile_palette.progress_refresh_passes > 0) {
            s_tile_palette.progress_refresh_cursor = 0;
            s_tile_palette.progress_refresh_remaining = s_tile_palette.count;
        } else {
            s_tile_palette.progress_refresh_passes = 0;
        }
    }
}

} // namespace

void NotifyNativeTileContactCompleted(void* actor, MyCharacter* player) {
    // Fast O(1) class check in the existing hook. No UObject enumeration,
    // no disk I/O, and no new material creation in the contact callback.
    if (!s_tile_palette.cached || !actor || player != s_tile_palette.player ||
        s_tiles.active ||
        !IsReadable(actor, sizeof(UObjectHeader)) ||
        static_cast<const UObjectHeader*>(actor)->class_ptr != s_tile_palette.tile_class)
        return;
    const bool visual_active = s_random_tile_rgb_enabled ||
        (s_custom_colours[0].loaded && s_custom_colours[0].enabled) ||
        (s_custom_colours[1].loaded && s_custom_colours[1].enabled) ||
        (s_wall_colours[0].loaded && s_wall_colours[0].enabled) ||
        (s_wall_colours[1].loaded && s_wall_colours[1].enabled) ||
        s_tile_palette_choices[0] > 0 || s_tile_palette_choices[1] > 0;
    if (!visual_active) return;
    for (int i = 0; i < s_recent_native_tile_count; ++i)
        if (s_recent_native_tile_contacts[i].actor == actor) return;
    if (s_recent_native_tile_count < static_cast<int>(s_recent_native_tile_contacts.size()))
        s_recent_native_tile_contacts[s_recent_native_tile_count++] = {actor, 0};
    // On overflow, the bounded periodic native-state scanner still detects
    // the transition; never block gameplay or allocate from the hook.
}

bool GhostReplayTilesChanged() { return s_ghost_replay_tiles.contact_issued; }

void ResetGhostReplayTiles() {
    if (s_ghost_replay_tiles.contact_issued || s_ghost_replay_tiles.valid_regions > 0)
        Log((L"Ghost replay: activated " +
             std::to_wstring(s_ghost_replay_tiles.contact_count) +
             L" native colour-changing tiles (confirmed)").c_str());
    s_ghost_replay_tiles = {};
}

GhostReplayTileStatus PrepareGhostReplayTiles(MyCharacter* player) {
    auto& replay = s_ghost_replay_tiles;
    const LevelState* progress = PracticeLevelState();
    if (!progress || !player || player != g_player || s_tiles.active ||
        !g_process_event || !g_remove_based_character ||
        !(g_original_add_based_character || g_add_based_character))
        return GhostReplayTileStatus::Unavailable;
    const auto now = GetTickCount64();
    if (replay.player != player || replay.resets != progress->resets) {
        replay = {};
        replay.player = player;
        replay.resets = progress->resets;
        replay.started = now;
    }
    if (replay.unavailable) return GhostReplayTileStatus::Unavailable;
    if (replay.collected == kTotalColourableTiles)
        return GhostReplayTileStatus::Ready;
    // Timeout only discovery of the actor arrays, not bounded geometry work
    // (which can legitimately take longer on a slow machine).
    if (!replay.layout.count) {
        if (now < replay.next_layout_retry) return GhostReplayTileStatus::Pending;
        replay.next_layout_retry = now + 1000;
        TileBatch candidate{};
        if (!CollectNativeTileLayout(player,*progress,candidate) ||
            !PropertyMatches(candidate.tile_class,L"Color",L"IntProperty",candidate.color_offset) ||
            candidate.color_offset != 0x02C4) {
            if (now-replay.started <= 10000) return GhostReplayTileStatus::Pending;
            replay.unavailable = true;
            Log(L"Ghost replay: native FertileLands/Color layout unavailable; tile playback disabled");
            return GhostReplayTileStatus::Unavailable;
        }
        replay.layout = candidate;
        if (!PropertyMatches(candidate.tile_class,L"BaseMesh",L"ObjectProperty",
                             replay.layout.base_mesh_offset) ||
            replay.layout.base_mesh_offset != 0x02BC)
            replay.layout.base_mesh_offset = -1;
        if (void* fn = FindReflectedFunction(candidate.actors[0],L"GetActorBounds")) {
            if (ReplayTileVectorParam(fn,L"Origin",replay.origin_offset) &&
                ReplayTileVectorParam(fn,L"BoxExtent",replay.extent_offset) &&
                std::abs(replay.origin_offset-replay.extent_offset) >=
                    static_cast<int>(sizeof(Vec3))) replay.bounds_fn = fn;
        }
    }
    // Budget one tile (at most five UE calls) per preparation tick.
    // A single malformed/non-visible tile no longer disables the other 250.
    const int index = replay.collected++;
    void* actor = replay.layout.actors[index];
    auto& region = replay.regions[index];
    if (PrepareReplayTileMesh(actor,index,region)) {
        ++replay.mesh_regions;
        ++replay.valid_regions;
    } else if (ReadGhostReplayTileBounds(actor,index,region)) {
        ++replay.valid_regions;
    } else {
        region = {}; // invalid region is excluded from the broad phase
    }
    if (replay.collected != kTotalColourableTiles)
        return GhostReplayTileStatus::Pending;
    // Reject a fully collapsed/duplicated geometry cache. Partial, separated
    // geometry is safe to use and should not disable good actors.
    const ghost_replay_tile_contact::Bounds* anchor = nullptr;
    bool spread = false;
    for (const auto& box : replay.regions) {
        if (!ghost_replay_tile_contact::Valid(box)) continue;
        if (!anchor) anchor = &box;
        else if (std::abs(box.center.x-anchor->center.x)>100.f ||
                 std::abs(box.center.y-anchor->center.y)>100.f) spread=true;
    }
    if (!anchor || (replay.valid_regions > 1 && !spread)) {
        replay.unavailable = true;
        Log(L"Ghost replay: tile geometry missing or collapsed; tile playback disabled");
        return GhostReplayTileStatus::Unavailable;
    }
    Log((L"Ghost replay: cached " + std::to_wstring(replay.valid_regions) +
        L"/251 tile footprints (BaseMesh=" + std::to_wstring(replay.mesh_regions) +
        L", actor fallback=" + std::to_wstring(replay.valid_regions-replay.mesh_regions) +
        L")").c_str());
    return GhostReplayTileStatus::Ready;
}

void TouchGhostReplayTiles(MyCharacter* player, const Vec3& old_pose,
                          const Vec3& new_pose, bool continuous) {
    auto& replay = s_ghost_replay_tiles;
    const LevelState* progress = PracticeLevelState();
    if (replay.unavailable || replay.collected != kTotalColourableTiles ||
        player != replay.player || !progress || progress->resets != replay.resets ||
        s_tiles.active || !g_remove_based_character) return;
    auto native = g_original_add_based_character ?
        g_original_add_based_character : g_add_based_character;
    if (!native) return;
    using ghost_replay_tile_contact::Point;
    const Point start = continuous ? Point{old_pose.x,old_pose.y,old_pose.z} :
                                     Point{new_pose.x,new_pose.y,new_pose.z};
    const Point finish{new_pose.x,new_pose.y,new_pose.z};
    const auto now = GetTickCount64();
    int checked=0, touched=0;
    for (int i=0;i<kTotalColourableTiles;++i) {
        if (replay.attempted[i] || now < replay.retry_after[i]) continue;
        // Cached XY broad phase; query only nearby live tile surfaces. Never
        // treat an unverified/missing geometry entry as a real tile.
        if (!ghost_replay_tile_contact::CrossedXY(start,finish,replay.regions[i]))
            continue;
        if (checked++>=8 || touched>=3) break;
        void* actor = replay.layout.actors[i];
        int32_t color=-1;
        if (!ReadTileColor(actor,replay.layout.color_offset,color,replay.layout.tile_class)) {
            replay.retry_after[i]=now+1000; continue;
        }
        if (color) { replay.attempted[i]=true; continue; }
        ghost_replay_tile_contact::Bounds current{};
        if (!ReadGhostReplayTileBounds(actor,i,current)) {
            replay.retry_after[i]=now+1000; continue;
        }
        if (!ghost_replay_tile_contact::Crossed(start,finish,current)) continue;
        // Use the same contact pair as World -> Select Tiles. Bypass the
        // physical-touch gate only for this replay-owned synthetic action.
        replay.contact_issued=true; // cleanup even if Blueprint confirms later
        native(actor,player);
        g_remove_based_character(actor,player);
        ++touched;
        int32_t after_color=0;
        const LevelState* updated=PracticeLevelState();
        if (!updated || updated->resets != replay.resets ||
            !ReadTileColor(actor,replay.layout.color_offset,after_color,replay.layout.tile_class)) return;
        if (after_color) {
            replay.attempted[i]=true;
            if (++replay.contact_count == 1)
                Log(L"Ghost replay: first native tile activation confirmed");
            NotifyNativeTileContactCompleted(actor,player);
        } else {
            // Report once, rather than silently failing or spamming the hook.
            if (!replay.contact_attempts[i])
                Log((L"Ghost replay: tile " + std::to_wstring(i+1) +
                    L" native contact did not immediately confirm Color").c_str());
            if (++replay.contact_attempts[i] >= 3) replay.attempted[i]=true;
            else replay.retry_after[i]=now+250;
        }
    }
}

bool ButtonTileSelectionDisabledPreference() { LoadButtonTileSetting(); return s_disable_button_tile_selection; }

bool IsButtonTileSelectionDisabled() {
    LoadButtonTileSetting();
    return s_disable_button_tile_selection &&
        g_section_button_contact_hook_available.load(std::memory_order_acquire);
}

bool ToggleButtonTileSelection() {
    LoadButtonTileSetting();
    if (!g_section_button_contact_hook_available.load(std::memory_order_acquire)) {
        Notify(L"Button tile selection: native contact hook unavailable");
        return false;
    }
    s_disable_button_tile_selection = !s_disable_button_tile_selection;
    s_disable_button_tile_selection_loaded = true;
    s_button_tile_guard = {};
    s_button_tile_layout_ready = false;
    s_button_tile_layout_retry_at = 0;
    s_recent_tile_touches = {};
    s_recent_tile_cursor = 0;
    if (!g_settings_path.empty() &&
        !QueueSettingsValue(L"TileInteraction", L"DisableButtonTileSelection",
            s_disable_button_tile_selection ? L"1" : L"0"))
        Log(L"Button tile selection: failed to save setting");
    if (s_disable_button_tile_selection) {
        g_recording_tainted = true;
        g_run_pb_eligible = false;
    }
    Notify(s_disable_button_tile_selection
        ? L"Button tile selection DISABLED (button presses still work)"
        : L"Button tile selection ENABLED");
    return true;
}

bool BeginButtonTileSelectionGuard(void* actor, MyCharacter* player) {
    if (!IsButtonTileSelectionDisabled() || !actor || !player ||
        IsSyntheticPracticeButtonContact() ||
        !g_practice_button_actors_ready ||
        g_restart_requested || g_full_level_reload_requested ||
        (g_practice_active && g_practice_phase != PracticePhase::Ready &&
         g_practice_phase != PracticePhase::Idle)) return false;
    // Compare exact discovered actor pointers; never guess a Blueprint class
    // name or block another ALiftBase object's contact.
    if (std::find(g_practice_button_actors.begin(),
                  g_practice_button_actors.end(), actor) ==
        g_practice_button_actors.end()) return false;

    auto& guard = s_button_tile_guard;
    if (!guard.pending) {
        guard = {};
        if (!SnapshotButtonTiles(guard, player)) {
            // Fail open: the native button should work if tile state cannot
            // be validated. Never perform an unverified counter edit.
            return false;
        }
    }
    if (guard.button_callback_depth == 0) {
        // This setting alters tile progression; do not record an assisted run.
        g_recording_tainted = true;
        g_run_pb_eligible = false;
    }
    ++guard.button_callback_depth;
    return true;
}

void EndButtonTileSelectionGuard() {
    if (s_button_tile_guard.button_callback_depth > 0)
        --s_button_tile_guard.button_callback_depth;
}

void UpdateButtonTileSelectionGuard(MyCharacter* player) {
    auto& guard = s_button_tile_guard;
    if (!guard.pending) {
        // Lazy prewarm so button presses don't need full Unreal reflection.
        if (IsButtonTileSelectionDisabled() && player &&
            !g_restart_requested && !g_full_level_reload_requested &&
            (!g_practice_active || g_practice_phase == PracticePhase::Ready))
            PrepareButtonTileLayout(player);
        return;
    }
    const LevelState* state = PracticeLevelState();
    const ULONGLONG now = GetTickCount64();
    if (!IsButtonTileSelectionDisabled() || !state ||
        player != guard.player || player != g_player ||
        state->resets != guard.resets ||
        g_restart_requested || g_full_level_reload_requested ||
        s_tiles.active ||
        (g_practice_active && g_practice_phase != PracticePhase::Ready &&
         g_practice_phase != PracticePhase::Idle) ||
        now - guard.started_at > 750 ||
        state->buttons < guard.before_buttons ||
        state->buttons > guard.before_buttons + 1) {
        guard = {};
        return;
    }
    for (int i = 0; i < kPracticeClusterCount; ++i) {
        if (g_practice_level_actors[i] != guard.tiles.level_roots[i]) {
            guard = {};
            return;
        }
    }
    if (state->buttons == guard.before_buttons) return;
    // Give the game's Blueprint progression a frame to complete any deferred
    // tile material updates before comparing the exact native Color fields.
    if (!guard.button_progress_observed) {
        guard.button_progress_observed = true;
        return;
    }

    std::array<int, kTotalColourableTiles> newly_selected{};
    int selected_total = 0, newly_count = 0;
    for (int i = 0; i < guard.tiles.count; ++i) {
        int32_t colour = -1;
        if (!ReadTileColor(guard.tiles.actors[i], guard.tiles.color_offset, colour,
                           guard.tiles.tile_class)) {
            Log(L"Button tile selection: native tile state became unreadable");
            guard = {};
            return;
        }
        if (colour != 0) {
            ++selected_total;
            if (guard.old_colours[i] == 0) newly_selected[newly_count++] = i;
        }
    }
    if (selected_total != state->platforms) {
        Log(L"Button tile selection: tile counter mismatch; no tiles changed");
        guard = {};
        return;
    }
    int reversed = 0;
    for (int i = 0; i < newly_count; ++i) {
        void* actor = guard.tiles.actors[newly_selected[i]];
        const LevelState before = *PracticeLevelState();
        if (DeselectOneTile(actor, guard.tiles, before) != TileDeselectResult::Reset) {
            Log(L"Button tile selection: verified tile reversal failed");
            Notify(L"Button tile selection: could not restore tile (see log)");
            break;
        }
        RecordRandomTileTransition(actor, false);
        // Keep the user's custom unselected-tile appearance in sync now;
        // the general palette updater only visits a few tiles per frame.
        if (s_tile_palette.cached)
            ApplyTilePaletteVisual(actor, CurrentTilePaletteChoice(true),
                                   CurrentTilePaletteChoice(false));
        ++reversed;
    }
    if (reversed > 0) {
        g_recording_tainted = true;
        g_run_pb_eligible = false;
        Log((L"Button tile selection: restored " + std::to_wstring(reversed) +
             L" newly selected tiles after button press").c_str());
    }
    guard = {};
}

bool TileSelectionOnContactDisabledPreference() { LoadTileTouchSetting(); return s_disable_tile_selection_on_touch; }

bool IsTileSelectionOnContactDisabled() {
    LoadTileTouchSetting();
    // If the optional detour could not be installed, never claim blocking works.
    return s_disable_tile_selection_on_touch &&
        g_section_button_contact_hook_available.load(std::memory_order_acquire);
}

bool ToggleTileSelectionOnContact() {
    LoadTileTouchSetting();
    if (!g_section_button_contact_hook_available.load(std::memory_order_acquire)) {
        Notify(L"Tile selection: native contact hook unavailable; cannot disable touch");
        return false;
    }
    s_disable_tile_selection_on_touch = !s_disable_tile_selection_on_touch;
    s_disable_tile_selection_on_touch_loaded = true;
    if (!g_settings_path.empty() &&
        !QueueSettingsValue(L"TileInteraction", L"DisableSelectionOnTouch",
                s_disable_tile_selection_on_touch ? L"1" : L"0"))
        Log(L"Tile selection: failed to save DisableSelectionOnTouch setting");
    if (s_disable_tile_selection_on_touch) {
        // Suppressing normal player progression makes this an assisted run.
        g_recording_tainted = true;
        g_run_pb_eligible = false;
    }
    Notify(s_disable_tile_selection_on_touch
        ? L"Tile selection on touch DISABLED (manual Select All still works)"
        : L"Tile selection on touch ENABLED");
    return true;
}

bool ShouldBlockPhysicalTileContact(void* actor, MyCharacter* player) {
    const bool block_touch = IsTileSelectionOnContactDisabled();
    const bool block_button = IsButtonTileSelectionDisabled();
    // Avoid expensive Unreal class reflection when both settings are OFF.
    if (!player || (!block_touch && !block_button) ||
        !TileActorValid(actor, nullptr)) return false;
    if (block_touch || (block_button &&
        s_button_tile_guard.button_callback_depth > 0)) {
        g_recording_tainted = true;
        g_run_pb_eligible = false;
        return true;
    }
    // No button press known yet. Remember this tile's original native state
    // briefly to handle UE4 invoking a tile callback immediately before the
    // button callback on the same physics step.
    const LevelState* level = PracticeLevelState();
    int32_t colour = -1;
    if (!IsSyntheticPracticeButtonContact() && level && player == g_player &&
        ReadTileColor(actor, 0x02C4, colour) && colour == 0) {
        auto& touch = s_recent_tile_touches[
            s_recent_tile_cursor++ % s_recent_tile_touches.size()];
        touch = {actor, player, level->resets, GetTickCount64()};
    }
    return false;
}

// Tile selection needs a read-only height snapshot, not permission to MOVE
// sections. The normal section-placement snapshot requires both K2 getters and
// setters plus strict calibration of all 31 roots. If one of those movement-
// only checks fails, tile controls can still inspect the live native Z values.
// Never infer "above water" from the unlocked level/progression counter.
bool SnapshotRaisedTilesForWorldOperation(
    std::array<bool, kPracticeClusterCount>& raised) {
    if (SnapshotRaisedSections(raised)) return true;

    // Re-acquire the current world if a restart invalidated section placement.
    if ((!g_practice_level_class || !g_practice_level_actors[0]) &&
        !DiscoverPracticeButtons()) {
        Log(L"World tiles: height fallback: native section actors unavailable");
        return false;
    }
    if (!g_process_event || !g_practice_level_class) {
        Log(L"World tiles: height fallback: ProcessEvent/section class unavailable");
        return false;
    }
    void* first = g_practice_level_actors[0];
    if (!IsReadable(first, sizeof(UObjectHeader)) ||
        static_cast<const UObjectHeader*>(first)->class_ptr != g_practice_level_class) {
        Log(L"World tiles: height fallback: first section actor invalid");
        return false;
    }
    void* get_fn = FindReflectedFunction(first, L"K2_GetActorLocation");
    if (!get_fn) get_fn = FindReflectedFunction(first, L"GetActorLocation");
    if (!IsReadable(get_fn, sizeof(UStructHeader))) {
        Log(L"World tiles: height fallback: native GetActorLocation missing");
        return false;
    }
    const int32_t location_offset = FindClassPropertyOffset(get_fn, L"ReturnValue");
    if (location_offset < 0 ||
        location_offset > 128 - static_cast<int32_t>(sizeof(Vec3))) {
        Log(L"World tiles: height fallback: invalid location result layout");
        return false;
    }

    std::array<bool, kPracticeClusterCount> snapshot{};
    for (int section = 0; section < kPracticeClusterCount; ++section) {
        void* actor = g_practice_level_actors[section];
        if (!IsReadable(actor, sizeof(UObjectHeader)) ||
            static_cast<const UObjectHeader*>(actor)->class_ptr != g_practice_level_class) {
            Log((L"World tiles: height fallback: invalid section actor " +
                 std::to_wstring(section + 1)).c_str());
            return false;
        }
        alignas(16) std::array<uint8_t, 128> args{};
        g_process_event(actor, get_fn, args.data());
        Vec3 actual{};
        std::memcpy(&actual, args.data() + location_offset, sizeof(actual));
        if (!std::isfinite(actual.x) || !std::isfinite(actual.y) ||
            !std::isfinite(actual.z) || std::abs(actual.z) > 1000000.0f) {
            Log((L"World tiles: height fallback: unreadable native Z for section " +
                 std::to_wstring(section + 1)).c_str());
            return false;
        }
        // For v9.30 each fully raised native root is Z=0; the section
        // placement feature can add a per-section Z offset. Require the
        // root to have reached that raised endpoint, not merely to be moving
        // upward. Unknown locations fail closed and never trigger tile edits.
        const float raised_z = GetSectionHeightOffset(section + 1);
        snapshot[section] = std::abs(actual.z - raised_z) <= 8.0f;
    }
    raised = snapshot;
    Log(L"World tiles: read-only native height fallback succeeded (31 sections)");
    return true;
}

bool DeselectWorldTiles(bool all_tiles) {
    if (s_tiles.active) {
        Notify(L"Deselect tiles: another tile operation is running");
        return false;
    }
    if (g_practice_active && g_practice_phase != PracticePhase::Ready &&
        g_practice_phase != PracticePhase::Idle) {
        Notify(L"Deselect tiles: finish loading your section first");
        return false;
    }
    const LevelState* level = PracticeLevelState();
    if (!level || level->platforms < 0 || level->platforms > kTotalColourableTiles ||
        !g_player || !IsReadable(g_player, sizeof(MyCharacter)) || !g_process_event) {
        Notify(L"Deselect tiles: native level/player unavailable");
        return false;
    }
    if (level->platforms == 0) {
        Notify(L"Deselect tiles: tile counter is already zero");
        return true;
    }
    std::array<bool, kPracticeClusterCount> raised{};
    if (!all_tiles && !SnapshotRaisedTilesForWorldOperation(raised)) {
        Notify(L"Deselect tiles: cannot read section heights (see log)");
        return false;
    }
    int32_t offset = -1;
    if (!FindFertileLandsOffset(offset)) {
        Notify(L"Deselect tiles: native tile list unavailable");
        return false;
    }
    TileBatch candidate{};
    candidate.deselect = true;
    candidate.include_underwater = all_tiles;
    candidate.initial_count = level->platforms;
    candidate.resets = level->resets;
    candidate.player = g_player;
    int enumerated = 0;
    std::array<void*, kTotalColourableTiles> seen{};
    for (int section = 0; section < kPracticeClusterCount; ++section) {
        void* root = g_practice_level_actors[section];
        if (!root || !IsReadable(root, sizeof(UObjectHeader)) ||
            static_cast<const UObjectHeader*>(root)->class_ptr != g_practice_level_class) {
            Notify(L"Deselect tiles: section root unavailable");
            return false;
        }
        candidate.level_roots[section] = root;
        UObjectPointerArray list{};
        if (!ReadFertileLandList(root, offset, list)) {
            Notify(L"Deselect tiles: invalid tile list");
            return false;
        }
        for (int i = 0; i < list.count; ++i) {
            void* actor = list.data[i];
            if (enumerated >= kTotalColourableTiles ||
                !TileActorValid(actor, candidate.tile_class) ||
                std::find(seen.begin(), seen.begin() + enumerated, actor) !=
                    seen.begin() + enumerated) {
                Notify(L"Deselect tiles: unexpected tile actor");
                return false;
            }
            if (!candidate.tile_class)
                candidate.tile_class = static_cast<const UObjectHeader*>(actor)->class_ptr;
            seen[enumerated++] = actor;
            if (all_tiles || raised[section])
                candidate.actors[candidate.count++] = actor;
        }
    }
    if (enumerated != kTotalColourableTiles || !candidate.tile_class) {
        Notify(L"Deselect tiles: expected 251 native tiles");
        return false;
    }
    if (candidate.count == 0) {
        Notify(L"Deselect tiles: no tiles on raised platforms");
        return false;
    }
    if (!ResolveTileDeselectAbi(candidate)) {
        Log(L"World tiles: cannot validate BP_IslandChunk_C Color/NormalMaterials "
            L"and UMeshComponent::SetMaterial signatures");
        Notify(L"Deselect tiles: native material API unavailable (see log)");
        return false;
    }
    // Never decrement progress if the native tile states are inconsistent
    // with the LevelState count (for example after an older broken reset).
    int actual_coloured = 0;
    for (int i = 0; i < enumerated; ++i) {
        int32_t color = -1;
        if (!ReadTileColor(seen[i], candidate.color_offset, color)) {
            Notify(L"Deselect tiles: unable to read native tile colour state");
            return false;
        }
        if (color) ++actual_coloured;
    }
    if (actual_coloured != level->platforms) {
        Log((L"World tiles: mismatch native coloured actors=" +
             std::to_wstring(actual_coloured) + L" vs progress=" +
             std::to_wstring(level->platforms)).c_str());
        Notify(L"Deselect tiles: colour/counter mismatch (see log; reload level)");
        return false;
    }
    g_recording_tainted = true;
    g_run_pb_eligible = false;
    candidate.active = true;
    s_tiles = candidate;
    MarkCheatAction(CheatAction::Tiles);
    Log((L"World tiles: native colour/material reversal queued for " +
         std::to_wstring(candidate.count) + L" tiles; original progress=" +
         std::to_wstring(candidate.initial_count)).c_str());
    Notify(L"Deselect tiles: restoring native tiles, one per frame");
    return true;
}

bool IsRandomTileRGBEnabled() {
    return RandomTileRGBEnabled();
}

void ToggleRandomTileRGB() {
    s_random_tile_rgb_enabled = !RandomTileRGBEnabled();
    s_random_tile_rgb_loaded = true;
    if (!g_settings_path.empty() &&
        !QueueSettingsValue(L"TileColours", L"RandomSelectedRGB",
                                    s_random_tile_rgb_enabled ? L"1" : L"0"))
        Log(L"Tile colours: failed to save random RGB mode");
    if (s_random_tile_rgb_enabled) {
        // Baseline already-selected tiles without rebuilding all 31 section
        // actor arrays when this is still the same live map. A stale map
        // continues to use the validated full-cache rebuild on the tick.
        const LevelState* state = PracticeLevelState();
        bool reuse = s_tile_palette.cached && state &&
                     s_tile_palette.player == g_player &&
                     s_tile_palette.resets == state->resets;
        if (reuse) {
            for (int i = 0; i < kPracticeClusterCount; ++i) {
                if (s_tile_palette.roots[i] != g_practice_level_actors[i]) {
                    reuse = false;
                    break;
                }
            }
        }
        if (reuse) SnapshotRandomTileBaseline(s_tile_palette);
        else s_tile_palette.cached = false;
    } else {
        // The normal selected preset / HEX applies again on the next sweep.
        s_tile_palette.restore_defaults = true;
    }
    s_tile_palette.cursor = 0;
    s_tile_palette.failures = 0;
    s_recent_native_tile_count = 0;
    InvalidateTileVisuals();
    Notify(s_random_tile_rgb_enabled
           ? L"Random RGB ON: each NEXT selected tile gets its own colour"
           : L"Random RGB OFF: restoring the selected tile colour setting");
}

bool IsTileCustomColourEnabled(bool selected) {
    return CustomColour(selected).enabled;
}

int GetTileColourPreset(bool selected) {
    return CurrentTilePaletteChoice(selected);
}

std::wstring TileColourPresetLabel(bool selected) {
    if (selected && RandomTileRGBEnabled()) return L"Random RGB (new tiles)";
    const auto& custom = CustomColour(selected);
    if (custom.enabled) return L"Custom " + HexString(custom.rgb);
    const int palette = CurrentTilePaletteChoice(selected);
    return palette == 0 ? L"Game default" : L"Palette " + std::to_wstring(palette);
}

void CycleTileColourPreset(bool selected, int direction) {
    const int old = CurrentTilePaletteChoice(selected);
    const int next = (old + (direction < 0 ? 8 : 1)) % 9;
    if (selected && RandomTileRGBEnabled()) {
        // Choosing a normal selected-tile palette exits Random RGB mode.
        s_random_tile_rgb_enabled = false;
        if (!g_settings_path.empty())
            QueueSettingsValue(L"TileColours", L"RandomSelectedRGB",
                                       L"0");
    }
    if (next == old) return;
    s_tile_palette_choices[selected ? 0 : 1] = next;
    CustomColour(selected).enabled = false;
    SaveCustomColour(selected);
    s_tile_palette.cursor = 0;
    s_tile_palette.failures = 0;
    s_recent_native_tile_count = 0;
    InvalidateTileVisuals();
    s_tile_palette.restore_defaults =
        CurrentTilePaletteChoice(true) == 0 && CurrentTilePaletteChoice(false) == 0;
    if (!g_settings_path.empty()) {
        const std::wstring value = std::to_wstring(next);
        if (!QueueSettingsValue(L"TileColours",
                                        selected ? L"SelectedPreset" : L"UnselectedPreset",
                                        value.c_str()))
            Log(L"Tile colours: failed to save palette choice to settings.ini");
    }
    Notify(std::wstring(selected ? L"Selected" : L"Unselected") +
           L" tile colour: " + TileColourPresetLabel(selected) + L" (visual only)");
    Log((std::wstring(L"Tile colours: ") +
         (selected ? L"selected" : L"unselected") + L" visual palette " +
         std::to_wstring(next) + L" (0=game default)").c_str());
}

void RestoreDefaultTileColours() {
    const bool modified = CurrentTilePaletteChoice(true) != 0 ||
        CurrentTilePaletteChoice(false) != 0 ||
        CustomColour(true).enabled || CustomColour(false).enabled ||
        RandomTileRGBEnabled();
    if (!modified) return;

    for (const bool selected : {true, false}) {
        CustomColour(selected).enabled = false;
        SaveCustomColour(selected); // Preserve the last chosen HEX for re-enabling.
    }
    s_tile_palette_choices = {{0, 0}};
    s_random_tile_rgb_enabled = false;
    s_random_tile_rgb_loaded = true;
    if (!g_settings_path.empty()) {
        for (const wchar_t* key : {L"SelectedPreset", L"UnselectedPreset",
                                   L"RandomSelectedRGB"}) {
            if (!QueueSettingsValue(L"TileColours", key, L"0"))
                Log(L"Tile colours: failed to persist default palette reset");
        }
    }
    // The existing per-tick material sweep restores native visuals in small
    // batches. Native selected flags and LevelState.platforms remain intact.
    s_tile_palette.cursor = 0;
    s_tile_palette.failures = 0;
    s_recent_native_tile_count = 0;
    InvalidateTileVisuals();
    s_tile_palette.restore_defaults = true;
    Notify(L"Tile colour presets: game default (selected and unselected)");
}


void ApplyTileColourProfileState(int selected_preset, int unselected_preset,
    bool selected_custom, bool unselected_custom,
    uint32_t selected_rgb, uint32_t unselected_rgb, bool random_selected_rgb) {
    if (selected_preset < 0 || selected_preset > 8 ||
        unselected_preset < 0 || unselected_preset > 8 ||
        selected_rgb > 0xFFFFFFu || unselected_rgb > 0xFFFFFFu) return;
    // These modes are mutually exclusive in the UI; a corrupted profile
    // cannot enable both Random RGB and a custom selected tile colour.
    if (random_selected_rgb) selected_custom = false;
    auto& selected = CustomColour(true);
    auto& unselected = CustomColour(false);
    selected.rgb = selected_rgb;
    selected.enabled = selected_custom;
    unselected.rgb = unselected_rgb;
    unselected.enabled = unselected_custom;
    s_tile_palette_choices = {{selected_preset, unselected_preset}};
    s_random_tile_rgb_loaded = true;
    s_random_tile_rgb_enabled = random_selected_rgb;
    SaveCustomColour(true);
    SaveCustomColour(false);
    if (!g_settings_path.empty()) {
        QueueSettingsValue(L"TileColours", L"SelectedPreset",
            std::to_wstring(selected_preset).c_str());
        QueueSettingsValue(L"TileColours", L"UnselectedPreset",
            std::to_wstring(unselected_preset).c_str());
        QueueSettingsValue(L"TileColours", L"RandomSelectedRGB",
            random_selected_rgb ? L"1" : L"0");
    }
    s_tile_palette.cursor = 0;
    s_tile_palette.failures = 0;
    s_recent_native_tile_count = 0;
    s_tile_palette.restore_defaults = selected_preset == 0 &&
        unselected_preset == 0 && !selected_custom && !unselected_custom &&
        !random_selected_rgb;
    InvalidateTileVisuals();  // Applied via existing cosmetic frame-time budget.
}

void SetTileHexColour(bool selected, uint32_t rgb) {
    if (rgb > 0xFFFFFFu) return;
    auto& custom = CustomColour(selected);
    custom.rgb = rgb;
    custom.enabled = true;
    if (selected && RandomTileRGBEnabled()) {
        s_random_tile_rgb_enabled = false;
        if (!g_settings_path.empty())
            QueueSettingsValue(L"TileColours", L"RandomSelectedRGB",
                                       L"0");
    }
    SaveCustomColour(selected);
    s_tile_palette.cursor = 0;
    s_tile_palette.failures = 0;
    InvalidateTileVisuals();
    s_tile_palette.restore_defaults = false;
    Notify(std::wstring(selected ? L"Selected" : L"Unselected") +
           L" tile HEX: " + HexString(rgb) + L" (visual only)");
}

std::wstring GetTileHexColour(bool selected) {
    return HexString(CustomColour(selected).rgb);
}

bool IsWallCustomColourEnabled(bool selected) {
    return WallColour(selected).enabled;
}

std::wstring WallColourLabel(bool selected) {
    const auto& wall = WallColour(selected);
    return wall.enabled ? L"Custom " + HexString(wall.rgb) : L"Game default";
}

std::wstring GetWallHexColour(bool selected) {
    return HexString(WallColour(selected).rgb);
}

void SetWallCustomColourEnabled(bool selected, bool enabled) {
    auto& wall = WallColour(selected);
    if (wall.enabled == enabled) return;
    wall.enabled = enabled;
    SaveWallColour(selected);
    s_tile_palette.cursor = 0;
    s_tile_palette.failures = 0;
    InvalidateTileVisuals();
    // Preserve a full restoration sweep if this disabled the last override.
    s_tile_palette.restore_defaults =
        !WallColour(true).enabled && !WallColour(false).enabled;
    Notify(std::wstring(selected ? L"Selected" : L"Unselected") +
           (enabled ? L" wall colour: custom RGB" : L" wall colour: game default") +
           L" (visual only)");
}

void SetWallHexColour(bool selected, uint32_t rgb) {
    if (rgb > 0xFFFFFFu) return;
    auto& wall = WallColour(selected);
    wall.rgb = rgb;
    wall.enabled = true;
    SaveWallColour(selected);
    s_tile_palette.cursor = 0;
    s_tile_palette.failures = 0;
    InvalidateTileVisuals();
    s_tile_palette.restore_defaults = false;
    Notify(std::wstring(selected ? L"Selected" : L"Unselected") +
           L" wall HEX: " + HexString(rgb) + L" (visual only)");
}

void OpenColourPickerForTarget(int target, uint32_t starting_rgb) {
    // A pending result belongs to the previous picker and must be committed
    // on the game thread before a new picker can change the target index.
    if (s_picker_result.load(std::memory_order_acquire) != kPickerNoResult) {
        Notify(L"Previous tile colour picker result is still being applied");
        return;
    }
    if (s_picker_open.exchange(true, std::memory_order_acq_rel)) {
        Notify(L"A tile colour picker is already open");
        return;
    }
    const HWND active = GetForegroundWindow();
    DWORD pid{};
    if (active) GetWindowThreadProcessId(active, &pid);
    s_picker_owner.store(pid == GetCurrentProcessId() ? active : nullptr,
                         std::memory_order_relaxed);
    s_picker_start_rgb.store(starting_rgb, std::memory_order_relaxed);
    s_picker_target.store(target, std::memory_order_relaxed);
    s_picker_result.store(kPickerNoResult, std::memory_order_release);
    HANDLE thread = CreateThread(nullptr, 0, ColourPickerThread, nullptr, 0, nullptr);
    if (!thread) {
        s_picker_open.store(false, std::memory_order_release);
        Notify(L"Could not open the Windows colour picker");
        return;
    }
    CloseHandle(thread);
    Notify(L"Windows colour picker opened (Alt+Tab if hidden by fullscreen)");
}

void OpenTileColourPicker(bool selected) {
    OpenColourPickerForTarget(selected ? 0 : 1, CustomColour(selected).rgb);
}

void OpenWallColourPicker(bool selected) {
    OpenColourPickerForTarget(selected ? 2 : 3, WallColour(selected).rgb);
}

void UpdateTileColourPresets(MyCharacter* player) {
    // The native picker thread never touches Unreal; commit on the game thread.
    const uint32_t chosen = s_picker_result.exchange(kPickerNoResult,
                                                       std::memory_order_acq_rel);
    if (chosen <= 0xFFFFFFu) {
        const int target = s_picker_target.load(std::memory_order_relaxed);
        if (target == 2 || target == 3) SetWallHexColour(target == 2, chosen);
        else SetTileHexColour(target == 0, chosen);
    } else if (chosen == kPickerError) {
        Notify(L"Windows colour picker failed to open");
    }
    const int selected = CurrentTilePaletteChoice(true);
    const int unselected = CurrentTilePaletteChoice(false);
    if (selected == 0 && unselected == 0 &&
        !CustomColour(true).enabled && !CustomColour(false).enabled &&
        !WallColour(true).enabled && !WallColour(false).enabled &&
        !s_tile_palette.restore_defaults && !RandomTileRGBEnabled())
        return;
    // The native select/deselect batch writes to the same mesh slots. Wait
    // until it completes, then restore the user's visual preference.
    if (s_tiles.active || g_restart_requested || g_full_level_reload_requested ||
        g_preloaded_restart_requested || g_practice_restart_requested ||
        (g_practice_active && g_practice_phase != PracticePhase::Ready &&
         g_practice_phase != PracticePhase::Idle)) return;
    const LevelState* state = PracticeLevelState();
    if (!player || !state || !IsReadable(player, sizeof(MyCharacter))) return;

    // Actor discovery and each preset sweep run at 20 Hz, but a native tile
    // contact is processed on the very next game tick, not held behind the
    // 50 ms sweep timer. The expensive material change is still game-thread
    // only, outside the physics callback.
    static ULONGLONG next_palette_update_ms = 0;
    const ULONGLONG now = GetTickCount64();
    const bool new_native_progress = s_tile_palette.cached &&
        TilePaletteProgressChanged(*state);
    if (now < next_palette_update_ms && s_recent_native_tile_count == 0 &&
        s_tile_colour_dirty_remaining == 0 && !new_native_progress &&
        s_tile_palette.progress_refresh_remaining == 0) return;
    bool stale = !s_tile_palette.cached || s_tile_palette.player != player ||
                 s_tile_palette.resets != state->resets;
    if (!stale) {
        for (int i = 0; i < kPracticeClusterCount; ++i) {
            if (s_tile_palette.roots[i] != g_practice_level_actors[i]) {
                stale = true;
                break;
            }
        }
    }
    if (stale) {
        const bool restoring = s_tile_palette.restore_defaults;
        s_tile_palette = {};
        s_tile_palette.restore_defaults = restoring;
        s_recent_native_tile_count = 0;
        if (!BuildTilePaletteCache(player, state)) return;
        if (s_tile_colour_dirty_remaining) s_tile_colour_dirty_remaining = s_tile_palette.count;
    }
    // Native platform progress can come from a jump pad instead of direct
    // tile contact. Queue transitions before processing the contact queue so
    // the discovered tile can receive its custom material in this same tick.
    ObserveTilePaletteProgress(*state);
    RefreshTilesAfterNativeProgress();
    ProcessRecentNativeTileContacts(selected, unselected);
    if (now < next_palette_update_ms && s_tile_colour_dirty_remaining == 0) return;
    const bool urgent = s_tile_colour_dirty_remaining > 0;
    next_palette_update_ms = now + (urgent ? 0 : 50);

    // Random RGB's fallback scan remains limited to 20 Hz, even when a
    // different tile/wall palette is being applied across consecutive frames.
    static ULONGLONG next_random_fallback_ms = 0;
    if (now >= next_random_fallback_ms) {
        next_random_fallback_ms = now + 50;
        DetectRandomTileSelections(selected, unselected);
    }
    // Scan a few actors per tick to pick up tiles coloured by the player,
    // by Select All Tiles, or by section practice without a render hook.
    // At 20 Hz, 12 actors per update refresh all 251 within ~1.1 seconds,
    // with far fewer expensive reflected calls than scanning at render FPS.
    constexpr int kPerTick = 20;
    ColourWorkBudget palette_budget(1100);
    for (int n = 0; n < kPerTick && s_tile_palette.cached; ++n) {
        if (n > 0 && palette_budget.expired()) break;
        if (s_tile_colour_dirty_remaining) --s_tile_colour_dirty_remaining;
        if (s_tile_palette.cursor >= s_tile_palette.count) {
            if (selected == 0 && unselected == 0 &&
                !CustomColour(true).enabled && !CustomColour(false).enabled &&
                s_tile_palette.restore_defaults &&
                !WallColour(true).enabled && !WallColour(false).enabled &&
                !RandomTileRGBEnabled()) {
                s_tile_palette.restore_defaults = false;
                Log(L"Tile colours: restored game-default activated materials");
                return;
            }
            s_tile_palette.cursor = 0;
            s_tile_colour_dirty_remaining = 0; // Full palette pass completed.
        }
        const int index = s_tile_palette.cursor++;
        void* actor = s_tile_palette.actors[index];
        int32_t native_colour = -1;
        if (!ReadTileColor(actor, 0x02C4, native_colour,
                           s_tile_palette.tile_class)) {
            if (s_tile_palette.failures++ < 3)
                Log(L"Tile colours: native tile state unreadable; leaving materials unchanged");
            continue;
        }
        if (s_tile_palette.applied_native_colours[index] == native_colour &&
            s_tile_palette.applied_revisions[index] == s_tile_visual_revision)
            continue;
        if (!ApplyTilePaletteVisual(actor, selected, unselected)) {
            // Do not spam the log every frame for a single malformed tile.
            if (s_tile_palette.failures++ < 3)
                Log(L"Tile colours: tile material update failed; leaving gameplay state untouched");
        } else {
            s_tile_palette.applied_native_colours[index] = native_colour;
            s_tile_palette.applied_revisions[index] = s_tile_visual_revision;
        }
    }
    // UE4 Blueprints can replace a material without changing the tile's
    // Color property. Revalidate one tile per 50 ms refresh (never 251 at
    // once) to recover from that case without idle-frame hitching.
    if (!urgent && s_tile_palette.cached && s_tile_palette.count > 0) {
        static uint32_t integrity_cursor = 0;
        const int index = static_cast<int>(integrity_cursor++ %
                                            static_cast<uint32_t>(s_tile_palette.count));
        void* actor = s_tile_palette.actors[index];
        int32_t native_colour = -1;
        if (ReadTileColor(actor, 0x02C4, native_colour, s_tile_palette.tile_class) &&
            ApplyTilePaletteVisual(actor, selected, unselected)) {
            s_tile_palette.applied_native_colours[index] = native_colour;
            s_tile_palette.applied_revisions[index] = s_tile_visual_revision;
        }
    }
}

bool SelectWorldTiles(bool all_tiles) {
    if (s_tiles.active) {
        Notify(L"Select tiles: another tile operation is running");
        return false;
    }
    if (g_practice_active && g_practice_phase != PracticePhase::Ready &&
        g_practice_phase != PracticePhase::Idle) {
        Notify(L"Select tiles: finish loading your section first");
        return false;
    }
    const LevelState* level = PracticeLevelState();
    if (!level || level->platforms < 0 || level->platforms > kTotalColourableTiles ||
        !g_player || !IsReadable(g_player, sizeof(MyCharacter)) ||
        !g_add_based_character || !g_remove_based_character) {
        Notify(L"Select tiles: native level/player unavailable");
        return false;
    }
    if (level->platforms == kTotalColourableTiles) {
        Notify(L"Select tiles: all 251 tiles are already coloured");
        return true;
    }

    // Snapshot actual native Z heights. A tile is 'available' only when its
    // parent section is fully raised (not merely unlocked in LevelState).
    std::array<bool, kPracticeClusterCount> raised{};
    if (!all_tiles && !SnapshotRaisedTilesForWorldOperation(raised)) {
        Notify(L"Select tiles: cannot read section heights (see log)");
        return false;
    }
    int32_t tiles_offset = -1;
    if (!FindFertileLandsOffset(tiles_offset)) {
        Log(L"World tiles: BP_LevelRoot_C::FertileLands is not a reflected array");
        Notify(L"Select tiles: native tile list unavailable");
        return false;
    }

    TileBatch candidate{};
    candidate.resets = level->resets;
    candidate.initial_count = level->platforms;
    candidate.include_underwater = all_tiles;
    candidate.player = g_player;
    int enumerated = 0;
    std::array<void*, kTotalColourableTiles> seen{};
    for (int section = 0; section < kPracticeClusterCount; ++section) {
        void* root = g_practice_level_actors[section];
        if (!root || !IsReadable(root, sizeof(UObjectHeader)) ||
            static_cast<const UObjectHeader*>(root)->class_ptr != g_practice_level_class) {
            Notify(L"Select tiles: a section root is unavailable");
            return false;
        }
        candidate.level_roots[section] = root;
        UObjectPointerArray array{};
        if (!ReadFertileLandList(root, tiles_offset, array)) {
            Notify(L"Select tiles: an invalid native tile array was detected");
            return false;
        }
        for (int i = 0; i < array.count; ++i) {
            void* actor = array.data[i];
            if (!TileActorValid(actor, candidate.tile_class)) {
                Log((L"World tiles: invalid BP_IslandChunk_C in section " +
                     std::to_wstring(section + 1)).c_str());
                Notify(L"Select tiles: native tile actor check failed");
                return false;
            }
            if (!candidate.tile_class)
                candidate.tile_class = static_cast<const UObjectHeader*>(actor)->class_ptr;
            // Refuse duplicate or bogus arrays instead of pressing an actor
            // twice. There are precisely 251 tiles in the stock map.
            if (std::find(seen.begin(), seen.begin() + enumerated, actor) !=
                seen.begin() + enumerated) {
                Notify(L"Select tiles: duplicate native tile detected");
                return false;
            }
            if (enumerated == kTotalColourableTiles) {
                Notify(L"Select tiles: unexpected number of native tiles");
                return false;
            }
            seen[enumerated++] = actor;
            if (all_tiles || raised[section])
                candidate.actors[candidate.count++] = actor;
        }
    }
    if (enumerated != kTotalColourableTiles || !candidate.tile_class) {
        Log((L"World tiles: expected 251 native tiles, enumerated " +
             std::to_wstring(enumerated)).c_str());
        Notify(L"Select tiles: native map does not match the expected 251 tiles");
        return false;
    }
    if (!candidate.count) {
        Notify(L"Select tiles: no tiles are on raised sections");
        return false;
    }

    g_recording_tainted = true;
    g_run_pb_eligible = false;
    candidate.active = true;
    s_tiles = candidate;
    MarkCheatAction(CheatAction::Tiles);
    Log((L"World tiles: queued " + std::to_wstring(candidate.count) +
         (all_tiles ? L" tiles (including underwater)" : L" available tiles") +
         L"; existing coloured: " + std::to_wstring(candidate.initial_count)).c_str());
    Notify(L"Select tiles: colouring " + std::to_wstring(candidate.count) +
           (all_tiles ? L" tiles (all sections)" : L" tiles (raised sections)"));
    return true;
}

void UpdateWorldTileSelection(MyCharacter* player) {
    if (!s_tiles.active) return;
    const LevelState* level = PracticeLevelState();
    if (!level || level->resets != s_tiles.resets ||
        !player || player != s_tiles.player ||
        !IsReadable(player, sizeof(MyCharacter)) ||
        (g_practice_active && g_practice_phase != PracticePhase::Ready &&
         g_practice_phase != PracticePhase::Idle) ||
        g_full_level_reload_requested || g_restart_requested ||
        g_preloaded_restart_requested || g_practice_restart_requested) {
        StopTileBatch(L"level restarted or player changed");
        return;
    }
    for (int i = 0; i < kPracticeClusterCount; ++i) {
        if (g_practice_level_actors[i] != s_tiles.level_roots[i]) {
            StopTileBatch(L"native section actor cache changed");
            return;
        }
    }

    if (s_tiles.deselect && (s_tiles.cursor == s_tiles.count || level->platforms == 0)) {
        const int changed = s_tiles.changed;
        const int total = level->platforms;
        const bool all = s_tiles.include_underwater;
        s_tiles = {};
        Log((L"World tiles: restored native state/material for " +
             std::to_wstring(changed) + L" tiles; remaining=" +
             std::to_wstring(total)).c_str());
        Notify(L"Deselect " + std::wstring(all ? L"all" : L"available") +
               L" tiles: " + std::to_wstring(total) + L"/251 coloured");
        return;
    }
    if (!s_tiles.deselect &&
        (level->platforms >= kTotalColourableTiles || s_tiles.cursor == s_tiles.count)) {
        const int attempted = s_tiles.cursor;
        const int gained = level->platforms - s_tiles.initial_count;
        const int total = level->platforms;
        const bool all = s_tiles.include_underwater;
        Log((L"World tiles: finished " + std::to_wstring(attempted) +
             L" native contacts; tile progress " +
             std::to_wstring(s_tiles.initial_count) + L" -> " +
             std::to_wstring(total)).c_str());
        s_tiles = {};
        Notify(L"Select " + std::wstring(all ? L"all" : L"available") +
               L" tiles: " + std::to_wstring(total) + L"/251 coloured" +
               (gained == 0 ? L" (no new tiles)" : L""));
        return;
    }

    // One actor per game tick to avoid a synchronous 251-object burst.
    // Selection uses native contact callbacks; deselection restores the
    // *verified* Color/material transition and its corresponding progress.
    void* actor = s_tiles.actors[s_tiles.cursor];
    if (!TileActorValid(actor, s_tiles.tile_class)) {
        StopTileBatch(L"tile actor became invalid");
        return;
    }
    ++s_tiles.cursor;
    const LevelState before = *level;
    if (s_tiles.deselect) {
        const TileDeselectResult result = DeselectOneTile(actor, s_tiles, before);
        if (result == TileDeselectResult::Failed) {
            StopTileBatch(L"tile state/material restore failed; see RefunctUtility.log");
            return;
        }
        if (result == TileDeselectResult::Reset) {
            ++s_tiles.changed;
            RecordRandomTileTransition(actor, false);
        }
        return;
    }
    // An explicit menu action must still activate tiles while automatic
    // footstep contact is disabled. Call the MinHook trampoline only for this
    // deliberate batch; never bypass the gate for gameplay physics contacts.
    BasedCharacterFn add_contact = g_add_based_character;
    if (IsTileSelectionOnContactDisabled())
        add_contact = g_original_add_based_character;
    if (!add_contact || !g_remove_based_character) {
        StopTileBatch(L"native tile contact functions unavailable");
        return;
    }
    add_contact(actor, player);
    g_remove_based_character(actor, player);
    const LevelState* updated = PracticeLevelState();
    if (!updated || updated->resets != s_tiles.resets ||
        updated->platforms < before.platforms ||
        updated->platforms > kTotalColourableTiles) {
        StopTileBatch(L"unexpected native progress change");
        return;
    }
    if (updated->platforms > before.platforms) {
        RecordRandomTileTransition(actor, true);
        // The generic palette updater is paused during bulk selection; apply
        // this newly selected tile immediately instead of waiting for all 251.
        if (RandomTileRGBEnabled() && s_tile_palette.cached)
            ApplyTilePaletteVisual(actor, CurrentTilePaletteChoice(true),
                                   CurrentTilePaletteChoice(false));
    }
}

} // namespace refunct

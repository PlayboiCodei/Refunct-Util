#include "refunct/internal_api.hpp"
#include "refunct/colour_work_budget.hpp"
#include <commdlg.h>
#include <unordered_map>

namespace refunct {
namespace {
// BP_BuildingDistributor creates these tall decorative static-mesh components
// using M_Concrete_Rough_Bisque / M_Concrete_Rough_Brown. Do not target
// BP_IslandChunk, moving platforms, or any other concrete surface.
constexpr wchar_t kPillarMesh[] = L"SM_Block_1000x1000x20000";
constexpr size_t kMaxTargets = 4096;
constexpr int kScanBatch = 1024;
constexpr size_t kApplyBatch = 5;
// Refunct's decorative pillar meshes are static within a live world.
// Discover them once per world, never on a periodic timer.
constexpr ULONGLONG kValidateMs = 1500;
constexpr uint32_t kNoPick = 0xFFFFFFFFu;
constexpr uint32_t kCancelled = 0x01000000u;

struct Config {
    bool loaded = false;
    bool enabled = false;
    bool removed = false; // User-classified cosmetic; detach rendering and physics.
    bool visible = true;  // Legacy setting retained internally for restoration.
    uint32_t rgb = 0xC9B59E;
};
Config s_config{};
struct Target {
    void* mesh = nullptr;
    int32_t index = -1;
    int32_t serial = 0;
    void* source = nullptr;
    void* mid = nullptr;
    FNameView parameter{};
    uint32_t rgb = 0xFFFFFFFFu;
    bool original_visible = false;
    uint8_t original_collision = 0;
    void* original_static_mesh = nullptr;
    bool mesh_detached = false;
    bool removal_saved = false;
    bool visual_original_visible = false;
    bool visual_saved = false;
    ULONGLONG next_visibility_check = 0;
    ULONGLONG next_removal_check = 0;
    ULONGLONG next_check = 0;
    ULONGLONG retry_after = 0;
};
std::vector<Target> s_targets{};
MyCharacter* s_player = nullptr;
int32_t s_scan_cursor = -1;
size_t s_apply_cursor = 0;
size_t s_pending = 0;
bool s_discovery_complete = false;
ULONGLONG s_next_update = 0;
ULONGLONG s_save_due = 0;
bool s_save_dirty = false;
bool s_discovery_logged = false;
bool s_failure_logged = false;
bool s_removal_failure_logged = false;
bool s_visibility_failure_logged = false;
bool s_detach_fallback_logged = false;
bool s_detach_success_logged = false;

std::atomic<uint32_t> s_picker_initial{0};
std::atomic<uint32_t> s_picker_result{kNoPick};
std::atomic<bool> s_picker_open{false};

std::wstring Hex(uint32_t rgb) {
    wchar_t result[8]{};
    swprintf_s(result, L"#%06X", rgb & 0xFFFFFFu);
    return result;
}
uint32_t ParseHex(const wchar_t* value, uint32_t fallback) {
    if (!value) return fallback;
    if (*value == L'#') ++value;
    if (wcslen(value) != 6) return fallback;
    uint32_t rgb = 0;
    for (int i = 0; i < 6; ++i) {
        const wchar_t c = value[i];
        const int digit = c >= L'0' && c <= L'9' ? c - L'0' :
                          c >= L'a' && c <= L'f' ? c - L'a' + 10 :
                          c >= L'A' && c <= L'F' ? c - L'A' + 10 : -1;
        if (digit < 0) return fallback;
        rgb = (rgb << 4) | static_cast<uint32_t>(digit);
    }
    return rgb;
}
Config& Settings() {
    if (s_config.loaded || g_settings_path.empty()) return s_config;
    wchar_t hex[24]{};
    GetPrivateProfileStringW(L"PillarColours", L"Hex", L"#C9B59E", hex,
        static_cast<DWORD>(std::size(hex)), g_settings_path.c_str());
    s_config.rgb = ParseHex(hex, s_config.rgb);
    s_config.enabled = GetPrivateProfileIntW(L"PillarColours", L"CustomEnabled", 0,
        g_settings_path.c_str()) != 0;
    // The old cosmetic visibility toggle has been replaced by Remove Pillars.
    // Discard a persisted Visible=0 so switching removal OFF restores the
    // original mesh visibility instead of leaving a legacy hidden override.
    s_config.visible = true;
    s_config.removed = GetPrivateProfileIntW(L"PillarColours", L"Removed", 0,
        g_settings_path.c_str()) != 0;
    s_config.loaded = true;
    return s_config;
}
void SaveNow() {
    if (g_settings_path.empty()) return;
    const auto& setting = Settings();
    const bool hex_ok = QueueSettingsValue(L"PillarColours", L"Hex",
        Hex(setting.rgb).c_str()) != 0;
    const bool enabled_ok = QueueSettingsValue(L"PillarColours", L"CustomEnabled",
        setting.enabled ? L"1" : L"0") != 0;
    const bool removal_ok = QueueSettingsValue(L"PillarColours", L"Removed",
        setting.removed ? L"1" : L"0") != 0;
    const bool visibility_ok = QueueSettingsValue(L"PillarColours", L"Visible",
        setting.visible ? L"1" : L"0") != 0;
    if (!hex_ok || !enabled_ok || !removal_ok || !visibility_ok)
        Log(L"Pillars: could not persist settings");
}
void Dirty() {
    // Save outside menu action processing to reduce keyboard-toggle hitches.
    s_save_dirty = true;
    s_save_due = GetTickCount64() + 350;
    s_apply_cursor = 0;
    s_pending = s_targets.size();
    s_next_update = 0;
    for (auto& target : s_targets) {
        target.next_removal_check = 0;
        target.next_visibility_check = 0;
    }
    // Settings changes reuse the existing pillar cache. Only an unfinished
    // initial discovery is allowed to resume; no completed registry rescans.
    if ((Settings().enabled || Settings().removed || !Settings().visible) &&
        !s_discovery_complete && s_scan_cursor < 0) {
        s_scan_cursor = 0;
    }
}
DWORD WINAPI PickerThread(LPVOID) {
    COLORREF swatches[16]{};
    CHOOSECOLORW picker{};
    picker.lStructSize = sizeof(picker);
    picker.lpCustColors = swatches;
    const uint32_t initial = s_picker_initial.load(std::memory_order_relaxed);
    picker.rgbResult = RGB((initial >> 16) & 255, (initial >> 8) & 255, initial & 255);
    picker.Flags = CC_FULLOPEN | CC_ANYCOLOR | CC_RGBINIT;
    if (ChooseColorW(&picker)) {
        s_picker_result.store((static_cast<uint32_t>(GetRValue(picker.rgbResult)) << 16) |
                              (static_cast<uint32_t>(GetGValue(picker.rgbResult)) << 8) |
                              static_cast<uint32_t>(GetBValue(picker.rgbResult)),
                              std::memory_order_release);
    } else s_picker_result.store(kCancelled, std::memory_order_release);
    s_picker_open.store(false, std::memory_order_release);
    return 0;
}
std::wstring ObjectName(void* object) {
    return IsReadable(object, sizeof(UObjectHeader))
        ? UnrealName(static_cast<const UObjectHeader*>(object)->name) : L"";
}
std::wstring ClassName(void* object) {
    if (!IsReadable(object, sizeof(UObjectHeader))) return L"";
    return ObjectName(static_cast<const UObjectHeader*>(object)->class_ptr);
}
bool Inherits(void* object, const wchar_t* name) {
    if (!IsReadable(object, sizeof(UObjectHeader))) return false;
    void* cls = static_cast<const UObjectHeader*>(object)->class_ptr;
    for (int i = 0; i < 12 && IsReadable(cls, sizeof(UStructHeader)); ++i) {
        if (ObjectName(cls) == name) return true;
        cls = static_cast<const UStructHeader*>(cls)->super_struct;
    }
    return false;
}
bool IsDistributorOwnedMesh(void* object) {
    if (!IsReadable(object, sizeof(UObjectHeader))) return false;
    const auto* header = static_cast<const UObjectHeader*>(object);
    if ((static_cast<uint32_t>(header->flags) & 0x30u) || !header->outer) return false;
    // The class test is cached because global object discovery sees many
    // unrelated UObjects. An object pointer is still validated before use.
    static std::unordered_map<void*, bool> mesh_classes;
    bool mesh = false;
    if (const auto it = mesh_classes.find(header->class_ptr); it != mesh_classes.end())
        mesh = it->second;
    else {
        mesh = Inherits(object, L"StaticMeshComponent");
        if (mesh_classes.size() < 1024) mesh_classes.emplace(header->class_ptr, mesh);
    }
    if (!mesh) return false;
    void* owner = header->outer;
    for (int i = 0; i < 4 && IsReadable(owner, sizeof(UObjectHeader)); ++i) {
        if (ClassName(owner) == L"BP_BuildingDistributor_C") {
            return true;
        }
        owner = static_cast<const UObjectHeader*>(owner)->outer;
    }
    return false;
}
bool IsDistributorComponent(void* object) {
    return IsDistributorOwnedMesh(object) &&
        ObjectName(ReadReflectedObject(object, L"StaticMesh")) == kPillarMesh;
}
const GlobalObjectArray* Objects() {
    if (!g_game_module) return nullptr;
    const auto* objects = reinterpret_cast<const GlobalObjectArray*>(
        reinterpret_cast<uintptr_t>(g_game_module) + kGlobalObjectArrayRva);
    if (!IsReadable(objects, sizeof(GlobalObjectArray)) ||
        objects->num_elements < 0 || objects->num_elements > 1000000 ||
        objects->max_elements < objects->num_elements || !objects->objects ||
        !IsReadable(objects->objects,
                    static_cast<size_t>(objects->num_elements) * sizeof(GlobalObjectItem)))
        return nullptr;
    return objects;
}
bool IsLive(void* object, const GlobalObjectArray* objects) {
    if (!objects || !IsReadable(object, sizeof(UObjectHeader))) return false;
    const int32_t index = static_cast<const UObjectHeader*>(object)->index;
    return index >= 0 && index < objects->num_elements &&
        objects->objects[index].object == object;
}
bool Valid(const Target& target, const GlobalObjectArray* objects) {
    return objects && target.index >= 0 && target.index < objects->num_elements &&
        objects->objects[target.index].object == target.mesh &&
        objects->objects[target.index].serial_number == target.serial;
}
// Require the real UE4 reflected property type and offset. In particular,
// an FName is 8 bytes, NOT an arbitrary temporary string or pointer.
int32_t ParameterOffset(void* fn, const wchar_t* property, const wchar_t* type) {
    if (!IsReadable(fn, sizeof(UStructHeader))) return -1;
    void* field = static_cast<const UStructHeader*>(fn)->children;
    for (int i = 0; i < 48 && IsReadable(field, sizeof(UFieldHeader)); ++i) {
        const auto* p = static_cast<const UFieldHeader*>(field);
        if (UnrealName(p->object.name) == property &&
            ObjectName(p->object.class_ptr) == type) {
            const int32_t offset = FindClassPropertyOffset(fn, property);
            const int32_t bytes = wcscmp(type, L"NameProperty") == 0 ? 8 :
                                  wcscmp(type, L"StructProperty") == 0 ? 16 : 4;
            return offset >= 0 && offset + bytes <= 96 ? offset : -1;
        }
        field = p->next;
    }
    return -1;
}
struct Call { void* fn = nullptr; int32_t first = -1; int32_t second = -1; };
Call ResolveCall(void* object, int action) {
    if (!g_process_event || !IsReadable(object, sizeof(UObjectHeader))) return {};
    void* cls = static_cast<const UObjectHeader*>(object)->class_ptr;
    static std::unordered_map<void*, Call> reads, writes, colours;
    auto& cache = action == 0 ? reads : action == 1 ? writes : colours;
    if (const auto it = cache.find(cls); it != cache.end() &&
        IsReadable(it->second.fn, sizeof(UStructHeader))) return it->second;
    Call result{};
    result.fn = FindReflectedFunction(object,
        action == 0 ? L"GetMaterial" : action == 1 ? L"SetMaterial" : L"SetVectorParameterValue");
    if (action == 2 && !result.fn)
        result.fn = FindReflectedFunction(object, L"K2_SetVectorParameterValue");
    result.first = ParameterOffset(result.fn,
        action == 2 ? L"ParameterName" : L"ElementIndex",
        action == 2 ? L"NameProperty" : L"IntProperty");
    result.second = ParameterOffset(result.fn,
        action == 0 ? L"ReturnValue" : action == 1 ? L"Material" : L"Value",
        action == 2 ? L"StructProperty" : L"ObjectProperty");
    if (result.fn && result.first >= 0 && result.second >= 0 && cache.size() < 1024)
        cache.emplace(cls, result);
    return result;
}
void* GetMaterial(void* mesh) {
    const auto call = ResolveCall(mesh, 0);
    if (!call.fn || call.first < 0 || call.second < 0) return nullptr;
    alignas(16) std::array<uint8_t, 96> args{};
    g_process_event(mesh, call.fn, args.data());
    void* material = nullptr;
    std::memcpy(&material, args.data() + call.second, sizeof(material));
    return IsReadable(material, sizeof(UObjectHeader)) ? material : nullptr;
}
bool SetMaterial(void* mesh, void* material) {
    if (!mesh || !IsReadable(material, sizeof(UObjectHeader))) return false;
    const auto call = ResolveCall(mesh, 1);
    if (!call.fn || call.first < 0 || call.second < 0) return false;
    alignas(16) std::array<uint8_t, 96> args{};
    std::memcpy(args.data() + call.second, &material, sizeof(material));
    g_process_event(mesh, call.fn, args.data());
    return GetMaterial(mesh) == material;
}
bool NativeColourName(void* material, FNameView& out) {
    // Both concrete materials are cooked MICs with a native Color override.
    // Extract the actual FName from that override; never guess an FName index.
    void* current = material;
    for (int depth = 0; depth < 4 && IsReadable(current, sizeof(UObjectHeader)); ++depth) {
        const std::wstring name = ObjectName(current);
        if (name == L"M_Concrete_Rough_Bisque" || name == L"M_Concrete_Rough_Brown") {
            void* cls = static_cast<const UObjectHeader*>(current)->class_ptr;
            const int32_t offset = FindClassPropertyOffset(cls, L"VectorParameterValues");
            if (offset < 0 || offset > 0x400 ||
                !IsReadable(static_cast<uint8_t*>(current) + offset,
                            sizeof(UObjectPointerArray))) return false;
            UObjectPointerArray values{};
            std::memcpy(&values, static_cast<uint8_t*>(current) + offset, sizeof(values));
            // Both verified cooked MICs contain one native Color override.
            // Refuse ambiguous arrays: their entry stride is UE4-version specific.
            if (values.count != 1 || !IsReadable(values.data, sizeof(FNameView)))
                return false;
            std::memcpy(&out, values.data, sizeof(FNameView));
            return UnrealName(out) == L"Color";
        }
        current = ReadReflectedObject(current, L"Parent");
    }
    return false;
}
void* CreateMID(void* mesh, void* source) {
    struct Candidate { const wchar_t* method; const wchar_t* parent; bool named; };
    constexpr Candidate methods[] = {
        {L"CreateDynamicMaterialInstance", L"SourceMaterial", true},
        {L"CreateAndSetMaterialInstanceDynamicFromMaterial", L"Parent", false},
        {L"CreateAndSetMaterialInstanceDynamic", nullptr, false}
    };
    for (const auto& m : methods) {
        void* fn = FindReflectedFunction(mesh, m.method);
        const int32_t index = ParameterOffset(fn, L"ElementIndex", L"IntProperty");
        const int32_t result = ParameterOffset(fn, L"ReturnValue", L"ObjectProperty");
        const int32_t parent = m.parent ? ParameterOffset(fn, m.parent, L"ObjectProperty") : -1;
        const int32_t name = m.named ? ParameterOffset(fn, L"OptionalName", L"NameProperty") : -1;
        if (index < 0 || result < 0 || (m.parent && parent < 0) || (m.named && name < 0))
            continue;
        if (!m.parent && GetMaterial(mesh) != source && !SetMaterial(mesh, source)) break;
        alignas(16) std::array<uint8_t, 96> args{};
        if (parent >= 0) std::memcpy(args.data() + parent, &source, sizeof(source));
        g_process_event(mesh, fn, args.data());
        void* mid = nullptr;
        std::memcpy(&mid, args.data() + result, sizeof(mid));
        if (IsReadable(mid, sizeof(UObjectHeader)) && GetMaterial(mesh) == mid &&
            ClassName(mid).find(L"MaterialInstanceDynamic") != std::wstring::npos)
            return mid;
    }
    if (GetMaterial(mesh) != source) SetMaterial(mesh, source);
    return nullptr;
}
bool SetTint(void* mid, FNameView parameter, uint32_t rgb) {
    const Call call = ResolveCall(mid, 2);
    if (!call.fn || call.first < 0 || call.second < 0) return false;
    const auto linear = [](uint32_t value) {
        const float c = static_cast<float>(value) / 255.0f;
        return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    };
    const LinearColor value{linear((rgb >> 16) & 255), linear((rgb >> 8) & 255),
                            linear(rgb & 255), 1.0f};
    alignas(16) std::array<uint8_t, 96> args{};
    std::memcpy(args.data() + call.first, &parameter, sizeof(parameter));
    std::memcpy(args.data() + call.second, &value, sizeof(value));
    g_process_event(mid, call.fn, args.data());
    return true;
}
// Prefer detaching the *StaticMesh asset* from the verified component. UE4
// removes its render/physics mesh while the construction-script component
// remains allocated and can be restored without a level reload. Never destroy
// the component/actor: that would require rerunning the level construction.
bool SetStaticMeshAsset(void* component, void* mesh) {
    if (!g_process_event || !IsReadable(component, sizeof(UObjectHeader)) ||
        (mesh && !IsReadable(mesh, sizeof(UObjectHeader)))) return false;
    void* fn = FindReflectedFunction(component, L"SetStaticMesh");
    const int32_t arg = ParameterOffset(fn, L"NewMesh", L"ObjectProperty");
    if (!fn || arg < 0 || arg + static_cast<int32_t>(sizeof(mesh)) > 96) return false;
    alignas(16) std::array<uint8_t, 96> params{};
    std::memcpy(params.data() + arg, &mesh, sizeof(mesh));
    g_process_event(component, fn, params.data());
    return ReadReflectedObject(component, L"StaticMesh") == mesh;
}
// Fallback for games where SetStaticMesh cannot be resolved or used: disable
// rendering *and* native collision, preserving both original component states.
// Runtime "removal" disables both rendering and native collision on the
// verified decorative pillar components. Never DestroyComponent: the level
// cannot reconstruct those construction-script components without a reload.
// Preserve the exact original states so this single toggle can restore them
// without resetting gameplay, and validate writes through reflected getters.
struct VisibilityCall {
    void* get = nullptr;
    void* set = nullptr;
    int32_t result = -1;
    int32_t value = -1;
    int32_t propagate = -1;
};
VisibilityCall ResolveVisibility(void* component) {
    if (!g_process_event || !IsReadable(component, sizeof(UObjectHeader))) return {};
    void* cls = static_cast<const UObjectHeader*>(component)->class_ptr;
    static std::unordered_map<void*, VisibilityCall> cache;
    if (const auto it = cache.find(cls); it != cache.end() &&
        IsReadable(it->second.get, sizeof(UStructHeader)) &&
        IsReadable(it->second.set, sizeof(UStructHeader))) return it->second;
    VisibilityCall api{};
    api.get = FindReflectedFunction(component, L"IsVisible");
    api.set = FindReflectedFunction(component, L"SetVisibility");
    api.result = ParameterOffset(api.get, L"ReturnValue", L"BoolProperty");
    api.value = ParameterOffset(api.set, L"bNewVisibility", L"BoolProperty");
    api.propagate = ParameterOffset(api.set, L"bPropagateToChildren", L"BoolProperty");
    if (!api.get || !api.set || api.result < 0 || api.value < 0 ||
        api.propagate < 0 || api.value == api.propagate) return {};
    if (cache.size() < 1024) cache[cls] = api;
    return api;
}
bool ReadVisibility(void* component, const VisibilityCall& api, bool& visible) {
    if (!api.get || api.result < 0 || !IsReadable(component, sizeof(UObjectHeader)))
        return false;
    alignas(16) std::array<uint8_t, 96> args{};
    g_process_event(component, api.get, args.data());
    if (args[api.result] > 1) return false;
    visible = args[api.result] != 0;
    return true;
}
bool WriteVisibility(void* component, const VisibilityCall& api, bool visible) {
    if (!api.set || api.value < 0 || api.propagate < 0) return false;
    alignas(16) std::array<uint8_t, 96> args{};
    args[api.value] = visible ? 1 : 0;
    args[api.propagate] = 0; // Do not hide any attached non-pillar component.
    g_process_event(component, api.set, args.data());
    bool observed = false;
    return ReadVisibility(component, api, observed) && observed == visible;
}
struct CollisionCall {
    void* get = nullptr;
    void* set = nullptr;
    int32_t result = -1;
    int32_t value = -1;
};
CollisionCall ResolveCollision(void* component) {
    if (!g_process_event || !IsReadable(component, sizeof(UObjectHeader))) return {};
    void* cls = static_cast<const UObjectHeader*>(component)->class_ptr;
    static std::unordered_map<void*, CollisionCall> cache;
    if (const auto it = cache.find(cls); it != cache.end() &&
        IsReadable(it->second.get, sizeof(UStructHeader)) &&
        IsReadable(it->second.set, sizeof(UStructHeader))) return it->second;
    CollisionCall api{};
    api.get = FindReflectedFunction(component, L"GetCollisionEnabled");
    api.set = FindReflectedFunction(component, L"SetCollisionEnabled");
    // Refunct v9.30 exposes ECollisionEnabled as one byte (0..3) in these
    // reflected functions. Do not assume every component starts at QueryAndPhysics.
    if (!api.get || !api.set) return {};
    api.result = FindClassPropertyOffset(api.get, L"ReturnValue");
    api.value = FindClassPropertyOffset(api.set, L"NewType");
    if (!api.get || !api.set || api.result < 0 || api.result >= 96 ||
        api.value < 0 || api.value >= 96) return {};
    if (cache.size() < 1024) cache[cls] = api;
    return api;
}
bool ReadCollision(void* component, const CollisionCall& api, uint8_t& mode) {
    if (!api.get || api.result < 0 || !IsReadable(component, sizeof(UObjectHeader)))
        return false;
    alignas(16) std::array<uint8_t, 96> args{};
    g_process_event(component, api.get, args.data());
    if (args[api.result] > 3) return false;
    mode = args[api.result];
    return true;
}
bool WriteCollision(void* component, const CollisionCall& api, uint8_t mode) {
    if (!api.set || api.value < 0 || mode > 3) return false;
    alignas(16) std::array<uint8_t, 96> args{};
    args[api.value] = mode;
    g_process_event(component, api.set, args.data());
    uint8_t observed = 255;
    return ReadCollision(component, api, observed) && observed == mode;
}
void ApplyRemoval(Target& target, const GlobalObjectArray* objects,
                  bool removed, ULONGLONG now) {
    if (now < target.next_removal_check) return;
    // Once detached, the component has no StaticMesh property value; keep it
    // tracked by its UObject serial and its verified distributor ownership.
    if (target.mesh_detached) {
        if (removed) {
            if (ReadReflectedObject(target.mesh, L"StaticMesh") != nullptr &&
                !SetStaticMeshAsset(target.mesh, nullptr)) {
                target.next_removal_check = now + 3000;
                return;
            }
        } else {
            if (!IsLive(target.original_static_mesh, objects) ||
                ObjectName(target.original_static_mesh) != kPillarMesh ||
                !SetStaticMeshAsset(target.mesh, target.original_static_mesh)) {
                target.next_removal_check = now + 3000;
                if (!s_removal_failure_logged) {
                    s_removal_failure_logged = true;
                    Log(L"Pillars: could not restore original StaticMesh asset");
                }
                return;
            }
            target.mesh_detached = false;
            target.original_static_mesh = nullptr;
            target.removal_saved = false;
        }
        target.next_removal_check = now + kValidateMs;
        return;
    }
    // Try true scene-geometry detachment once before the compatibility fallback.
    if (removed && !target.removal_saved) {
        void* original = ReadReflectedObject(target.mesh, L"StaticMesh");
        if (IsLive(original, objects) && ObjectName(original) == kPillarMesh &&
            SetStaticMeshAsset(target.mesh, nullptr)) {
            target.original_static_mesh = original;
            target.mesh_detached = true;
            target.removal_saved = true;
            target.next_removal_check = now + kValidateMs;
            if (!s_detach_success_logged) {
                s_detach_success_logged = true;
                Log(L"Pillars: native StaticMesh asset detached (render + physics)");
            }
            return;
        }
        if (!s_detach_fallback_logged) {
            s_detach_fallback_logged = true;
            Log(L"Pillars: SetStaticMesh unavailable; using visual/collision fallback");
        }
    }
    // Visibility and physics changes are paired. Don't hide a solid pillar
    // if the collision API is missing: the toggle must remove both effects.
    if (now < target.next_removal_check || (!removed && !target.removal_saved)) return;
    const VisibilityCall visibility = ResolveVisibility(target.mesh);
    const CollisionCall collision = ResolveCollision(target.mesh);
    bool current_visible = false;
    uint8_t current_collision = 255;
    if (!visibility.get || !visibility.set || !collision.get || !collision.set ||
        !ReadVisibility(target.mesh, visibility, current_visible) ||
        !ReadCollision(target.mesh, collision, current_collision)) {
        target.next_removal_check = now + 3000;
        if (!s_removal_failure_logged) {
            s_removal_failure_logged = true;
            Log(L"Pillars: missing native visibility/collision getter or setter");
        }
        return;
    }
    if (removed && !target.removal_saved) {
        target.original_visible = current_visible;
        target.original_collision = current_collision;
        target.removal_saved = true;
    }
    const bool desired_visible = removed ? false : target.original_visible;
    const uint8_t desired_collision = removed ? 0 : target.original_collision;
    // Stop physics first so the pillar cannot remain invisible but solid.
    if ((current_collision != desired_collision &&
         !WriteCollision(target.mesh, collision, desired_collision)) ||
        (current_visible != desired_visible &&
         !WriteVisibility(target.mesh, visibility, desired_visible))) {
        target.next_removal_check = now + 3000;
        if (!s_removal_failure_logged) {
            s_removal_failure_logged = true;
            Log(L"Pillars: native removal/restore write failed verification");
        }
        return;
    }
    target.next_removal_check = now + kValidateMs;
    if (!removed) target.removal_saved = false;
}
// Cosmetic-only visibility is independent from world removal. While the mesh
// is removed, defer visual writes: restoring removal first must not overwrite
// the user's visibility preference or capture the removed state as "original".
void ApplyVisibility(Target& target, bool visible, bool removed, ULONGLONG now) {
    if (removed || target.removal_saved || target.mesh_detached ||
        now < target.next_visibility_check || (visible && !target.visual_saved)) return;
    const VisibilityCall api = ResolveVisibility(target.mesh);
    bool current = false;
    if (!api.get || !api.set || !ReadVisibility(target.mesh, api, current)) {
        target.next_visibility_check = now + 3000;
        if (!s_visibility_failure_logged) {
            s_visibility_failure_logged = true;
            Log(L"Pillars: could not read native visibility for cosmetic toggle");
        }
        return;
    }
    if (!visible && !target.visual_saved) {
        target.visual_original_visible = current;
        target.visual_saved = true;
    }
    const bool desired = visible ? target.visual_original_visible : false;
    if (current != desired && !WriteVisibility(target.mesh, api, desired)) {
        target.next_visibility_check = now + 3000;
        if (!s_visibility_failure_logged) {
            s_visibility_failure_logged = true;
            Log(L"Pillars: native visibility write failed verification");
        }
        return;
    }
    target.next_visibility_check = now + kValidateMs;
    if (visible) target.visual_saved = false;
}
void Apply(Target& target, const GlobalObjectArray* objects, bool enabled, uint32_t rgb,
           ULONGLONG now) {
    if (now < target.retry_after) return;
    if (enabled && target.mid && target.rgb == rgb && now < target.next_check) return;
    if (!IsDistributorComponent(target.mesh)) return;
    void* displayed = GetMaterial(target.mesh);
    if (!displayed) return;
    const bool own = target.mid && target.source && displayed == target.mid &&
        IsLive(target.mid, objects) && IsLive(target.source, objects);
    if (!enabled) {
        // If game logic replaced the MID, never overwrite its newer material.
        if (own && !SetMaterial(target.mesh, target.source)) return;
        target.mid = nullptr;
        target.source = nullptr;
        target.rgb = 0xFFFFFFFFu;
        return;
    }
    if (own && target.rgb == rgb) {
        target.next_check = now + kValidateMs;
        return;
    }
    FNameView parameter{};
    void* source = own ? target.source : displayed;
    if (own) parameter = target.parameter;
    else if (!NativeColourName(source, parameter)) {
        target.retry_after = now + 3000;
        if (!s_failure_logged) {
            s_failure_logged = true;
            Log(L"Pillar colours: material is not the verified Bisque/Brown Color MIC");
        }
        return;
    }
    void* mid = own ? target.mid : CreateMID(target.mesh, source);
    if (!mid || !SetTint(mid, parameter, rgb)) {
        if (mid && !own) SetMaterial(target.mesh, source);
        target.retry_after = now + 3000;
        if (!s_failure_logged) {
            s_failure_logged = true;
            Log(L"Pillar colours: could not create/tint verified pillar MID");
        }
        return;
    }
    target.source = source;
    target.mid = mid;
    target.parameter = parameter;
    target.rgb = rgb;
    target.next_check = now + kValidateMs;
}
void Discover(const GlobalObjectArray* objects) {
    ColourWorkBudget budget(500);
    const int32_t end = (std::min)(objects->num_elements, s_scan_cursor + kScanBatch);
    int examined = 0;
    for (; s_scan_cursor < end; ++s_scan_cursor) {
        if ((examined & 31) == 0 && budget.expired()) break;
        ++examined;
        const auto& item = objects->objects[s_scan_cursor];
        if (!item.object || (static_cast<uint32_t>(item.flags) & 0x30000000u) ||
            !IsDistributorComponent(item.object)) continue;
        const auto already = std::find_if(s_targets.begin(), s_targets.end(),
            [&](const Target& t) {return t.mesh == item.object && t.serial == item.serial_number;});
        if (already != s_targets.end()) continue;
        if (s_targets.size() >= kMaxTargets) break;
        Target t{};
        t.mesh = item.object;
        t.index = s_scan_cursor;
        t.serial = item.serial_number;
        if (s_pending == 0) s_apply_cursor = s_targets.size();
        ++s_pending;
        s_targets.push_back(t);
    }
    if (s_scan_cursor >= objects->num_elements || s_targets.size() >= kMaxTargets) {
        s_scan_cursor = -1;
        s_discovery_complete = true;
        if (!s_discovery_logged) {
            s_discovery_logged = true;
            Log((L"Pillars: identified " + std::to_wstring(s_targets.size()) +
                 L" BP_BuildingDistributor decorative pillar components").c_str());
        }
    }
}
} // namespace

bool IsPillarColourEnabled() { return Settings().enabled; }
bool ArePillarsRemoved() { return Settings().removed; }
bool ArePillarsVisible() { return Settings().visible; }
void SetPillarsVisible(bool visible) {
    if (Settings().visible == visible) return;
    Settings().visible = visible;
    Dirty();
    Notify(visible ? L"Pillars visible: ON (render only)" :
                     L"Pillars visible: OFF (collision retained)");
}
void SetPillarsRemoved(bool removed) {
    if (Settings().removed == removed) return;
    Settings().removed = removed;
    Dirty();
    Notify(removed ? L"Pillars removed: ON (render + collision)" :
                     L"Pillars removed: OFF (restoring)");
}
void SetPillarColourEnabled(bool enabled) {
    if (Settings().enabled == enabled) return;
    Settings().enabled = enabled;
    Dirty();
    Notify(enabled ? L"Pillar colours: Custom (visual only)" :
                     L"Pillar colours: Game default (visual only)");
}
std::wstring GetPillarColourHex() { return Hex(Settings().rgb); }
void SetPillarColourHex(uint32_t rgb) {
    if (rgb > 0xFFFFFFu || (Settings().rgb == rgb && Settings().enabled)) return;
    Settings().rgb = rgb;
    Settings().enabled = true;
    for (auto& target : s_targets) target.retry_after = 0;
    Dirty();
    Notify(L"Pillar colour: " + Hex(rgb) + L" (visual only)");
}
void OpenPillarColourPicker() {
    if (s_picker_open.exchange(true, std::memory_order_acq_rel)) {
        Notify(L"Pillar colour picker already open");
        return;
    }
    s_picker_initial.store(Settings().rgb, std::memory_order_relaxed);
    s_picker_result.store(kNoPick, std::memory_order_release);
    HANDLE thread = CreateThread(nullptr, 0, PickerThread, nullptr, 0, nullptr);
    if (!thread) {
        s_picker_open.store(false, std::memory_order_release);
        Notify(L"Could not open pillar colour picker");
    } else CloseHandle(thread);
}
void UpdatePillarColours(MyCharacter* player) {
    if (s_save_dirty && GetTickCount64() >= s_save_due) {
        s_save_dirty = false;
        SaveNow();
    }
    const uint32_t result = s_picker_result.exchange(kNoPick, std::memory_order_acq_rel);
    if (result <= 0xFFFFFFu) SetPillarColourHex(result);
    if (!player || g_restart_requested || g_full_level_reload_requested) return;
    const bool enabled = Settings().enabled;
    const bool removed = Settings().removed;
    const bool visible = Settings().visible;
    const bool need_discovery = enabled || removed || !visible;
    if (!need_discovery && s_targets.empty()) return; // No idle global UObject scans.
    if (s_player != player) {
        // New player/world: the old UObject indices and material instances
        // must not be reused. A new discovery is valid for the new world.
        s_player = player;
        s_targets.clear();
        s_scan_cursor = need_discovery ? 0 : -1;
        s_discovery_complete = false;
        s_discovery_logged = false;
        s_apply_cursor = 0;
        s_pending = 0;
        s_next_update = 0;
    }
    if (!need_discovery) {
        // Restore any previous appearance changes, but retain discovered
        // targets so toggling settings ON does not scan the registry again.
        const bool pending_restore = std::any_of(s_targets.begin(), s_targets.end(),
            [](const Target& t) {
                return t.mid != nullptr || t.removal_saved || t.visual_saved;
            });
        if (!pending_restore) return;
    }
    const ULONGLONG now = GetTickCount64();
    // Skip the registry read between scheduled sweeps. Do not defer pending
    // colour/restore work, a new world or an incomplete discovery.
    if (s_pending == 0 && now < s_next_update &&
        (!need_discovery || (s_discovery_complete && s_scan_cursor < 0))) return;
    const auto* objects = Objects();
    if (!objects) return;
    // A completed discovery stays complete until the world changes or an
    // indexed UObject is invalidated. No time-based rescans.
    if (need_discovery && !s_discovery_complete && s_scan_cursor < 0) {
        s_scan_cursor = 0;
        s_discovery_logged = false;
    }
    if (need_discovery && s_scan_cursor >= 0) Discover(objects);
    if (!need_discovery) s_scan_cursor = -1;
    if (s_targets.empty() || (now < s_next_update && !s_pending)) return;
    s_next_update = now + (s_pending ? 0 : 100);
    ColourWorkBudget budget(850);
    size_t handled = 0;
    const size_t limit = (std::min)(kApplyBatch, s_targets.size());
    while (handled < limit && !s_targets.empty()) {
        if (handled > 0 && budget.expired()) break;
        ++handled;
        if (s_apply_cursor >= s_targets.size()) s_apply_cursor = 0;
        Target& target = s_targets[s_apply_cursor++];
        if (s_pending) --s_pending;
        // Discovery already checked class, ownership and the correct native
        // pillar mesh. Registry index + serial guard proves it remains the
        // same UObject: avoid repeating expensive owner-name conversions.
        if (!Valid(target, objects)) {
            target = {};
            // The cached UObject was destroyed/replaced. Rediscovery is
            // allowed on the next tick, but never just because time elapsed.
            s_discovery_complete = false;
            continue;
        }
        if (enabled || target.mid)
            Apply(target, objects, enabled, Settings().rgb, now);
        if (removed || target.removal_saved)
            ApplyRemoval(target, objects, removed, now);
        if (!visible || target.visual_saved)
            ApplyVisibility(target, visible, removed, now);
    }
    s_targets.erase(std::remove_if(s_targets.begin(), s_targets.end(),
        [](const Target& t) { return !t.mesh; }), s_targets.end());
    if (s_apply_cursor >= s_targets.size()) s_apply_cursor = 0;
    // Keep the cache even when pillar customisation is fully OFF. This avoids
    // repeating name lookups and a full UObject scan the next time it is ON.
}
} // namespace refunct

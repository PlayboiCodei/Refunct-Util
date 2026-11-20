#include "refunct/internal_api.hpp"
#include "refunct/features/tubes.hpp"
#include "refunct/colour_work_budget.hpp"
#include "refunct/discovery_watermark.hpp"
#include <commdlg.h>
#include <cwctype>
#include <limits>
#include <unordered_set>
#pragma comment(lib, "Comdlg32.lib")

namespace refunct {
namespace {
// Unlike tiles and buttons, the shipped tube actor/asset names are not yet
// independently verified. Discover only live StaticMeshComponents with an
// explicit pipe/tube identifier in their actor, component, or mesh asset.
// Never recolour arbitrary scene meshes or modify shared game materials.
struct TubeSettings {
    bool loaded = false;
    bool enabled = false;
    uint32_t rgb = 0x4CD9FF;
};
TubeSettings s_setting;
ULONGLONG s_tube_save_due = 0;
bool s_tube_save_pending = false;
struct TubeMesh {
    void* component = nullptr;
    int32_t index = -1;
    int32_t serial = 0;
    void* source = nullptr;
    void* no_tint_material = nullptr; // Avoid repeatedly probing materials with no tint parameter.
    uint32_t no_tint_revision = 0; // Recheck when another game MIC reveals a valid parent tint.
    void* mid = nullptr;
    FNameView parameter{};
    uint32_t last_rgb = 0;
    uint8_t restore_failures = 0;
};
constexpr size_t kMaxTubes = 512;
std::vector<TubeMesh> s_tubes;
MyCharacter* s_player = nullptr;
int32_t s_scan_cursor = 0;
ULONGLONG s_next_scan = 0;
DiscoveryWatermark s_discovery_watermark{};
// Colour discovery and reflected material queries must not run at render FPS.
ULONGLONG s_next_colour_work = 0;
size_t s_apply_cursor = 0;
size_t s_pending_apply = 0; // An edit processes every cached tube without a slow polling loop.
void QueueTubeColourRefresh() {
    s_apply_cursor = 0;
    s_pending_apply = s_tubes.size();
    s_next_colour_work = 0;
}
bool s_scan_warning = false;
bool s_material_warning = false;
bool s_restore_pending = false;

// Cooked UMaterial assets do not have the VectorParameterValues array used by
// MaterialInstanceConstant assets. Observe the existing game MICs instead:
// a parameter override in an MIC sharing a tube material's parent provides
// evidence that its compiled shader accepts that parameter, without guessing.
struct TintEvidence {
    FNameView name{};
    bool verified = false;
    bool ambiguous = false;
};
std::unordered_map<void*, TintEvidence> s_parent_tint;
uint32_t s_parent_tint_revision = 1;
// Verified against the user's unencrypted Refunct-WindowsNoEditor.pak:
// /Game/Materials/Abstract/M_Basic and M_Basic_TwoSided both contain a
// MaterialExpressionVectorParameter whose name table includes "Color".
// A base UMaterial does not expose MaterialInstance::VectorParameterValues;
// acquire the engine's real FName for that cooked parameter before using it.
FNameView s_cooked_color_parameter{};
bool s_cooked_color_parameter_ready = false;

// The shipped BP_MyCharacter contains a TravelMover TimelineComponent and
// calls SetPlayRate for pipe travel. The native value can vary by tube; it must
// be observed and multiplied rather than replaced with a fixed play rate.
struct TubeTravelSettings {
    bool loaded = false;
    int percent = 100;
};
TubeTravelSettings s_travel_setting{};
struct TravelRateState {
    MyCharacter* player = nullptr;
    void* timeline = nullptr;
    void* get_rate = nullptr;
    void* set_rate = nullptr;
    int32_t get_return_offset = -1;
    int32_t set_input_offset = -1;
    float native_rate = 1.0f;
    float last_written = 0.0f;
    bool modified = false;
    bool warning_logged = false;
    bool applied_logged = false;
    ULONGLONG next_check = 0;
};
TravelRateState s_travel{};

TubeTravelSettings& TravelSettings() {
    if (s_travel_setting.loaded || g_settings_path.empty()) return s_travel_setting;
    const int read = GetPrivateProfileIntW(L"TubeTravel", L"SpeedPercent",
                                            100, g_settings_path.c_str());
    // There is no fixed upper speed percentage. Reject invalid or negative
    // saved values; the UI uses 25% increments and int for persistence.
    s_travel_setting.percent = read >= 25 && read % 25 == 0
        ? read : 100;
    s_travel_setting.loaded = true;
    return s_travel_setting;
}
void SaveTravelSettings() {
    if (g_settings_path.empty()) return;
    if (!QueueSettingsValue(L"TubeTravel", L"SpeedPercent",
            std::to_wstring(TravelSettings().percent).c_str()))
        Log(L"Tube speed: could not save TubeTravel settings");
}

int32_t Param(void* function, const wchar_t* name, const wchar_t* kind, int32_t length);

bool IsTimelineComponent(void* object, MyCharacter* owner) {
    if (!IsReadable(object, sizeof(UObjectHeader)) ||
        (static_cast<uint32_t>(static_cast<const UObjectHeader*>(object)->flags) & 0x30u) ||
        static_cast<const UObjectHeader*>(object)->outer != owner) return false;
    void* cls = static_cast<const UObjectHeader*>(object)->class_ptr;
    for (int i = 0; i < 12 && IsReadable(cls, sizeof(UStructHeader)); ++i) {
        if (UnrealName(static_cast<const UObjectHeader*>(cls)->name) ==
                L"TimelineComponent") return true;
        cls = static_cast<const UStructHeader*>(cls)->super_struct;
    }
    return false;
}

void WarnTravelUnavailable(const wchar_t* detail) {
    if (s_travel.warning_logged) return;
    s_travel.warning_logged = true;
    Log((std::wstring(L"Tube speed: ") + detail).c_str());
    Notify(L"Tube speed unavailable (see RefunctUtility.log)");
}

bool BindTravelTimeline(MyCharacter* player) {
    if (!player || !IsReadable(player, sizeof(UObjectHeader))) return false;
    void* timeline = ReadReflectedObject(player, L"TravelMover");
    if (!IsTimelineComponent(timeline, player)) {
        WarnTravelUnavailable(L"BP_MyCharacter.TravelMover was not found or validated");
        return false;
    }
    if (s_travel.timeline == timeline && s_travel.get_rate && s_travel.set_rate &&
        s_travel.get_return_offset >= 0 && s_travel.set_input_offset >= 0)
        return true;
    // Only the specifically named character travel timeline may be changed;
    // never enumerate and modify generic engine timeline components.
    s_travel.timeline = timeline;
    s_travel.get_rate = FindReflectedFunction(timeline, L"GetPlayRate");
    s_travel.set_rate = FindReflectedFunction(timeline, L"SetPlayRate");
    s_travel.get_return_offset = Param(s_travel.get_rate, L"ReturnValue", L"FloatProperty", 96);
    s_travel.set_input_offset = Param(s_travel.set_rate, L"NewRate", L"FloatProperty", 96);
    if (s_travel.get_return_offset < 0 || s_travel.set_input_offset < 0 ||
        !g_process_event) {
        WarnTravelUnavailable(L"TravelMover GetPlayRate/SetPlayRate signature unavailable");
        return false;
    }
    s_travel.modified = false;
    Log(L"Tube speed: bound BP_MyCharacter.TravelMover timeline");
    return true;
}

bool ReadTravelRate(float& value) {
    if (!s_travel.timeline || !s_travel.get_rate || !g_process_event) return false;
    alignas(16) std::array<uint8_t, 96> args{};
    g_process_event(s_travel.timeline, s_travel.get_rate, args.data());
    std::memcpy(&value, args.data() + s_travel.get_return_offset, sizeof(value));
    return std::isfinite(value) && value >= 0.01f && value <= 1000.0f;
}

void WriteTravelRate(float value) {
    alignas(16) std::array<uint8_t, 96> args{};
    std::memcpy(args.data() + s_travel.set_input_offset, &value, sizeof(value));
    g_process_event(s_travel.timeline, s_travel.set_rate, args.data());
}

void SetTravelPercent(int percent) {
    auto& setting = TravelSettings();
    if (percent == setting.percent) return;
    setting.percent = percent;
    // Changing physics during a run taints it even if reset later.
    g_recording_tainted = true;
    if (!g_pb_cheats_allowed_for_run) g_run_pb_eligible = false;
    g_practice_gold_anchor_button = 0;
    g_practice_gold_anchor_seconds = -1.0;
    SaveTravelSettings();
    Notify(L"Tube travel speed: " + std::to_wstring(percent) + L"%" +
           (percent == 100 ? L" (restart to restore record eligibility)"
                           : L" (assisted run)"));
    s_travel.next_check = 0; // Apply/restore immediately on next game tick.
    s_travel.applied_logged = false;
}


constexpr uint32_t kNoPickerResult = 0xFFFFFFFFu;
constexpr uint32_t kPickerFailed = 0x01000000u;
constexpr uint32_t kPickerCancelled = 0x01000001u;
std::atomic<uint32_t> s_picker_result{kNoPickerResult};
std::atomic<uint32_t> s_picker_initial{0};
std::atomic<bool> s_picker_open{false};

std::wstring Hex(uint32_t rgb) {
    wchar_t buffer[8]{};
    swprintf_s(buffer, L"#%06X", rgb & 0xFFFFFFu);
    return buffer;
}
bool ParseTubeHex(const wchar_t* input, uint32_t& result) {
    if (!input) return false;
    const wchar_t* digits = input[0] == L'#' ? input + 1 : input;
    if (wcslen(digits) != 6) return false;
    uint32_t parsed = 0;
    for (int i = 0; i < 6; ++i) {
        const wchar_t c = digits[i];
        const int n = c >= L'0' && c <= L'9' ? c - L'0' :
            c >= L'A' && c <= L'F' ? c - L'A' + 10 :
            c >= L'a' && c <= L'f' ? c - L'a' + 10 : -1;
        if (n < 0) return false;
        parsed = (parsed << 4) | static_cast<uint32_t>(n);
    }
    result = parsed;
    return true;
}
TubeSettings& Settings() {
    if (s_setting.loaded || g_settings_path.empty()) return s_setting;
    wchar_t hex[32]{};
    GetPrivateProfileStringW(L"TubeColours", L"Hex", L"#4CD9FF", hex,
                             static_cast<DWORD>(std::size(hex)), g_settings_path.c_str());
    uint32_t rgb = 0;
    if (ParseTubeHex(hex, rgb)) s_setting.rgb = rgb;
    s_setting.enabled = GetPrivateProfileIntW(L"TubeColours", L"CustomEnabled",
                                              0, g_settings_path.c_str()) != 0;
    s_setting.loaded = true;
    return s_setting;
}
void Save() {
    if (g_settings_path.empty()) return;
    const auto& setting = Settings();
    const bool a = QueueSettingsValue(L"TubeColours", L"Hex",
        Hex(setting.rgb).c_str()) != 0;
    const bool b = QueueSettingsValue(L"TubeColours", L"CustomEnabled",
        setting.enabled ? L"1" : L"0") != 0;
    if (!a || !b) Log(L"Tube colours: could not save TubeColours settings");
}

void QueueTubeSave() {
    s_tube_save_pending = true;
    s_tube_save_due = GetTickCount64() + 350;
}
void FlushTubeSave() {
    if (!s_tube_save_pending || GetTickCount64() < s_tube_save_due) return;
    s_tube_save_pending = false;
    Save();
}

DWORD WINAPI PickerThread(LPVOID) {
    COLORREF custom[16]{};
    CHOOSECOLORW choice{};
    choice.lStructSize = sizeof(choice);
    choice.lpCustColors = custom;
    const uint32_t rgb = s_picker_initial.load(std::memory_order_relaxed);
    choice.rgbResult = RGB((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
    choice.Flags = CC_FULLOPEN | CC_ANYCOLOR | CC_RGBINIT;
    if (ChooseColorW(&choice)) {
        const uint32_t chosen = (static_cast<uint32_t>(GetRValue(choice.rgbResult)) << 16) |
            (static_cast<uint32_t>(GetGValue(choice.rgbResult)) << 8) |
            static_cast<uint32_t>(GetBValue(choice.rgbResult));
        s_picker_result.store(chosen, std::memory_order_release);
    } else {
        s_picker_result.store(CommDlgExtendedError() ? kPickerFailed : kPickerCancelled,
                              std::memory_order_release);
    }
    s_picker_open.store(false, std::memory_order_release);
    return 0;
}

bool HasTerm(const std::wstring& text) {
    if (text.empty()) return false;
    std::wstring lower = text;
    for (auto& c : lower) c = static_cast<wchar_t>(towlower(c));
    return lower.find(L"tube") != std::wstring::npos ||
           lower.find(L"pipe") != std::wstring::npos;
}
std::wstring ClassName(void* object) {
    if (!IsReadable(object, sizeof(UObjectHeader))) return L"";
    const void* cls = static_cast<const UObjectHeader*>(object)->class_ptr;
    return IsReadable(cls, sizeof(UObjectHeader))
        ? UnrealName(static_cast<const UObjectHeader*>(cls)->name) : L"";
}
bool IsLiveStaticMeshComponent(void* component) {
    if (!IsReadable(component, sizeof(UObjectHeader))) return false;
    const auto* object = static_cast<const UObjectHeader*>(component);
    if (static_cast<uint32_t>(object->flags) & 0x30u) return false; // CDO / archetype
    void* cls = object->class_ptr;
    if (!IsReadable(cls, sizeof(UStructHeader))) return false;
    // Most global objects share a small set of classes. Cache inheritance
    // decisions instead of repeatedly resolving Unreal FNames each scan.
    static std::unordered_map<void*, bool> class_cache;
    if (const auto it = class_cache.find(cls); it != class_cache.end())
        return it->second;
    void* current = cls;
    bool is_mesh = false;
    for (int depth = 0; depth < 12 && IsReadable(current, sizeof(UStructHeader)); ++depth) {
        if (UnrealName(static_cast<const UObjectHeader*>(current)->name) == L"StaticMeshComponent") {
            is_mesh = true;
            break;
        }
        current = static_cast<const UStructHeader*>(current)->super_struct;
    }
    if (class_cache.size() < 4096) class_cache.emplace(cls, is_mesh);
    return is_mesh;
}
// Only instance components whose outer is an actual actor are candidates.
// CDOs, component archetypes and free-standing asset components are excluded.
bool TubeIdentity(void* component, std::wstring& diagnostic_name) {
    if (!IsLiveStaticMeshComponent(component)) return false;
    const auto* object = static_cast<const UObjectHeader*>(component);
    const auto* outer = static_cast<const UObjectHeader*>(object->outer);
    if (!IsReadable(outer, sizeof(UObjectHeader)) ||
        (static_cast<uint32_t>(outer->flags) & 0x30u)) return false;
    const auto owner_class = ClassName(const_cast<UObjectHeader*>(outer));
    const auto owner_name = UnrealName(outer->name);
    if (owner_name.find(L"Default__") != std::wstring::npos ||
        owner_class.empty()) return false;
    const bool actor_like = owner_class.find(L"Actor") != std::wstring::npos ||
        (owner_class.rfind(L"BP_", 0) == 0 && owner_class.size() > 4);
    if (!actor_like) return false;
    std::wstring asset_name;
    // Native property metadata belongs to the class, not to each actor.
    // Cache offsets so discovery cannot repeatedly traverse hundreds of
    // reflected fields on the game thread.
    static std::unordered_map<void*, int32_t> offset_cache;
    const auto it = offset_cache.find(object->class_ptr);
    const int32_t offset = it != offset_cache.end() ? it->second :
        offset_cache.emplace(object->class_ptr,
            FindClassPropertyOffset(object->class_ptr, L"StaticMesh")).first->second;
    if (offset >= 0 && offset < 0x800 &&
        IsReadable(static_cast<const uint8_t*>(component) + offset, sizeof(void*))) {
        void* asset = nullptr;
        std::memcpy(&asset, static_cast<const uint8_t*>(component) + offset, sizeof(asset));
        if (IsReadable(asset, sizeof(UObjectHeader)) && ClassName(asset) == L"StaticMesh")
            asset_name = UnrealName(static_cast<const UObjectHeader*>(asset)->name);
    }
    const std::wstring component_name = UnrealName(object->name);
    if (!HasTerm(owner_class) && !HasTerm(owner_name) &&
        !HasTerm(component_name) && !HasTerm(asset_name)) return false;
    diagnostic_name = owner_class + L"/" + component_name + L" [" + asset_name + L"]";
    return true;
}
const GlobalObjectArray* Objects() {
    if (!g_game_module) return nullptr;
    auto* objects = reinterpret_cast<const GlobalObjectArray*>(
        reinterpret_cast<uintptr_t>(g_game_module) + kGlobalObjectArrayRva);
    if (!IsReadable(objects, sizeof(GlobalObjectArray)) ||
        objects->num_elements < 0 || objects->num_elements > 1000000 ||
        objects->max_elements < objects->num_elements ||
        !objects->objects ||
        !IsReadable(objects->objects, static_cast<size_t>(objects->num_elements) *
                                     sizeof(GlobalObjectItem))) return nullptr;
    return objects;
}
bool ValidTarget(const TubeMesh& target, const GlobalObjectArray* objects) {
    return objects && target.index >= 0 && target.index < objects->num_elements &&
        objects->objects[target.index].object == target.component &&
        objects->objects[target.index].serial_number == target.serial &&
        IsReadable(target.component, sizeof(UObjectHeader));
}
int32_t Param(void* function, const wchar_t* name, const wchar_t* kind,
              int32_t length = 96) {
    if (!IsReadable(function, sizeof(UStructHeader))) return -1;
    void* field = static_cast<const UStructHeader*>(function)->children;
    for (int n = 0; n < 48 && IsReadable(field, sizeof(UFieldHeader)); ++n) {
        const auto* prop = static_cast<const UFieldHeader*>(field);
        if (UnrealName(prop->object.name) == name &&
            IsReadable(prop->object.class_ptr, sizeof(UObjectHeader)) &&
            UnrealName(static_cast<const UObjectHeader*>(prop->object.class_ptr)->name) == kind) {
            const int32_t offset = FindClassPropertyOffset(function, name);
            return offset >= 0 && offset + (kind[0] == L'N' ? 8 : 4) <= length
                ? offset : -1;
        }
        field = prop->next;
    }
    return -1;
}
struct TubeMaterialMethod {
    void* function = nullptr;
    int32_t first = -1;
    int32_t second = -1;
};
TubeMaterialMethod TubeMethod(void* object, bool setter) {
    if (!IsReadable(object, sizeof(UObjectHeader))) return {};
    void* cls = static_cast<const UObjectHeader*>(object)->class_ptr;
    if (!IsReadable(cls, sizeof(UObjectHeader))) return {};
    static std::unordered_map<void*, TubeMaterialMethod> readers;
    static std::unordered_map<void*, TubeMaterialMethod> writers;
    auto& cache = setter ? writers : readers;
    if (auto it = cache.find(cls); it != cache.end() &&
        IsReadable(it->second.function, sizeof(UStructHeader))) return it->second;
    TubeMaterialMethod result{};
    result.function = FindReflectedFunction(object, setter ? L"SetMaterial" : L"GetMaterial");
    result.first = Param(result.function, L"ElementIndex", L"IntProperty");
    result.second = Param(result.function,
                          setter ? L"Material" : L"ReturnValue", L"ObjectProperty");
    if (result.function && result.first >= 0 && result.second >= 0 &&
        cache.size() < 1024) cache[cls] = result;
    return result;
}
void* GetMaterial(void* mesh) {
    if (!g_process_event || !IsReadable(mesh, sizeof(UObjectHeader))) return nullptr;
    const auto call = TubeMethod(mesh, false);
    if (!call.function || call.first < 0 || call.second < 0) return nullptr;
    alignas(16) std::array<uint8_t, 96> args{};
    g_process_event(mesh, call.function, args.data());  // slot 0 only
    void* material = nullptr;
    std::memcpy(&material, args.data() + call.second, sizeof(material));
    return IsReadable(material, sizeof(UObjectHeader)) ? material : nullptr;
}
bool SetMaterial(void* mesh, void* material) {
    if (!IsReadable(mesh, sizeof(UObjectHeader)) ||
        !IsReadable(material, sizeof(UObjectHeader)) || !g_process_event) return false;
    const auto call = TubeMethod(mesh, true);
    if (!call.function || call.first < 0 || call.second < 0) return false;
    alignas(16) std::array<uint8_t, 96> args{};
    std::memcpy(args.data() + call.second, &material, sizeof(material));
    g_process_event(mesh, call.function, args.data());
    return GetMaterial(mesh) == material;
}
// Stock MICs commonly expose one vector parameter. Never guess a name or
// mutate a shared instance. If the tube material has no tint vector, leave it
// unmodified and emit an explicit diagnostic for the next iteration.
bool TintParameter(void* material, FNameView& parameter) {
    for (int depth = 0; depth < 5 && IsReadable(material, sizeof(UObjectHeader)); ++depth) {
        void* cls = static_cast<const UObjectHeader*>(material)->class_ptr;
        const int32_t offset = FindClassPropertyOffset(cls, L"VectorParameterValues");
        if (offset >= 0 && offset < 0x400 &&
            IsReadable(static_cast<const uint8_t*>(material) + offset,
                       sizeof(UObjectPointerArray))) {
            UObjectPointerArray array{};
            std::memcpy(&array, static_cast<const uint8_t*>(material) + offset,
                        sizeof(array));
            if (array.count > 1 || array.count < 0) return false;
            if (array.count == 1) {
                if (!IsReadable(array.data, sizeof(parameter))) return false;
                std::memcpy(&parameter, array.data, sizeof(parameter));
                const auto name = UnrealName(parameter);
                return !name.empty() && name != L"None" && name.size() < 100;
            }
        }
        const int32_t parent_offset = FindClassPropertyOffset(cls, L"Parent");
        if (parent_offset < 0 || parent_offset > 0x400 ||
            !IsReadable(static_cast<const uint8_t*>(material) + parent_offset,
                        sizeof(void*))) return false;
        void* parent = nullptr;
        std::memcpy(&parent, static_cast<const uint8_t*>(material) + parent_offset,
                    sizeof(parent));
        if (!parent || parent == material) return false;
        material = parent;
    }
    return false;
}
// Resolve the real engine FName through a reflected Blueprint utility; do not
// invent an FName index from a cooked package's local name-map indices.
// This is attempted only when the known library CDO is encountered in the
// existing bounded global-object discovery pass.
void ObserveNameConversionLibrary(void* object) {
    if (s_cooked_color_parameter_ready || !g_process_event ||
        !IsReadable(object, sizeof(UObjectHeader))) return;
    const auto* header = static_cast<const UObjectHeader*>(object);
    // Only class-default objects can be the static library. This avoids
    // resolving an FName for every live object during discovery.
    if ((static_cast<uint32_t>(header->flags) & 0x10u) == 0 ||
        UnrealName(header->name) != L"Default__KismetStringLibrary" ||
        ClassName(object) != L"KismetStringLibrary") return;
    void* function = FindReflectedFunction(object, L"Conv_StringToName");
    const int32_t input = Param(function, L"InString", L"StrProperty");
    const int32_t output = Param(function, L"ReturnValue", L"NameProperty");
    if (input < 0 || output < 0 ||
        input + static_cast<int32_t>(sizeof(FStringView)) > 96 ||
        output + static_cast<int32_t>(sizeof(FNameView)) > 96) return;
    wchar_t colour[] = L"Color";
    const FStringView text{colour, 6, 6};
    alignas(16) std::array<uint8_t, 96> args{};
    std::memcpy(args.data() + input, &text, sizeof(text));
    g_process_event(object, function, args.data());
    FNameView result{};
    std::memcpy(&result, args.data() + output, sizeof(result));
    if (UnrealName(result) != L"Color") return;
    s_cooked_color_parameter = result;
    s_cooked_color_parameter_ready = true;
    ++s_parent_tint_revision;
    Log(L"Tube colours: resolved cooked M_Basic colour parameter 'Color' via KismetStringLibrary");
}

// Only the exact base materials identified from Refunct's PAK are eligible.
// This deliberately excludes materials on unrelated meshes even if they also
// have a parameter named Color.
bool KnownCookedTubeTint(void* source, FNameView& parameter) {
    if (!s_cooked_color_parameter_ready ||
        !IsReadable(source, sizeof(UObjectHeader)) ||
        ClassName(source) != L"Material") return false;
    const auto name = UnrealName(static_cast<const UObjectHeader*>(source)->name);
    if (name != L"M_Basic" && name != L"M_Basic_TwoSided") return false;
    parameter = s_cooked_color_parameter;
    return true;
}

// A base UMaterial in a cooked shipping game generally cannot enumerate its
// editor-only vector expressions. But a material instance derived from it has
// its runtime parameter overrides intact. We collect real override names from
// loaded game MICs and use a name only if all observed candidates agree.
// Never modify the evidence MIC or any globally shared UMaterial.
void ObserveExistingMaterialInstance(void* object) {
    if (!IsReadable(object, sizeof(UObjectHeader))) return;
    const auto* header = static_cast<const UObjectHeader*>(object);
    if (!IsReadable(header->class_ptr, sizeof(UObjectHeader))) return;
    static std::unordered_map<void*, bool> material_class_cache;
    bool eligible = false;
    if (const auto it = material_class_cache.find(header->class_ptr);
        it != material_class_cache.end()) {
        eligible = it->second;
    } else {
        eligible = ClassName(object) == L"MaterialInstanceConstant";
        if (material_class_cache.size() < 4096)
            material_class_cache.emplace(header->class_ptr, eligible);
    }
    if (!eligible) return;
    FNameView parameter{};
    if (!TintParameter(object, parameter)) return;
    // A loaded instance with a real Color override is an additional safe
    // source of the globally indexed FName used by the cooked base materials.
    if (!s_cooked_color_parameter_ready && UnrealName(parameter) == L"Color") {
        s_cooked_color_parameter = parameter;
        s_cooked_color_parameter_ready = true;
        ++s_parent_tint_revision;
        Log(L"Tube colours: resolved cooked M_Basic colour parameter 'Color' from game MIC");
    }
    // Follow only verified native Parent pointers. A single override may be
    // inherited through another MIC before reaching M_Basic/M_Basic_TwoSided.
    void* current = object;
    for (int depth = 0; depth < 6; ++depth) {
        if (!IsReadable(current, sizeof(UObjectHeader))) break;
        void* cls = static_cast<const UObjectHeader*>(current)->class_ptr;
        if (!IsReadable(cls, sizeof(UStructHeader))) break;
        static std::unordered_map<void*, int32_t> parent_offset_cache;
        const auto it = parent_offset_cache.find(cls);
        const int32_t offset = it == parent_offset_cache.end()
            ? parent_offset_cache.emplace(cls, FindClassPropertyOffset(cls, L"Parent")).first->second
            : it->second;
        if (offset < 0 || offset > 0x400 ||
            !IsReadable(static_cast<const uint8_t*>(current) + offset, sizeof(void*))) break;
        void* parent = nullptr;
        std::memcpy(&parent, static_cast<const uint8_t*>(current) + offset, sizeof(parent));
        if (!parent || parent == current || !IsReadable(parent, sizeof(UObjectHeader))) break;
        if (s_parent_tint.size() >= 4096 &&
            s_parent_tint.find(parent) == s_parent_tint.end()) break;
        auto& evidence = s_parent_tint[parent];
        if (!evidence.verified) {
            evidence.name = parameter;
            evidence.verified = true;
            ++s_parent_tint_revision;
        } else if (evidence.name.index != parameter.index ||
                   evidence.name.number != parameter.number) {
            if (!evidence.ambiguous) {
                evidence.ambiguous = true; // Different overrides: do not guess.
                ++s_parent_tint_revision;
            }
        }
        current = parent;
    }
}

bool InferredParentTint(void* source, FNameView& parameter) {
    const auto it = s_parent_tint.find(source);
    if (it == s_parent_tint.end() || !it->second.verified || it->second.ambiguous)
        return false;
    parameter = it->second.name;
    return true;
}

void* CreateMID(void* mesh, void* source) {
    if (!source || !g_process_event) return nullptr;
    struct Method { const wchar_t* name; const wchar_t* parent; bool needs_name; };
    constexpr Method methods[] = {
        {L"CreateDynamicMaterialInstance", L"SourceMaterial", true},
        {L"CreateAndSetMaterialInstanceDynamicFromMaterial", L"Parent", false},
        {L"CreateAndSetMaterialInstanceDynamic", nullptr, false}
    };
    for (const auto& method : methods) {
        void* fn = FindReflectedFunction(mesh, method.name);
        const int32_t index = Param(fn, L"ElementIndex", L"IntProperty");
        const int32_t returned = Param(fn, L"ReturnValue", L"ObjectProperty");
        const int32_t parent = method.parent
            ? Param(fn, method.parent, L"ObjectProperty") : -1;
        const int32_t name = method.needs_name
            ? Param(fn, L"OptionalName", L"NameProperty") : -1;
        if (index < 0 || returned < 0 || (method.parent && parent < 0) ||
            (method.needs_name && name < 0)) continue;
        if (!method.parent && GetMaterial(mesh) != source &&
            !SetMaterial(mesh, source)) break;
        alignas(16) std::array<uint8_t, 96> args{};
        if (parent >= 0) std::memcpy(args.data() + parent, &source, sizeof(source));
        g_process_event(mesh, fn, args.data());
        void* mid = nullptr;
        std::memcpy(&mid, args.data() + returned, sizeof(mid));
        if (IsReadable(mid, sizeof(UObjectHeader)) && GetMaterial(mesh) == mid &&
            ClassName(mid).find(L"MaterialInstanceDynamic") != std::wstring::npos)
            return mid;
    }
    if (GetMaterial(mesh) != source) SetMaterial(mesh, source);
    return nullptr;
}
bool SetTint(void* mid, FNameView parameter, uint32_t rgb) {
    if (!IsReadable(mid, sizeof(UObjectHeader))) return false;
    const void* cls = static_cast<const UObjectHeader*>(mid)->class_ptr;
    static std::unordered_map<const void*, TubeMaterialMethod> methods;
    TubeMaterialMethod call{};
    if (const auto it = methods.find(cls); it != methods.end() &&
        IsReadable(it->second.function, sizeof(UStructHeader))) call = it->second;
    else {
        call.function = FindReflectedFunction(mid, L"SetVectorParameterValue");
        if (!call.function) call.function = FindReflectedFunction(mid, L"K2_SetVectorParameterValue");
        call.first = Param(call.function, L"ParameterName", L"NameProperty");
        call.second = Param(call.function, L"Value", L"StructProperty");
        if (call.first >= 0 && call.second >= 0 && methods.size() < 1024)
            methods[cls] = call;
    }
    if (!call.function || call.first < 0 || call.second < 0 ||
        call.second + static_cast<int32_t>(sizeof(LinearColor)) > 96)
        return false;
    auto linear = [](uint32_t channel) {
        const float x = static_cast<float>(channel) / 255.0f;
        return x <= 0.04045f ? x / 12.92f : std::pow((x + 0.055f) / 1.055f, 2.4f);
    };
    const LinearColor colour{linear((rgb >> 16) & 255), linear((rgb >> 8) & 255),
                             linear(rgb & 255), 1.0f};
    alignas(16) std::array<uint8_t, 96> args{};
    std::memcpy(args.data() + call.first, &parameter, sizeof(parameter));
    std::memcpy(args.data() + call.second, &colour, sizeof(colour));
    g_process_event(mid, call.function, args.data());
    return true;
}
void WarnMaterial(const wchar_t* message) {
    if (s_material_warning) return;
    s_material_warning = true;
    Log((std::wstring(L"Tube colours: ") + message).c_str());
    Notify(std::wstring(L"Tube colour unavailable: ") + message +
           L" (see RefunctUtility.log)");
}
void Discover(const GlobalObjectArray* objects) {
    if (!objects || s_scan_cursor < 0) return;
    constexpr int kPerTick = 2048;
    const int32_t last = (std::min)(objects->num_elements, s_scan_cursor + kPerTick);
    ColourWorkBudget scan_budget(800);
    int examined = 0;
    for (; s_scan_cursor < last; ++s_scan_cursor) {
        if ((examined & 31) == 0 && scan_budget.expired()) break;
        ++examined;
        const auto& item = objects->objects[s_scan_cursor];
        if (!item.object || (static_cast<uint32_t>(item.flags) & 0x30000000u) ||
            !IsReadable(item.object, sizeof(UObjectHeader))) continue;
        // This is the same bounded discovery pass as the tube search; it
        // introduces no separate full-object sweep or per-frame scan.
        ObserveExistingMaterialInstance(item.object);
        // Reuses the same bounded discovery scan; no new per-frame full scan.
        // Most engine builds load the library CDO even when no relevant MIC is loaded.
        if (!s_cooked_color_parameter_ready)
            ObserveNameConversionLibrary(item.object);
        if (s_tubes.size() >= kMaxTubes) continue;
        std::wstring name;
        if (!TubeIdentity(item.object, name)) continue;
        const auto already = std::find_if(s_tubes.begin(), s_tubes.end(),
            [&](const TubeMesh& entry) { return entry.component == item.object; });
        if (already != s_tubes.end()) continue;
        TubeMesh entry{};
        entry.component = item.object;
        entry.index = s_scan_cursor;
        entry.serial = item.serial_number;
        if (s_pending_apply == 0) s_apply_cursor = s_tubes.size();
        s_tubes.push_back(entry);
        ++s_pending_apply;
        // Don't synchronously log every newly discovered component from the
        // game tick: even a short burst of file writes can hitch a frame.
    }
    if (s_scan_cursor >= objects->num_elements || s_tubes.size() >= kMaxTubes) {
        s_scan_cursor = -1;
        s_discovery_watermark.Complete(objects->objects, objects->num_elements, GetTickCount64(),
            s_tubes.empty() ? 5000 : 43000, s_next_scan);
        if (!s_scan_warning && !s_tubes.empty()) {
            s_scan_warning = true;
            Log((L"Tube colours: discovered " + std::to_wstring(s_tubes.size()) +
                 L" live pipe/tube components").c_str());
        }
        if (s_tubes.empty() && !s_scan_warning) {
            s_scan_warning = true;
            Log(L"Tube colours: no live tube/pipe StaticMeshComponent identified; "
                L"need actor/mesh names from this Refunct version");
            Notify(L"Tube meshes not identified (see RefunctUtility.log)");
        }
    }
}
void ApplyOne(TubeMesh& target, const GlobalObjectArray* objects, bool custom,
              uint32_t rgb) {
    if (!ValidTarget(target, objects)) { target = {}; s_next_scan = 0; return; }
    void* displayed = GetMaterial(target.component);
    if (!displayed) return;
    const bool ours = target.mid && displayed == target.mid && target.source;
    if (!custom) {
        if (ours && !SetMaterial(target.component, target.source)) {
            // A stale native component must not keep causing unbounded writes.
            // Retry a few ticks, then leave the game-owned material untouched.
            if (++target.restore_failures < 3) return;
            WarnMaterial(L"original tube material could not be restored");
        }
        target.mid = nullptr;
        target.source = nullptr;
        target.restore_failures = 0;
        return;
    }
    if (ours && target.last_rgb == rgb) return;
    void* source = ours ? target.source : displayed;
    // Source materials with no tint controls are common and immutable through
    // SetVectorParameterValue. Rechecking their reflection metadata 20 times
    // per second costs CPU without changing appearance.
    if (target.no_tint_material == source &&
        target.no_tint_revision == s_parent_tint_revision) return;
    FNameView parameter{};
    bool inferred = false;
    bool cooked = false;
    if (!TintParameter(source, parameter)) {
        inferred = InferredParentTint(source, parameter);
        // In the shipped PAK these exact UMaterial assets have a Color
        // vector expression, even though they have no MIC override array.
        if (!inferred) cooked = KnownCookedTubeTint(source, parameter);
        if (!inferred && !cooked) {
            // Discovery is still looking for matching MICs. Don't announce
            // failure while we may find a legitimate parameter later.
            if (s_scan_cursor >= 0) return;
            target.no_tint_material = source;
            target.no_tint_revision = s_parent_tint_revision;
            static std::unordered_set<std::wstring> logged_missing_sources;
            const std::wstring source_name = UnrealName(
                static_cast<const UObjectHeader*>(source)->name);
            if (logged_missing_sources.insert(source_name).second)
                Log((L"Tube colours: no usable parameter found for " + source_name).c_str());
            const auto evidence_it = s_parent_tint.find(source);
            const bool conflicting = evidence_it != s_parent_tint.end() &&
                                     evidence_it->second.ambiguous;
            WarnMaterial(conflicting ? L"ambiguous colour parameters for base material"
                                     : L"no compatible colour parameter found in loaded game materials");
            return;
        }
    }
    if (cooked) {
        static std::unordered_set<std::wstring> logged_cooked_sources;
        const auto material_name = UnrealName(
            static_cast<const UObjectHeader*>(source)->name);
        if (logged_cooked_sources.insert(material_name).second)
            Log((L"Tube colours: applying cooked parameter 'Color' to " +
                 material_name + L" through a separate dynamic material instance").c_str());
    }
    if (inferred) {
        static std::unordered_set<std::wstring> logged_tint_sources;
        const std::wstring material_name = UnrealName(
            static_cast<const UObjectHeader*>(source)->name);
        if (logged_tint_sources.insert(material_name).second)
            Log((L"Tube colours: using verified parameter '" + UnrealName(parameter) +
                 L"' from game material instances sharing parent " + material_name).c_str());
    }
    target.no_tint_material = nullptr;
    void* mid = ours ? target.mid : CreateMID(target.component, source);
    if (!mid || !SetTint(mid, parameter, rgb)) {
        if (mid && !ours) SetMaterial(target.component, source);
        // Avoid repeated expensive failing reflected calls on every sweep.
        target.no_tint_material = source;
        target.no_tint_revision = s_parent_tint_revision;
        WarnMaterial(L"tube dynamic material creation/tint failed");
        return;
    }
    target.source = source;
    target.mid = mid;
    target.parameter = parameter;
    target.last_rgb = rgb;
    target.restore_failures = 0;
}
} // namespace

void SetTubeTravelSpeedProfilePercent(int percent) { if (percent >= 25) SetTravelPercent(percent); }
int GetTubeTravelSpeedPercent() { return TravelSettings().percent; }
bool IsTubeTravelSpeedModified() { return TravelSettings().percent != 100; }
void AdjustTubeTravelSpeed(int direction) {
    if (direction == 0) return;
    constexpr int kStep = 25;
    constexpr int kMinimum = 25;
    const int current = GetTubeTravelSpeedPercent();
    if (direction < 0) {
        if (current > kMinimum) SetTravelPercent(current - kStep);
        return;
    }
    // No arbitrary percentage cap. Stop before signed overflow, because
    // the value is stored as an int by Windows INI and displayed in the menu.
    if (current <= (std::numeric_limits<int>::max)() - kStep)
        SetTravelPercent(current + kStep);
}
void ResetTubeTravelSpeed() { SetTravelPercent(100); }

void UpdateTubeTravelSpeed(MyCharacter* player) {
    if (g_restart_requested || g_full_level_reload_requested || !player) return;
    if (s_travel.player != player) {
        // The previous UTimelineComponent may already have been destroyed.
        // Never restore native objects through a stale character pointer.
        s_travel = {};
        s_travel.player = player;
    }
    const int speed = TravelSettings().percent;
    if (speed == 100 && !s_travel.modified) return;
    const ULONGLONG now = GetTickCount64();
    if (now < s_travel.next_check) return;
    s_travel.next_check = now + 50; // <=20 reflected reads/writes a second.
    if (!BindTravelTimeline(player)) {
        // A missing reflected method must not cause expensive lookups at 20 Hz.
        s_travel.next_check = now + 1000;
        return;
    }

    float observed = 0.0f;
    if (!ReadTravelRate(observed)) {
        WarnTravelUnavailable(L"TravelMover returned an invalid playback rate");
        return;
    }
    // Refunct calls SetPlayRate itself when a pipe begins. If that happens,
    // the new value is its *new native rate*, not a drift in our multiplier.
    // This ensures different pipe lengths keep their own original travel speed.
    const float tolerance = std::max(0.001f, std::abs(s_travel.last_written) * 0.002f);
    const bool still_ours = s_travel.modified &&
                            std::abs(observed - s_travel.last_written) <= tolerance;
    if (!still_ours) {
        s_travel.native_rate = observed;
        s_travel.modified = false;
    }
    if (speed == 100) {
        if (still_ours) {
            WriteTravelRate(s_travel.native_rate);
            Log(L"Tube speed: restored native TravelMover playback rate");
        }
        s_travel.modified = false;
        return;
    }
    // The menu has no 400% ceiling, but native timeline rates must remain
    // finite and inside the existing engine safety range. Very large speed
    // requests saturate the physical playback rate rather than disabling it.
    const float requested = s_travel.native_rate * (static_cast<float>(speed) / 100.0f);
    if (!std::isfinite(requested) || requested < 0.01f) {
        WarnTravelUnavailable(L"calculated tube timeline playback rate invalid");
        return;
    }
    const float desired = std::min(requested, 1000.0f);
    if (still_ours && std::abs(observed - desired) <= tolerance) return;
    WriteTravelRate(desired);
    float applied = 0.0f;
    if (!ReadTravelRate(applied) || std::abs(applied - desired) >
            std::max(0.01f, desired * 0.002f)) {
        WarnTravelUnavailable(L"TravelMover playback rate write did not take effect");
        return;
    }
    s_travel.last_written = applied;
    s_travel.modified = true;
    if (!s_travel.applied_logged) {
        s_travel.applied_logged = true;
        Log((L"Tube speed: TravelMover rate " + std::to_wstring(s_travel.native_rate) +
             L" -> " + std::to_wstring(applied) + L" (" +
             std::to_wstring(speed) + L"%)").c_str());
    }
}

bool IsTubeCustomColourEnabled() { return Settings().enabled; }
void SetTubeCustomColourEnabled(bool enabled) {
    auto& setting = Settings();
    if (setting.enabled == enabled) return;
    setting.enabled = enabled;
    s_tube_save_pending = false;
    Save(); // A mode toggle is infrequent and should persist immediately.
    s_restore_pending = !enabled;
    QueueTubeColourRefresh();
    // The previous scan/known component list remains valid when the colour changes.
    if (enabled && s_tubes.empty() && s_scan_cursor < 0) {
        s_scan_cursor = 0; s_next_scan = 0; s_scan_warning = false;
    }
    Notify(enabled ? L"Tube custom colour ON: checking live pipe/tube materials"
                   : L"Tube custom colour OFF: restoring game-default materials");
}
std::wstring GetTubeCustomColourHex() { return Hex(Settings().rgb); }
void SetTubeCustomColourHex(uint32_t rgb) {
    if (rgb > 0xFFFFFFu) return;
    auto& setting = Settings();
    if (setting.rgb == rgb && setting.enabled) return;
    setting.rgb = rgb;
    setting.enabled = true;
    s_restore_pending = false;
    QueueTubeColourRefresh(); // No global UObject rescan for a known tube.
    if (s_tubes.empty() && s_scan_cursor < 0) s_scan_cursor = 0;
    QueueTubeSave(); // Collapse rapid HEX edits into one INI write.
    Notify(L"Tube custom colour: " + Hex(rgb));
}
void OpenTubeColourPicker() {
    if (s_picker_result.load(std::memory_order_acquire) != kNoPickerResult) {
        Notify(L"Previous tube picker result is still being applied");
        return;
    }
    if (s_picker_open.exchange(true, std::memory_order_acq_rel)) {
        Notify(L"Tube colour picker already open");
        return;
    }
    s_picker_initial.store(Settings().rgb, std::memory_order_release);
    HANDLE thread = CreateThread(nullptr, 0, PickerThread, nullptr, 0, nullptr);
    if (!thread) {
        s_picker_open.store(false, std::memory_order_release);
        Notify(L"Could not open tube colour picker");
        return;
    }
    CloseHandle(thread);
    Notify(L"Tube colour picker opened (Alt+Tab if hidden by fullscreen)");
}
void UpdateTubeColours(MyCharacter* player) {
    FlushTubeSave();
    const uint32_t chosen = s_picker_result.exchange(kNoPickerResult,
                                                     std::memory_order_acq_rel);
    if (chosen <= 0xFFFFFFu) SetTubeCustomColourHex(chosen);
    else if (chosen == kPickerFailed) Notify(L"Tube colour picker failed");
    const auto& setting = Settings();
    if (!setting.enabled && !s_restore_pending) return;
    if (g_restart_requested || g_full_level_reload_requested || !player) return;
    if (s_player != player) {
        // Old native actors may already have been released; never restore a
        // cached pointer across a character/world replacement.
        s_tubes.clear();
        s_player = player;
        s_scan_cursor = setting.enabled ? 0 : -1;
        s_apply_cursor = 0;
        s_pending_apply = 0;
        s_restore_pending = false;
        s_next_scan = 0;
        s_next_colour_work = 0;
        s_scan_warning = false;
        s_material_warning = false;
        s_parent_tint.clear();
        ++s_parent_tint_revision;
    }
    const ULONGLONG now = GetTickCount64();
    if (s_pending_apply == 0 && s_scan_cursor < 0 && now < s_next_colour_work) return;
    const auto* objects = Objects();
    if (!objects) return;
    if (setting.enabled) {
        if (s_scan_cursor < 0)
            s_scan_cursor = s_discovery_watermark.Resume(objects->objects, objects->num_elements, now, s_next_scan);
        // Discovery is no longer delayed by the 50 ms colour polling timer.
        // It gets a small time budget every game tick until the scan completes.
        if (s_scan_cursor >= 0) Discover(objects);
    }
    if (s_pending_apply == 0 && now < s_next_colour_work) return;
    // Material instance creation is an engine operation; never create many
    // instances in a single frame. Existing MIDs can be recoloured immediately.
    ColourWorkBudget apply_budget(1000);
    const size_t batch = s_pending_apply ? 5 : 4;
    for (size_t n = 0; n < batch && !s_tubes.empty(); ++n) {
        if (n > 0 && apply_budget.expired()) break;
        if (s_apply_cursor >= s_tubes.size()) s_apply_cursor = 0;
        auto& entry = s_tubes[s_apply_cursor++];
        ApplyOne(entry, objects, setting.enabled, setting.rgb);
        if (s_pending_apply) --s_pending_apply;
    }
    // Recycle invalid slots after iteration so stale entries do not consume the cap.
    if (s_next_scan == 0) {
        const auto old_size = s_tubes.size();
        s_tubes.erase(std::remove_if(s_tubes.begin(), s_tubes.end(),
            [](const TubeMesh& entry) { return !entry.component; }), s_tubes.end());
        if (s_tubes.size() != old_size) QueueTubeColourRefresh();
    }
    s_next_colour_work = GetTickCount64() + (s_pending_apply ? 0 : 650);
    // A failed Game Default restore must be retried, not silently discarded.
    if (!setting.enabled && s_restore_pending && s_pending_apply == 0) {
        const bool any_ours = std::any_of(s_tubes.begin(), s_tubes.end(),
            [](const TubeMesh& entry) { return entry.mid != nullptr; });
        if (any_ours) {
            s_pending_apply = s_tubes.size();
            s_apply_cursor = 0;
        } else {
            s_restore_pending = false;
            s_tubes.clear();
            s_apply_cursor = 0;
        }
    }
}
} // namespace refunct

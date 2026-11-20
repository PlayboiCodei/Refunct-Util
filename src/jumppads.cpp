#include "refunct/internal_api.hpp"
#include "refunct/colour_work_budget.hpp"
#include "refunct/discovery_watermark.hpp"
#include <unordered_map>
#include "refunct/features/jumppads.hpp"
#include <commdlg.h>
#include <limits>
#include <algorithm>

namespace refunct {
namespace {
// Refunct-WindowsNoEditor.pak contains BP_Jumppad_C, Mover/SM_Jumppad (top), and a slot-0
// M_Gold_Rough_Yellow MIC with a real "Color" vector parameter.
// Never touch ordinary jumping or the separate BP_Lift/BP_Mover actors.
constexpr size_t kMaximumTargets = 128;
constexpr int kScanBatch = 2048; // Scan incrementally every tick, under CPU budget.
constexpr int kApplyBatch = 8;
constexpr ULONGLONG kUpdateMs = 50;

struct Settings {
    bool loaded = false;
    bool custom_colour = false;
    uint32_t rgb = 0x4CD9FF;
    int height_percent = 100;
};
Settings s_settings{};

struct Target {
    void* actor = nullptr;
    int32_t index = -1;
    int32_t serial = 0;
    void* mesh = nullptr;
    void* source = nullptr;
    void* material_instance = nullptr;
    FNameView colour_parameter{};
    uint32_t applied_rgb = 0;
    bool colour_unsupported = false;
    ULONGLONG next_colour_check = 0;
    // Read-only world-space position, refreshed periodically. A cached pad
    // position lets the launch detector avoid reflection in the tick hot path.
    Vec3 world_position{};
    bool world_position_valid = false;
    ULONGLONG next_position_refresh = 0;
    ULONGLONG last_position_valid_at = 0;
};
std::vector<Target> s_targets{};
MyCharacter* s_player = nullptr;
int32_t s_scan_cursor = 0;
size_t s_apply_cursor = 0;
ULONGLONG s_next_scan = 0;
DiscoveryWatermark s_discovery_watermark{};
ULONGLONG s_next_update = 0;
ULONGLONG s_next_height_check = 0;
size_t s_pending_colour = 0; // Flush colour changes to cached actors without rescanning UObject array.
ULONGLONG s_save_due = 0;
bool s_save_dirty = false;
bool s_restore_pending = false;
bool s_discovery_reported = false;
bool s_colour_warning = false;
bool s_mesh_warning = false;

constexpr uint32_t kNoPickerResult = 0xFFFFFFFFu;
constexpr uint32_t kPickerCancelled = 0x01000000u;
std::atomic<uint32_t> s_picker_result{kNoPickerResult};
std::atomic<uint32_t> s_picker_initial{0};
std::atomic<bool> s_picker_open{false};

std::wstring JumpPadHex(uint32_t rgb) {
    wchar_t text[8]{};
    swprintf_s(text, L"#%06X", rgb & 0xFFFFFFu);
    return text;
}
uint32_t JumpPadParseHex(const wchar_t* text, uint32_t fallback) {
    if (!text) return fallback;
    if (*text == L'#') ++text;
    if (wcslen(text) != 6) return fallback;
    uint32_t value = 0;
    for (int i = 0; i < 6; ++i) {
        const wchar_t c = text[i];
        const int digit = c >= L'0' && c <= L'9' ? c - L'0' :
                          c >= L'A' && c <= L'F' ? c - L'A' + 10 :
                          c >= L'a' && c <= L'f' ? c - L'a' + 10 : -1;
        if (digit < 0) return fallback;
        value = (value << 4) | static_cast<uint32_t>(digit);
    }
    return value;
}
Settings& Config() {
    if (s_settings.loaded || g_settings_path.empty()) return s_settings;
    wchar_t hex[32]{};
    GetPrivateProfileStringW(L"JumpPadColours", L"Hex", L"#4CD9FF", hex,
                             static_cast<DWORD>(std::size(hex)), g_settings_path.c_str());
    s_settings.rgb = JumpPadParseHex(hex, s_settings.rgb);
    s_settings.custom_colour =
        GetPrivateProfileIntW(L"JumpPadColours", L"CustomEnabled", 0,
                              g_settings_path.c_str()) != 0;
    const int height = GetPrivateProfileIntW(L"JumpPadLaunch", L"HeightPercent", 100,
                                            g_settings_path.c_str());
    s_settings.height_percent = height >= 25 && height % 25 == 0 ? height : 100;
    s_settings.loaded = true;
    return s_settings;
}
void JumpPadSaveConfig() {
    if (g_settings_path.empty()) return;
    const Settings& settings = Config();
    bool ok = QueueSettingsValue(L"JumpPadColours", L"Hex",
                  JumpPadHex(settings.rgb).c_str()) != 0;
    ok &= QueueSettingsValue(L"JumpPadColours", L"CustomEnabled",
               settings.custom_colour ? L"1" : L"0") != 0;
    ok &= QueueSettingsValue(L"JumpPadLaunch", L"HeightPercent",
               std::to_wstring(settings.height_percent).c_str()) != 0;
    if (!ok) Log(L"Jump pads: could not write jump pad settings");
}
void FlushDeferredJumpPadSave() {
    if (!s_save_dirty || GetTickCount64() < s_save_due) return;
    s_save_dirty = false;
    JumpPadSaveConfig();
}
void JumpPadChanged(const std::wstring& message, bool affects_gameplay) {
    if (affects_gameplay) {
        g_recording_tainted = true;
        if (!g_pb_cheats_allowed_for_run) g_run_pb_eligible = false;
        g_practice_gold_anchor_button = 0;
        g_practice_gold_anchor_seconds = -1.0;
    }
    // A colour edit must not restart the slow global UObject discovery scan.
    if (affects_gameplay || (s_targets.empty() && s_scan_cursor < 0)) {
        s_scan_cursor = 0;
        s_next_scan = 0;
    }
    if (!affects_gameplay) {
        s_apply_cursor = 0;
        s_pending_colour = s_targets.size();
        s_save_dirty = true;
        s_save_due = GetTickCount64() + 350;
    } else {
        // Queue physics-affecting settings immediately (without colour-edit debounce).
        s_save_dirty = false;
        JumpPadSaveConfig();
    }
    s_next_update = 0;
    Notify(message + (affects_gameplay ? L" (assisted run)" : L" (visual only)"));
}

DWORD WINAPI JumpPadPickerThread(LPVOID) {
    COLORREF custom[16]{};
    CHOOSECOLORW choice{};
    choice.lStructSize = sizeof(choice);
    choice.lpCustColors = custom;
    const uint32_t colour = s_picker_initial.load(std::memory_order_relaxed);
    choice.rgbResult = RGB((colour >> 16) & 0xff, (colour >> 8) & 0xff, colour & 0xff);
    choice.Flags = CC_FULLOPEN | CC_ANYCOLOR | CC_RGBINIT;
    if (ChooseColorW(&choice)) {
        const uint32_t value = (static_cast<uint32_t>(GetRValue(choice.rgbResult)) << 16) |
                               (static_cast<uint32_t>(GetGValue(choice.rgbResult)) << 8) |
                               static_cast<uint32_t>(GetBValue(choice.rgbResult));
        s_picker_result.store(value, std::memory_order_release);
    } else {
        s_picker_result.store(kPickerCancelled, std::memory_order_release);
    }
    s_picker_open.store(false, std::memory_order_release);
    return 0;
}

std::wstring JumpPadObjectName(void* object) {
    return IsReadable(object, sizeof(UObjectHeader))
        ? UnrealName(static_cast<const UObjectHeader*>(object)->name) : L"";
}
std::wstring JumpPadClassName(void* object) {
    if (!IsReadable(object, sizeof(UObjectHeader))) return L"";
    void* cls = static_cast<const UObjectHeader*>(object)->class_ptr;
    return JumpPadObjectName(cls);
}
bool NamedJumpPad(void* object) {
    if (!IsReadable(object, sizeof(UObjectHeader))) return false;
    const auto* header = static_cast<const UObjectHeader*>(object);
    if (static_cast<uint32_t>(header->flags) & 0x30u ||
        !IsReadable(header->class_ptr, sizeof(UObjectHeader))) return false;
    // Nearly all global UObjects are NOT our target actor. Cache the class
    // decision before resolving each instance's name during discovery.
    static std::unordered_map<void*, bool> class_cache;
    auto it = class_cache.find(header->class_ptr);
    bool eligible = false;
    if (it != class_cache.end()) eligible = it->second;
    else {
        const auto name = JumpPadClassName(object);
        eligible = name == L"BP_Jumppad_C";
        if (class_cache.size() < 4096) class_cache.emplace(header->class_ptr, eligible);
    }
    return eligible && JumpPadObjectName(object).find(L"Default__") == std::wstring::npos;
}
bool JumpPadInheritsClass(void* object, const wchar_t* class_name) {
    if (!IsReadable(object, sizeof(UObjectHeader))) return false;
    void* cls = static_cast<const UObjectHeader*>(object)->class_ptr;
    for (int i = 0; i < 12 && IsReadable(cls, sizeof(UStructHeader)); ++i) {
        if (JumpPadObjectName(cls) == class_name) return true;
        cls = static_cast<const UStructHeader*>(cls)->super_struct;
    }
    return false;
}
const GlobalObjectArray* Objects() {
    if (!g_game_module) return nullptr;
    auto* objects = reinterpret_cast<const GlobalObjectArray*>(
        reinterpret_cast<uintptr_t>(g_game_module) + kGlobalObjectArrayRva);
    if (!IsReadable(objects, sizeof(GlobalObjectArray)) ||
        objects->num_elements < 0 || objects->num_elements > 1000000 ||
        objects->max_elements < objects->num_elements || !objects->objects ||
        !IsReadable(objects->objects,
                    static_cast<size_t>(objects->num_elements) * sizeof(GlobalObjectItem)))
        return nullptr;
    return objects;
}
bool JumpPadValid(const Target& entry, const GlobalObjectArray* objects) {
    return objects && entry.index >= 0 && entry.index < objects->num_elements &&
        objects->objects[entry.index].object == entry.actor &&
        objects->objects[entry.index].serial_number == entry.serial &&
        NamedJumpPad(entry.actor);
}
int32_t JumpPadParam(void* function, const wchar_t* property, const wchar_t* type,
              int32_t length = 96) {
    if (!IsReadable(function, sizeof(UStructHeader))) return -1;
    void* field = static_cast<const UStructHeader*>(function)->children;
    for (int i = 0; i < 48 && IsReadable(field, sizeof(UFieldHeader)); ++i) {
        const auto* prop = static_cast<const UFieldHeader*>(field);
        if (UnrealName(prop->object.name) == property &&
            IsReadable(prop->object.class_ptr, sizeof(UObjectHeader)) &&
            JumpPadObjectName(prop->object.class_ptr) == type) {
            const int32_t offset = FindClassPropertyOffset(function, property);
            const int32_t bytes = wcscmp(type, L"NameProperty") == 0 ? 8 :
                                  wcscmp(type, L"StructProperty") == 0 ? 16 :
                                  wcscmp(type, L"StrProperty") == 0 ? 12 : 4;
            return offset >= 0 && offset + bytes <= length ? offset : -1;
        }
        field = prop->next;
    }
    return -1;
}
// A UFunction's signature is per class, not per individual component/MID.
// Cache only validated signatures; keep different classes independent.
struct JumpPadMaterialCall {
    void* function = nullptr;
    int32_t first = -1;
    int32_t second = -1;
};
JumpPadMaterialCall JumpPadCall(void* object, int operation) {
    if (!g_process_event || !IsReadable(object, sizeof(UObjectHeader))) return {};
    void* cls = static_cast<const UObjectHeader*>(object)->class_ptr;
    if (!IsReadable(cls, sizeof(UObjectHeader))) return {};
    static std::unordered_map<void*, JumpPadMaterialCall> reads;
    static std::unordered_map<void*, JumpPadMaterialCall> writes;
    static std::unordered_map<void*, JumpPadMaterialCall> tints;
    auto& cache = operation == 0 ? reads : (operation == 1 ? writes : tints);
    if (const auto it = cache.find(cls); it != cache.end() &&
        IsReadable(it->second.function, sizeof(UStructHeader))) return it->second;
    const wchar_t* method = operation == 0 ? L"GetMaterial" :
                            operation == 1 ? L"SetMaterial" : L"SetVectorParameterValue";
    JumpPadMaterialCall result{};
    result.function = FindReflectedFunction(object, method);
    if (operation == 2 && !result.function)
        result.function = FindReflectedFunction(object, L"K2_SetVectorParameterValue");
    if (operation == 2) {
        result.first = JumpPadParam(result.function, L"ParameterName", L"NameProperty");
        result.second = JumpPadParam(result.function, L"Value", L"StructProperty");
    } else {
        result.first = JumpPadParam(result.function, L"ElementIndex", L"IntProperty");
        result.second = JumpPadParam(result.function,
            operation == 0 ? L"ReturnValue" : L"Material", L"ObjectProperty");
    }
    if (result.function && result.first >= 0 && result.second >= 0 && cache.size() < 1024)
        cache[cls] = result;
    return result;
}
void* ReadJumpPadMesh(void* actor) {
    void* mesh = ReadReflectedObject(actor, L"Mover");
    if (!JumpPadInheritsClass(mesh, L"StaticMeshComponent") ||
        static_cast<const UObjectHeader*>(mesh)->outer != actor) return nullptr;
    // The packaged jump pad and mover blueprint references the SM_Jumppad asset.
    void* asset = ReadReflectedObject(mesh, L"StaticMesh");
    if (!asset || JumpPadObjectName(asset) != L"SM_Jumppad") return nullptr;
    return mesh;
}
void* JumpPadGetMaterial(void* mesh) {
    if (!mesh || !g_process_event) return nullptr;
    const auto call = JumpPadCall(mesh, 0);
    if (!call.function || call.first < 0 || call.second < 0) return nullptr;
    alignas(16) std::array<uint8_t, 96> args{};
    g_process_event(mesh, call.function, args.data());
    void* material = nullptr;
    std::memcpy(&material, args.data() + call.second, sizeof(material));
    return IsReadable(material, sizeof(UObjectHeader)) ? material : nullptr;
}
bool JumpPadSetMaterial(void* mesh, void* material) {
    if (!mesh || !IsReadable(material, sizeof(UObjectHeader)) || !g_process_event)
        return false;
    const auto call = JumpPadCall(mesh, 1);
    if (!call.function || call.first < 0 || call.second < 0) return false;
    alignas(16) std::array<uint8_t, 96> args{};
    std::memcpy(args.data() + call.second, &material, sizeof(material));
    g_process_event(mesh, call.function, args.data());
    return JumpPadGetMaterial(mesh) == material;
}
bool JumpPadFindColourParameter(void* material, FNameView& parameter) {
    // The SM_Jumppad slot-0 material is M_Gold_Rough_Yellow. Use only its actual
    // MIC vector override, not a made-up FName or a shader parameter guess.
    if (!material || JumpPadObjectName(material) != L"M_Gold_Rough_Yellow") return false;
    void* cls = static_cast<const UObjectHeader*>(material)->class_ptr;
    const int32_t offset = FindClassPropertyOffset(cls, L"VectorParameterValues");
    if (offset < 0 || offset > 0x400 ||
        !IsReadable(static_cast<uint8_t*>(material) + offset, sizeof(UObjectPointerArray)))
        return false;
    UObjectPointerArray values{};
    std::memcpy(&values, static_cast<uint8_t*>(material) + offset, sizeof(values));
    if (values.count != 1 || !IsReadable(values.data, sizeof(parameter))) return false;
    std::memcpy(&parameter, values.data, sizeof(parameter));
    return UnrealName(parameter) == L"Color";
}
void* JumpPadCreateMID(void* mesh, void* source) {
    if (!mesh || !source || !g_process_event) return nullptr;
    struct Method { const wchar_t* name; const wchar_t* parent; bool named; };
    constexpr Method methods[] = {
        {L"CreateDynamicMaterialInstance", L"SourceMaterial", true},
        {L"CreateAndSetMaterialInstanceDynamicFromMaterial", L"Parent", false},
        {L"CreateAndSetMaterialInstanceDynamic", nullptr, false}
    };
    for (const Method& method : methods) {
        void* fn = FindReflectedFunction(mesh, method.name);
        const int32_t index = JumpPadParam(fn, L"ElementIndex", L"IntProperty");
        const int32_t result = JumpPadParam(fn, L"ReturnValue", L"ObjectProperty");
        const int32_t parent = method.parent ? JumpPadParam(fn, method.parent, L"ObjectProperty") : -1;
        const int32_t name = method.named ? JumpPadParam(fn, L"OptionalName", L"NameProperty") : -1;
        if (index < 0 || result < 0 || (method.parent && parent < 0) ||
            (method.named && name < 0)) continue;
        if (!method.parent && JumpPadGetMaterial(mesh) != source && !JumpPadSetMaterial(mesh, source))
            break;
        alignas(16) std::array<uint8_t, 96> args{};
        if (parent >= 0) std::memcpy(args.data() + parent, &source, sizeof(source));
        g_process_event(mesh, fn, args.data());
        void* mid = nullptr;
        std::memcpy(&mid, args.data() + result, sizeof(mid));
        if (IsReadable(mid, sizeof(UObjectHeader)) && JumpPadGetMaterial(mesh) == mid &&
            JumpPadClassName(mid).find(L"MaterialInstanceDynamic") != std::wstring::npos)
            return mid;
    }
    if (JumpPadGetMaterial(mesh) != source) JumpPadSetMaterial(mesh, source);
    return nullptr;
}
bool JumpPadSetTint(void* material, FNameView name_value, uint32_t rgb) {
    const auto call = JumpPadCall(material, 2);
    if (!call.function || call.first < 0 || call.second < 0 ||
        call.second + static_cast<int32_t>(sizeof(LinearColor)) > 96) return false;
    const auto linear = [](uint32_t channel) {
        const float x = static_cast<float>(channel) / 255.0f;
        return x <= 0.04045f ? x / 12.92f : std::pow((x + 0.055f) / 1.055f, 2.4f);
    };
    LinearColor colour{linear((rgb >> 16) & 255), linear((rgb >> 8) & 255),
                       linear(rgb & 255), 1.0f};
    alignas(16) std::array<uint8_t, 96> args{};
    std::memcpy(args.data() + call.first, &name_value, sizeof(name_value));
    std::memcpy(args.data() + call.second, &colour, sizeof(colour));
    g_process_event(material, call.function, args.data());
    return true;
}
void JumpPadColourTarget(Target& target, bool enabled, uint32_t rgb) {
    const ULONGLONG now = GetTickCount64();
    // A stable MID does not need 20 reflected GetMaterial calls per second.
    // Periodically re-check ownership in case Refunct replaces the material.
    if (enabled && target.material_instance && target.source &&
        target.applied_rgb == rgb && now < target.next_colour_check) return;
    if (!target.mesh || !IsReadable(target.mesh, sizeof(UObjectHeader)))
        target.mesh = ReadJumpPadMesh(target.actor);
    if (!target.mesh) {
        if (!s_mesh_warning) {
            s_mesh_warning = true;
            Log(L"Jump pads: BP_Jumppad.Mover/SM_Jumppad mesh unavailable");
        }
        return;
    }
    void* displayed = JumpPadGetMaterial(target.mesh);
    if (!displayed) return;
    const bool ours = target.material_instance && displayed == target.material_instance &&
                      target.source && IsReadable(target.source, sizeof(UObjectHeader));
    if (!enabled) {
        if (ours && !JumpPadSetMaterial(target.mesh, target.source)) return;
        target.material_instance = nullptr;
        target.source = nullptr;
        target.colour_unsupported = false;
        return;
    }
    if (ours && target.applied_rgb == rgb) {
        target.next_colour_check = now + 750;
        return;
    }
    if (target.colour_unsupported && !ours) return;
    void* source = ours ? target.source : displayed;
    FNameView parameter{};
    if (ours) parameter = target.colour_parameter;
    else if (!JumpPadFindColourParameter(source, parameter)) {
        target.colour_unsupported = true;
        if (!s_colour_warning) {
            s_colour_warning = true;
            Log(L"Jump pads: SM_Jumppad material lacks verified M_Gold_Rough_Yellow/Color override");
        }
        return;
    }
    void* mid = ours ? target.material_instance : JumpPadCreateMID(target.mesh, source);
    if (!mid || !JumpPadSetTint(mid, parameter, rgb)) {
        if (mid && !ours) JumpPadSetMaterial(target.mesh, source);
        target.colour_unsupported = true;
        if (!s_colour_warning) {
            s_colour_warning = true;
            Log(L"Jump pads: failed to create/tint dynamic jump pad material");
        }
        return;
    }
    target.material_instance = mid;
    target.source = source;
    target.colour_parameter = parameter;
    target.applied_rgb = rgb;
    target.colour_unsupported = false;
    target.next_colour_check = now + 750;
}


// In the cooked BP_MyCharacter asset, Jumppad is an ObjectProperty argument of
// PerformJumppadJump, NOT a field on BP_MyCharacter. Checking the character
// for a Jumppad field silently disables the old launch-height feature.
// Observe the actual Blueprint call instead, and adjust ONLY the resulting
// upward launch velocity. Some UE4 LaunchCharacter calls defer application
// until the next movement update; a short pending window covers that case.
struct LaunchSample {
    MyCharacter* player = nullptr;
    void* function = nullptr;
    int32_t pad_argument_offset = -1;
    bool pending = false;
    float before_z = 0.0f;
    ULONGLONG expires_at = 0;
    ULONGLONG last_scaled_at = 0;
    ULONGLONG next_resolve_at = 0;
    bool warned_missing_function = false;
    bool warned_no_impulse = false;
    bool warned_hook_unavailable = false;
    bool warned_bad_parameter = false;
    bool warned_unreadable_velocity = false;
    bool warned_other_actor = false;
    bool logged_launch_event = false;
    bool scaled_once = false;
    // Refunct's Blueprint VM can invoke PerformJumppadJump without going
    // through UObject::ProcessEvent. Observe the resulting native motion too.
    Vec3 last_position{};
    float last_velocity_z = 0.0f;
    ULONGLONG last_sample_at = 0;
    bool has_motion_sample = false;
    bool reported_pad_position = false;
    bool reported_proximity_fallback = false;
    bool warned_no_pad_positions = false;
    bool reported_unmatched_velocity = false;
};
LaunchSample s_launch{};

bool ReadLaunchVelocity(MyCharacter* player, float& z) {
    if (!player || !IsReadable(player, sizeof(MyCharacter)) ||
        !IsReadable(player->movement, sizeof(MovementComponent))) return false;
    z = player->movement->velocity.z;
    return std::isfinite(z) && std::abs(z) <= 100000.0f;
}

void ObservePendingLaunch(MyCharacter* player) {
    if (!s_launch.pending || player != s_launch.player) return;
    const ULONGLONG now = GetTickCount64();
    if (now > s_launch.expires_at || Config().height_percent == 100 ||
        g_restart_requested || g_full_level_reload_requested) {
        s_launch.pending = false;
        if (now > s_launch.expires_at && !s_launch.warned_no_impulse) {
            s_launch.warned_no_impulse = true;
            Log(L"Jump pad launch height: pad event received, but no launch impulse was detected");
        }
        return;
    }
    float z = 0.0f;
    if (!ReadLaunchVelocity(player, z)) return;
    // The native event may have scheduled rather than applied LaunchCharacter.
    // A genuine launch produces a discontinuity in the vertical velocity; a
    // few cm/s of gravity or ordinary airborne movement is not sufficient.
    if (z < 60.0f || std::abs(z - s_launch.before_z) < 40.0f) return;
    const double ratio = std::sqrt(static_cast<double>(Config().height_percent) / 100.0);
    const double scaled = static_cast<double>(z) * ratio;
    if (!std::isfinite(scaled) || scaled < 0.0) {
        s_launch.pending = false;
        return;
    }
    const float updated = static_cast<float>((std::clamp)(scaled, 0.0, 30000.0));
    player->movement->velocity.z = updated;
    s_launch.pending = false;  // Never multiply the same pad launch twice.
    s_launch.last_scaled_at = now;
    if (!s_launch.scaled_once) {
        Log((L"Jump pad launch height: applied " +
             std::to_wstring(Config().height_percent) + L"% multiplier (velocity Z " +
             std::to_wstring(z) + L" -> " + std::to_wstring(updated) + L")").c_str());
    }
    s_launch.scaled_once = true;
}

void PrepareLaunchFunction(MyCharacter* player) {
    if (s_launch.player != player) {
        s_launch = {};
        s_launch.player = player;
    }
    if (!player || Config().height_percent == 100) return;
    if (!g_jump_pad_launch_hook_available) {
        if (!s_launch.warned_hook_unavailable) {
            s_launch.warned_hook_unavailable = true;
            Log(L"Jump pad launch height: ProcessEvent hook unavailable; multiplier inactive");
        }
        return;
    }
    if (s_launch.function) return;
    const ULONGLONG now = GetTickCount64();
    if (now < s_launch.next_resolve_at) return;
    s_launch.next_resolve_at = now + 1000;
    void* function = FindReflectedFunction(player, L"PerformJumppadJump");
    if (function && IsReadable(function, sizeof(UStructHeader))) {
        // Blueprint's Jumppad parameter is a BP_Jumppad_C ObjectProperty.
        // An unavailable argument does not justify inventing a character field;
        // the uniquely named function is still an explicit pad-launch signal.
        s_launch.pad_argument_offset = JumpPadParam(function, L"Jumppad", L"ObjectProperty", 256);
        s_launch.function = function;
        Log((L"Jump pad launch height: tracking PerformJumppadJump (" +
             std::to_wstring(Config().height_percent) + L"%, pad argument " +
             (s_launch.pad_argument_offset >= 0 ? L"resolved" : L"not resolved") +
             L")").c_str());
    } else if (!s_launch.warned_missing_function) {
        s_launch.warned_missing_function = true;
        Log(L"Jump pad launch height: PerformJumppadJump event unavailable");
    }
}

// K2_GetActorLocation returns a *world-space* point even when a pad is
// parented to a moving section. Never guess native actor field offsets.
bool JumpPadWorldPosition(void* actor, Vec3& out) {
    if (!actor || !g_process_event || !NamedJumpPad(actor)) return false;
    void* fn = FindReflectedFunction(actor, L"K2_GetActorLocation");
    if (!fn) fn = FindReflectedFunction(actor, L"GetActorLocation");
    const int result = JumpPadParam(fn, L"ReturnValue", L"StructProperty", 128);
    if (result < 0 || result > 128 - static_cast<int>(sizeof(Vec3))) return false;
    alignas(16) std::array<uint8_t, 128> args{};
    g_process_event(actor, fn, args.data());
    std::memcpy(&out, args.data() + result, sizeof(out));
    return std::isfinite(out.x) && std::isfinite(out.y) &&
           std::isfinite(out.z) &&
           std::abs(out.x) < 1000000.0f && std::abs(out.y) < 1000000.0f &&
           std::abs(out.z) < 1000000.0f;
}

void RefreshJumpPadPositions(const GlobalObjectArray* objects, ULONGLONG now) {
    if (Config().height_percent == 100) return;
    for (auto& target : s_targets) {
        if (!JumpPadValid(target, objects)) {
            target = {};  // Drop stale world actors after restarts/map reloads.
            continue;
        }
        if (target.world_position_valid && now < target.next_position_refresh) continue;
        Vec3 position{};
        const bool valid = JumpPadWorldPosition(target.actor, position);
        target.world_position_valid = valid;
        target.next_position_refresh = now + 250;
        if (valid) {
            target.world_position = position;
            target.last_position_valid_at = now;
            if (!s_launch.reported_pad_position) {
                s_launch.reported_pad_position = true;
                Log((L"Jump pad launch height: reading native pad world positions (example " +
                     std::to_wstring(position.x) + L", " +
                     std::to_wstring(position.y) + L", " +
                     std::to_wstring(position.z) + L")").c_str());
            }
        }
    }
}

bool AtKnownJumpPad(const Vec3& position, ULONGLONG now) {
    // A jump-pad launch is much stronger than the ordinary jump and occurs
    // on the pad surface. Requiring proximity prevents changing ordinary
    // jumps, wall jumps, tube motion or speed hacks elsewhere in the map.
    constexpr double kHorizontalRadiusSquared = 360.0 * 360.0;
    for (const auto& target : s_targets) {
        if (!target.world_position_valid ||
            now < target.last_position_valid_at ||
            now - target.last_position_valid_at > 1000) continue;
        const double dx = static_cast<double>(position.x) - target.world_position.x;
        const double dy = static_cast<double>(position.y) - target.world_position.y;
        const double dz = static_cast<double>(position.z) - target.world_position.z;
        if (dx * dx + dy * dy <= kHorizontalRadiusSquared &&
            dz >= -180.0 && dz <= 480.0) return true;
    }
    return false;
}

void ObserveNearbyJumpPadImpulse(MyCharacter* player) {
    if (Config().height_percent == 100 || !player || !player->root_component ||
        !IsReadable(player->root_component, sizeof(SceneComponent))) {
        s_launch.has_motion_sample = false;
        return;
    }
    float z = 0.0f;
    if (!ReadLaunchVelocity(player, z)) {
        s_launch.has_motion_sample = false;
        return;
    }
    const Vec3 pos = player->root_component->location;
    if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z)) {
        s_launch.has_motion_sample = false;
        return;
    }
    const ULONGLONG now = GetTickCount64();
    const float prior_z = s_launch.last_velocity_z;
    const Vec3 prior_pos = s_launch.last_position;
    const bool consecutive = s_launch.has_motion_sample &&
        now >= s_launch.last_sample_at && now - s_launch.last_sample_at < 250;
    s_launch.last_sample_at = now;
    s_launch.last_position = pos;
    s_launch.last_velocity_z = z;
    s_launch.has_motion_sample = true;

    if (!consecutive || g_flying || g_restart_requested ||
        g_full_level_reload_requested || s_launch.pending ||
        (g_practice_active && g_practice_phase != PracticePhase::Ready) ||
        (s_launch.last_scaled_at && now - s_launch.last_scaled_at < 400)) return;

    // Reject ordinary jumps using the reflected current JumpZVelocity, when
    // available. No matter how much Jump Height has been modified, a normal
    // jump near a pad must not be scaled as a jump-pad launch.
    float normal_jump = 400.0f;
    if (g_tuning_jump_velocity.address &&
        IsReadable(g_tuning_jump_velocity.address, sizeof(float))) {
        const float measured = *g_tuning_jump_velocity.address;
        if (std::isfinite(measured) && measured > 0.0f && measured < 30000.0f)
            normal_jump = measured;
    }
    const float minimum_launch_z = (std::max)(650.0f, normal_jump * 1.20f);
    const float required_velocity_rise = (std::max)(180.0f, normal_jump * 0.30f);
    if (z < minimum_launch_z || z - prior_z < required_velocity_rise) return;

    if (!AtKnownJumpPad(prior_pos, now) && !AtKnownJumpPad(pos, now)) {
        if (!s_launch.reported_unmatched_velocity) {
            s_launch.reported_unmatched_velocity = true;
            Log((L"Jump pad launch height: upward impulse detected away from cached pad bounds (" +
                 std::to_wstring(z) + L" cm/s at " +
                 std::to_wstring(pos.x) + L", " +
                 std::to_wstring(pos.y) + L", " +
                 std::to_wstring(pos.z) + L")").c_str());
        }
        return;
    }

    const double factor = std::sqrt(static_cast<double>(Config().height_percent) / 100.0);
    const double product = static_cast<double>(z) * factor;
    if (!std::isfinite(product) || product <= 0.0) return;
    const float scaled = static_cast<float>((std::clamp)(product, 0.0, 30000.0));
    if (!std::isfinite(scaled)) return;
    player->movement->velocity.z = scaled;
    s_launch.last_velocity_z = scaled;
    s_launch.last_scaled_at = now;
    if (!s_launch.reported_proximity_fallback) {
        s_launch.reported_proximity_fallback = true;
        Log((L"Jump pad launch height: native velocity at pad detected; applied " +
             std::to_wstring(Config().height_percent) + L"% (" +
             std::to_wstring(z) + L" -> " + std::to_wstring(scaled) + L")").c_str());
    }
}

void JumpPadDiscover(const GlobalObjectArray* objects) {
    const int32_t end = (std::min)(objects->num_elements, s_scan_cursor + kScanBatch);
    ColourWorkBudget budget(700);
    int examined = 0;
    for (; s_scan_cursor < end; ++s_scan_cursor) {
        if ((examined & 31) == 0 && budget.expired()) break;
        ++examined;
        const GlobalObjectItem& item = objects->objects[s_scan_cursor];
        if (!item.object || (static_cast<uint32_t>(item.flags) & 0x30000000u) ||
            !NamedJumpPad(item.object)) continue;
        const auto existing = std::find_if(s_targets.begin(), s_targets.end(),
            [&](const Target& t) { return t.actor == item.object &&
                                          t.serial == item.serial_number; });
        if (existing != s_targets.end()) continue;
        if (s_targets.size() >= kMaximumTargets) break;
        Target target{};
        target.actor = item.object;
        target.index = s_scan_cursor;
        target.serial = item.serial_number;
        if (Config().custom_colour || s_restore_pending) {
            if (s_pending_colour == 0) s_apply_cursor = s_targets.size();
            ++s_pending_colour;
        }
        s_targets.push_back(target);
        s_next_update = 0;
    }
    if (s_scan_cursor >= objects->num_elements || s_targets.size() >= kMaximumTargets) {
        s_scan_cursor = -1;
        s_discovery_watermark.Complete(objects->objects, objects->num_elements, GetTickCount64(),
            s_targets.empty() || Config().height_percent != 100 ? 5000 : 29000, s_next_scan);
        if (!s_discovery_reported) {
            s_discovery_reported = true;
            Log((L"Jump pads: found " + std::to_wstring(s_targets.size()) +
                 L" BP_Jumppad actors").c_str());
        }
    }
}
} // namespace

// Optional hook installed once during native startup. The hot path is just two
// pointer comparisons; no Unreal name lookups or disk writes for normal events.
ProcessEventFn g_original_jump_pad_process_event = nullptr;
bool g_jump_pad_launch_hook_available = false;
void __fastcall JumpPadProcessEventHook(void* object, void* /*edx*/,
                                        void* function, void* args) {
    if (!g_original_jump_pad_process_event) return;
    // The reflected UFunction is a unique pad-launch event. Parameter
    // validation is useful diagnostics, but must not block a genuine launch:
    // cooked Blueprint parameter layouts can differ from an offset lookup.
    if (g_jump_pad_launch_hook_available &&
        s_launch.function != nullptr && function == s_launch.function &&
        object != s_launch.player && !s_launch.warned_other_actor) {
        s_launch.warned_other_actor = true;
        Log(L"Jump pad launch height: matching function called on another character");
    }
    const bool launch_event = g_jump_pad_launch_hook_available &&
        s_launch.function != nullptr && object == s_launch.player &&
        function == s_launch.function &&
        Config().height_percent != 100;
    if (!launch_event) {
        g_original_jump_pad_process_event(object, function, args);
        return;
    }
    bool valid_pad = true;
    if (s_launch.pad_argument_offset >= 0) {
        valid_pad = args && IsReadable(static_cast<uint8_t*>(args) +
            s_launch.pad_argument_offset, sizeof(void*));
        if (valid_pad) {
            void* pad = nullptr;
            std::memcpy(&pad, static_cast<uint8_t*>(args) +
                        s_launch.pad_argument_offset, sizeof(pad));
            valid_pad = NamedJumpPad(pad);
        }
    }
    if (!s_launch.logged_launch_event) {
        s_launch.logged_launch_event = true;
        Log(L"Jump pad launch height: PerformJumppadJump event observed");
    }
    if (!valid_pad && !s_launch.warned_bad_parameter) {
        s_launch.warned_bad_parameter = true;
        Log(L"Jump pad launch height: pad argument unverified; using matched launch event");
    }
    float before = 0.0f;
    // The function identity is the authoritative launch signal. A missing or
    // unexpected Jumppad argument must not silently suppress the multiplier.
    const bool has_velocity = ReadLaunchVelocity(s_launch.player, before);
    if (!has_velocity && !s_launch.warned_unreadable_velocity) {
        s_launch.warned_unreadable_velocity = true;
        Log(L"Jump pad launch height: event received, but player velocity unreadable");
    }
    // Arm BEFORE the Blueprint executes, so an immediate LaunchCharacter call
    // and a deferred next-tick launch use the same reference sample.
    const ULONGLONG now = GetTickCount64();
    if (has_velocity && now - s_launch.last_scaled_at >= 120) {
        s_launch.pending = true;
        s_launch.before_z = before;
        s_launch.expires_at = now + 750;
    }
    g_original_jump_pad_process_event(object, function, args);
    if (s_launch.pending) ObservePendingLaunch(s_launch.player);
}

bool IsJumpPadCustomColourEnabled() { return Config().custom_colour; }
void SetJumpPadCustomColourEnabled(bool enabled) {
    if (Config().custom_colour == enabled) return;
    Config().custom_colour = enabled;
    s_restore_pending = !enabled;
    JumpPadChanged(enabled ? L"Jump pad colour: Custom" : L"Jump pad colour: Game default", false);
}
std::wstring GetJumpPadCustomColourHex() { return JumpPadHex(Config().rgb); }
void SetJumpPadCustomColourHex(uint32_t rgb) {
    if (rgb > 0xFFFFFFu || Config().rgb == rgb) return;
    Config().rgb = rgb;
    for (auto& target : s_targets) target.colour_unsupported = false;
    JumpPadChanged(L"Jump pad colour: " + JumpPadHex(rgb), false);
}
void OpenJumpPadColourPicker() {
    if (s_picker_open.exchange(true, std::memory_order_acq_rel)) {
        Notify(L"Jump pad colour picker already open"); return;
    }
    s_picker_initial.store(Config().rgb, std::memory_order_relaxed);
    s_picker_result.store(kNoPickerResult, std::memory_order_release);
    HANDLE thread = CreateThread(nullptr, 0, JumpPadPickerThread, nullptr, 0, nullptr);
    if (!thread) {
        s_picker_open.store(false, std::memory_order_release);
        Notify(L"Could not open jump pad colour picker");
    } else CloseHandle(thread);
}
void SetJumpPadHeightProfilePercent(int percent) {
    if (percent < 25 || Config().height_percent == percent) return;
    Config().height_percent = percent;
    JumpPadChanged(L"Jump pad launch height: " + std::to_wstring(percent) + L"%", true);
}
int GetJumpPadHeightPercent() { return Config().height_percent; }
bool IsJumpPadHeightModified() { return GetJumpPadHeightPercent() != 100; }
void AdjustJumpPadHeight(int direction) {
    if (!direction) return;
    constexpr int step = 25;
    const int old = Config().height_percent;
    const int next = direction < 0
        ? (std::max)(25, old - step)
        : (old <= (std::numeric_limits<int>::max)() - step ? old + step : old);
    if (next == old) return;
    Config().height_percent = next;
    JumpPadChanged(L"Jump pad launch height: " + std::to_wstring(next) + L"%", true);
}
void ResetJumpPadHeight() {
    if (Config().height_percent == 100) return;
    Config().height_percent = 100;
    JumpPadChanged(L"Jump pad launch height: 100% (normal)", true);
}
void UpdateJumpPads(MyCharacter* player) {
    FlushDeferredJumpPadSave();
    const uint32_t picked = s_picker_result.exchange(kNoPickerResult,
                                                     std::memory_order_acq_rel);
    if (picked <= 0xFFFFFFu) SetJumpPadCustomColourHex(picked);
    if (!player || g_restart_requested || g_full_level_reload_requested) return;
    PrepareLaunchFunction(player);
    ObservePendingLaunch(player);
    const bool needs_height = Config().height_percent != 100;
    const bool needs_colour = Config().custom_colour || s_restore_pending;
    if (!needs_colour) s_pending_colour = 0;
    if (!needs_height && !needs_colour && s_targets.empty()) {
        s_launch.has_motion_sample = false;
        return;
    }
    if (s_player != player) {
        s_player = player;
        // Keep existing MID/source ownership if the level reused its actors.
        // Every use still validates the native object index + serial; a new
        // map will safely discard invalid targets without writing through them.
        s_apply_cursor = 0;
        s_pending_colour = s_targets.size();
        s_scan_cursor = 0;
        s_next_scan = 0;
        s_next_update = 0;
        s_next_height_check = 0;
        s_launch.has_motion_sample = false;
        for (auto& target : s_targets) target.world_position_valid = false;
    }
    const ULONGLONG now = GetTickCount64();
    const auto* discovery_objects = Objects();
    if (discovery_objects && s_scan_cursor < 0 &&
        (Config().custom_colour || needs_height))
        s_scan_cursor = s_discovery_watermark.Resume(discovery_objects->objects, discovery_objects->num_elements, now, s_next_scan);
    if (discovery_objects && s_scan_cursor >= 0 && (Config().custom_colour || needs_height))
        JumpPadDiscover(discovery_objects); // CPU-bounded work every frame.
    else if (!Config().custom_colour && !needs_height)
        s_scan_cursor = -1; // No background scan while visuals are disabled.
    if (now >= s_next_update || s_pending_colour != 0) {
        const bool urgent = s_pending_colour != 0;
        s_next_update = now + (urgent ? 0 : kUpdateMs);
        const auto* objects = discovery_objects;
        if (objects) {
            // Launch-height detection retains its own 50 ms cadence even
            // while we are recolouring pads across multiple game frames.
            if (needs_height && now >= s_next_height_check) {
                s_next_height_check = now + kUpdateMs;
                RefreshJumpPadPositions(objects, now);
            }
            if (needs_colour && !s_targets.empty()) {
                const size_t count = s_targets.size();
                const size_t batch = urgent ? 5 : static_cast<size_t>(kApplyBatch);
                ColourWorkBudget apply_budget(1100);
                for (size_t checked = 0; checked < (std::min)(batch, count); ++checked) {
                    if (checked > 0 && apply_budget.expired()) break;
                    if (s_apply_cursor >= s_targets.size()) s_apply_cursor = 0;
                    Target& target = s_targets[s_apply_cursor++];
                    if (s_pending_colour) --s_pending_colour;
                    if (!JumpPadValid(target, objects)) { target = {}; s_next_scan = 0; continue; }
                    if (Config().custom_colour || target.material_instance)
                        JumpPadColourTarget(target, Config().custom_colour, Config().rgb);
                }
            }
            s_targets.erase(std::remove_if(s_targets.begin(), s_targets.end(),
                [](const Target& t) { return t.actor == nullptr; }), s_targets.end());
            if (s_apply_cursor >= s_targets.size()) s_apply_cursor = 0;
            if (!Config().custom_colour) {
                s_restore_pending = std::any_of(s_targets.begin(), s_targets.end(),
                    [](const Target& t) { return t.material_instance != nullptr; });
                // Preserve tracked actors when Launch Height is enabled even
                // after resetting the colour to Game Default.
                if (!s_restore_pending && !needs_height) {
                    s_targets.clear();
                    s_scan_cursor = -1;
                }
            }
        }
    }
    // Sample velocity EVERY game tick; scanning/colour remain at 20 Hz.
    // Blueprint VM calls may bypass ProcessEvent, but physics velocity still
    // changes at the native movement component when the pad launches us.
    if (needs_height) {
        if (s_scan_cursor < 0 && !s_targets.empty() &&
            !s_launch.warned_no_pad_positions &&
            std::none_of(s_targets.begin(), s_targets.end(),
                [](const Target& target) { return target.world_position_valid; })) {
            s_launch.warned_no_pad_positions = true;
            Log(L"Jump pad launch height: cannot read native pad world positions; proximity fallback unavailable");
        }
        ObserveNearbyJumpPadImpulse(player);
    }
}
} // namespace refunct

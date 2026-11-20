#include "refunct/internal_api.hpp"
#include "refunct/colour_work_budget.hpp"
#include "refunct/discovery_watermark.hpp"
#include "refunct/ghost_replay_tile_contact.hpp"
#include <unordered_map>
#include <commdlg.h>
#include <limits>

namespace refunct {
namespace {
// Refunct-WindowsNoEditor.pak contains BP_Lift_C, BP_Mover_C, the LiftMover
// timeline, LiftMesh/SM_Lift and an M_Gold_Rough_Yellow MIC with Color vector.
// Match these exact actor types: the 31 BP_LevelRoot_C sections are unrelated.
constexpr size_t kMaximumTargets = 256;
constexpr int kScanBatch = 2048; // Scan incrementally every tick, under CPU budget.
constexpr int kApplyBatch = 12;
constexpr ULONGLONG kUpdateMs = 50;

struct Settings {
    bool loaded = false;
    bool custom_colour = false;
    uint32_t rgb = 0x4CD9FF;
    int speed_percent = 100;
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
    void* timeline = nullptr;
    void* get_rate = nullptr;
    void* set_rate = nullptr;
    int32_t get_rate_out = -1;
    int32_t set_rate_in = -1;
    float native_rate = 0.0f;
    float written_rate = 0.0f;
    bool rate_modified = false;
    bool rate_unsupported = false;
};
std::vector<Target> s_targets{};
MyCharacter* s_player = nullptr;
int32_t s_scan_cursor = 0;
size_t s_apply_cursor = 0;
ULONGLONG s_next_scan = 0;
DiscoveryWatermark s_discovery_watermark{};
ULONGLONG s_next_update = 0;
ULONGLONG s_next_speed_check = 0;
size_t s_pending_colour = 0; // Flush colour changes to cached actors without rescanning UObject array.
ULONGLONG s_save_due = 0;
bool s_save_dirty = false;
bool s_restore_pending = false;
bool s_discovery_reported = false;
bool s_colour_warning = false;
bool s_speed_warning = false;

constexpr uint32_t kNoPickerResult = 0xFFFFFFFFu;
constexpr uint32_t kPickerCancelled = 0x01000000u;
std::atomic<uint32_t> s_picker_result{kNoPickerResult};
std::atomic<uint32_t> s_picker_initial{0};
std::atomic<bool> s_picker_open{false};

std::wstring PlatformHex(uint32_t rgb) {
    wchar_t text[8]{};
    swprintf_s(text, L"#%06X", rgb & 0xFFFFFFu);
    return text;
}
uint32_t PlatformParseHex(const wchar_t* text, uint32_t fallback) {
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
    GetPrivateProfileStringW(L"PlatformColours", L"Hex", L"#4CD9FF", hex,
                             static_cast<DWORD>(std::size(hex)), g_settings_path.c_str());
    s_settings.rgb = PlatformParseHex(hex, s_settings.rgb);
    s_settings.custom_colour =
        GetPrivateProfileIntW(L"PlatformColours", L"CustomEnabled", 0,
                              g_settings_path.c_str()) != 0;
    const int speed = GetPrivateProfileIntW(L"PlatformMovement", L"SpeedPercent", 100,
                                            g_settings_path.c_str());
    s_settings.speed_percent = speed >= 25 && speed % 25 == 0 ? speed : 100;
    s_settings.loaded = true;
    return s_settings;
}
void SaveConfig() {
    if (g_settings_path.empty()) return;
    const Settings& settings = Config();
    bool ok = QueueSettingsValue(L"PlatformColours", L"Hex",
                  PlatformHex(settings.rgb).c_str()) != 0;
    ok &= QueueSettingsValue(L"PlatformColours", L"CustomEnabled",
               settings.custom_colour ? L"1" : L"0") != 0;
    ok &= QueueSettingsValue(L"PlatformMovement", L"SpeedPercent",
               std::to_wstring(settings.speed_percent).c_str()) != 0;
    if (!ok) Log(L"Platforms: could not write platform settings");
}
void FlushDeferredPlatformSave() {
    if (!s_save_dirty || GetTickCount64() < s_save_due) return;
    s_save_dirty = false;
    SaveConfig();
}
void Changed(const std::wstring& message, bool affects_gameplay) {
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
        SaveConfig();
    }
    s_next_update = 0;
    Notify(message + (affects_gameplay ? L" (assisted run)" : L" (visual only)"));
}

DWORD WINAPI PlatformPickerThread(LPVOID) {
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

std::wstring ObjectName(void* object) {
    return IsReadable(object, sizeof(UObjectHeader))
        ? UnrealName(static_cast<const UObjectHeader*>(object)->name) : L"";
}
std::wstring PlatformClassName(void* object) {
    if (!IsReadable(object, sizeof(UObjectHeader))) return L"";
    void* cls = static_cast<const UObjectHeader*>(object)->class_ptr;
    return ObjectName(cls);
}
bool NamedPlatform(void* object) {
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
        const auto name = PlatformClassName(object);
        eligible = name == L"BP_Lift_C" || name == L"BP_Mover_C";
        if (class_cache.size() < 4096) class_cache.emplace(header->class_ptr, eligible);
    }
    return eligible && ObjectName(object).find(L"Default__") == std::wstring::npos;
}
bool InheritsClass(void* object, const wchar_t* class_name) {
    if (!IsReadable(object, sizeof(UObjectHeader))) return false;
    void* cls = static_cast<const UObjectHeader*>(object)->class_ptr;
    for (int i = 0; i < 12 && IsReadable(cls, sizeof(UStructHeader)); ++i) {
        if (ObjectName(cls) == class_name) return true;
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
bool Valid(const Target& entry, const GlobalObjectArray* objects) {
    return objects && entry.index >= 0 && entry.index < objects->num_elements &&
        objects->objects[entry.index].object == entry.actor &&
        objects->objects[entry.index].serial_number == entry.serial &&
        NamedPlatform(entry.actor);
}
int32_t PlatformParam(void* function, const wchar_t* property, const wchar_t* type,
              int32_t length = 96) {
    if (!IsReadable(function, sizeof(UStructHeader))) return -1;
    void* field = static_cast<const UStructHeader*>(function)->children;
    for (int i = 0; i < 48 && IsReadable(field, sizeof(UFieldHeader)); ++i) {
        const auto* prop = static_cast<const UFieldHeader*>(field);
        if (UnrealName(prop->object.name) == property &&
            IsReadable(prop->object.class_ptr, sizeof(UObjectHeader)) &&
            ObjectName(prop->object.class_ptr) == type) {
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
struct PlatformMaterialCall {
    void* function = nullptr;
    int32_t first = -1;
    int32_t second = -1;
};
PlatformMaterialCall PlatformCall(void* object, int operation) {
    if (!g_process_event || !IsReadable(object, sizeof(UObjectHeader))) return {};
    void* cls = static_cast<const UObjectHeader*>(object)->class_ptr;
    if (!IsReadable(cls, sizeof(UObjectHeader))) return {};
    static std::unordered_map<void*, PlatformMaterialCall> reads;
    static std::unordered_map<void*, PlatformMaterialCall> writes;
    static std::unordered_map<void*, PlatformMaterialCall> tints;
    auto& cache = operation == 0 ? reads : (operation == 1 ? writes : tints);
    if (const auto it = cache.find(cls); it != cache.end() &&
        IsReadable(it->second.function, sizeof(UStructHeader))) return it->second;
    const wchar_t* method = operation == 0 ? L"GetMaterial" :
                            operation == 1 ? L"SetMaterial" : L"SetVectorParameterValue";
    PlatformMaterialCall result{};
    result.function = FindReflectedFunction(object, method);
    if (operation == 2 && !result.function)
        result.function = FindReflectedFunction(object, L"K2_SetVectorParameterValue");
    if (operation == 2) {
        result.first = PlatformParam(result.function, L"ParameterName", L"NameProperty");
        result.second = PlatformParam(result.function, L"Value", L"StructProperty");
    } else {
        result.first = PlatformParam(result.function, L"ElementIndex", L"IntProperty");
        result.second = PlatformParam(result.function,
            operation == 0 ? L"ReturnValue" : L"Material", L"ObjectProperty");
    }
    if (result.function && result.first >= 0 && result.second >= 0 && cache.size() < 1024)
        cache[cls] = result;
    return result;
}
void* ReadLiftMesh(void* actor) {
    void* mesh = ReadReflectedObject(actor, L"LiftMesh");
    if (!InheritsClass(mesh, L"StaticMeshComponent") ||
        static_cast<const UObjectHeader*>(mesh)->outer != actor) return nullptr;
    // The packaged lift and mover blueprint references the SM_Lift asset.
    void* asset = ReadReflectedObject(mesh, L"StaticMesh");
    if (!asset || ObjectName(asset) != L"SM_Lift") return nullptr;
    return mesh;
}
void* ReadLiftTimeline(void* actor) {
    void* timeline = ReadReflectedObject(actor, L"LiftMover");
    if (!InheritsClass(timeline, L"TimelineComponent") ||
        static_cast<const UObjectHeader*>(timeline)->outer != actor) return nullptr;
    return timeline;
}
void* PlatformGetMaterial(void* mesh) {
    if (!mesh || !g_process_event) return nullptr;
    const auto call = PlatformCall(mesh, 0);
    if (!call.function || call.first < 0 || call.second < 0) return nullptr;
    alignas(16) std::array<uint8_t, 96> args{};
    g_process_event(mesh, call.function, args.data());
    void* material = nullptr;
    std::memcpy(&material, args.data() + call.second, sizeof(material));
    return IsReadable(material, sizeof(UObjectHeader)) ? material : nullptr;
}
bool PlatformSetMaterial(void* mesh, void* material) {
    if (!mesh || !IsReadable(material, sizeof(UObjectHeader)) || !g_process_event)
        return false;
    const auto call = PlatformCall(mesh, 1);
    if (!call.function || call.first < 0 || call.second < 0) return false;
    alignas(16) std::array<uint8_t, 96> args{};
    std::memcpy(args.data() + call.second, &material, sizeof(material));
    g_process_event(mesh, call.function, args.data());
    return PlatformGetMaterial(mesh) == material;
}
bool FindColourParameter(void* material, FNameView& parameter) {
    // The SM_Lift slot-0 material is M_Gold_Rough_Yellow. Use only its actual
    // MIC vector override, not a made-up FName or a shader parameter guess.
    if (!material || ObjectName(material) != L"M_Gold_Rough_Yellow") return false;
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
void* PlatformCreateMID(void* mesh, void* source) {
    if (!mesh || !source || !g_process_event) return nullptr;
    struct Method { const wchar_t* name; const wchar_t* parent; bool named; };
    constexpr Method methods[] = {
        {L"CreateDynamicMaterialInstance", L"SourceMaterial", true},
        {L"CreateAndSetMaterialInstanceDynamicFromMaterial", L"Parent", false},
        {L"CreateAndSetMaterialInstanceDynamic", nullptr, false}
    };
    for (const Method& method : methods) {
        void* fn = FindReflectedFunction(mesh, method.name);
        const int32_t index = PlatformParam(fn, L"ElementIndex", L"IntProperty");
        const int32_t result = PlatformParam(fn, L"ReturnValue", L"ObjectProperty");
        const int32_t parent = method.parent ? PlatformParam(fn, method.parent, L"ObjectProperty") : -1;
        const int32_t name = method.named ? PlatformParam(fn, L"OptionalName", L"NameProperty") : -1;
        if (index < 0 || result < 0 || (method.parent && parent < 0) ||
            (method.named && name < 0)) continue;
        if (!method.parent && PlatformGetMaterial(mesh) != source && !PlatformSetMaterial(mesh, source))
            break;
        alignas(16) std::array<uint8_t, 96> args{};
        if (parent >= 0) std::memcpy(args.data() + parent, &source, sizeof(source));
        g_process_event(mesh, fn, args.data());
        void* mid = nullptr;
        std::memcpy(&mid, args.data() + result, sizeof(mid));
        if (IsReadable(mid, sizeof(UObjectHeader)) && PlatformGetMaterial(mesh) == mid &&
            PlatformClassName(mid).find(L"MaterialInstanceDynamic") != std::wstring::npos)
            return mid;
    }
    if (PlatformGetMaterial(mesh) != source) PlatformSetMaterial(mesh, source);
    return nullptr;
}
bool PlatformSetTint(void* material, FNameView name_value, uint32_t rgb) {
    const auto call = PlatformCall(material, 2);
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
void ColourTarget(Target& target, bool enabled, uint32_t rgb) {
    const ULONGLONG now = GetTickCount64();
    // A stable MID does not need 20 reflected GetMaterial calls per second.
    // Periodically re-check ownership in case Refunct replaces the material.
    if (enabled && target.material_instance && target.source &&
        target.applied_rgb == rgb && now < target.next_colour_check) return;
    if (!target.mesh || !IsReadable(target.mesh, sizeof(UObjectHeader)))
        target.mesh = ReadLiftMesh(target.actor);
    if (!target.mesh) return;
    void* displayed = PlatformGetMaterial(target.mesh);
    if (!displayed) return;
    const bool ours = target.material_instance && displayed == target.material_instance &&
                      target.source && IsReadable(target.source, sizeof(UObjectHeader));
    if (!enabled) {
        if (ours && !PlatformSetMaterial(target.mesh, target.source)) return;
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
    else if (!FindColourParameter(source, parameter)) {
        target.colour_unsupported = true;
        if (!s_colour_warning) {
            s_colour_warning = true;
            Log(L"Platforms: SM_Lift material lacks verified M_Gold_Rough_Yellow/Color override");
        }
        return;
    }
    void* mid = ours ? target.material_instance : PlatformCreateMID(target.mesh, source);
    if (!mid || !PlatformSetTint(mid, parameter, rgb)) {
        if (mid && !ours) PlatformSetMaterial(target.mesh, source);
        target.colour_unsupported = true;
        if (!s_colour_warning) {
            s_colour_warning = true;
            Log(L"Platforms: failed to create/tint dynamic lift material");
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
bool BindTimeline(Target& target) {
    void* timeline = ReadLiftTimeline(target.actor);
    if (!timeline) return false;
    if (target.timeline == timeline && target.get_rate && target.set_rate &&
        target.get_rate_out >= 0 && target.set_rate_in >= 0) return true;
    target.timeline = timeline;
    target.get_rate = FindReflectedFunction(timeline, L"GetPlayRate");
    target.set_rate = FindReflectedFunction(timeline, L"SetPlayRate");
    target.get_rate_out = PlatformParam(target.get_rate, L"ReturnValue", L"FloatProperty");
    target.set_rate_in = PlatformParam(target.set_rate, L"NewRate", L"FloatProperty");
    target.native_rate = 0.0f;
    target.rate_modified = false;
    return target.get_rate_out >= 0 && target.set_rate_in >= 0;
}
bool ReadRate(Target& target, float& rate) {
    if (!target.get_rate || !g_process_event) return false;
    alignas(16) std::array<uint8_t, 96> args{};
    g_process_event(target.timeline, target.get_rate, args.data());
    std::memcpy(&rate, args.data() + target.get_rate_out, sizeof(rate));
    return std::isfinite(rate) && rate >= 0.001f && rate <= 1000.0f;
}
void WriteRate(Target& target, float rate) {
    alignas(16) std::array<uint8_t, 96> args{};
    std::memcpy(args.data() + target.set_rate_in, &rate, sizeof(rate));
    g_process_event(target.timeline, target.set_rate, args.data());
}
void SpeedTarget(Target& target, int percentage) {
    if (target.rate_unsupported || (!g_process_event)) return;
    if (!BindTimeline(target)) {
        target.rate_unsupported = true;
        if (!s_speed_warning) {
            s_speed_warning = true;
            Log(L"Platforms: BP_Lift/BP_Mover LiftMover timeline or SetPlayRate unavailable");
        }
        return;
    }
    float observed = 0.0f;
    if (!ReadRate(target, observed)) return;
    const float epsilon = (std::max)(0.0001f, std::abs(target.written_rate) * 0.0001f);
    const bool ours = target.rate_modified && std::abs(observed - target.written_rate) <= epsilon;
    if (!ours) {
        target.native_rate = observed; // Game may assign a new per-platform rate.
        target.rate_modified = false;
    }
    if (percentage == 100) {
        if (ours && target.native_rate > 0.0f) {
            WriteRate(target, target.native_rate);
            target.rate_modified = false;
        }
        return;
    }
    if (target.native_rate <= 0.0f) return;
    // No percentage cap in the UI; clamp only the native float playback rate.
    const float desired = static_cast<float>((std::min)(1000.0,
        static_cast<double>(target.native_rate) * percentage / 100.0));
    if (ours && std::abs(observed - desired) <= epsilon) return;
    WriteRate(target, desired);
    target.written_rate = desired;
    target.rate_modified = true;
}
void PlatformDiscover(const GlobalObjectArray* objects) {
    const int32_t end = (std::min)(objects->num_elements, s_scan_cursor + kScanBatch);
    ColourWorkBudget budget(700);
    int examined = 0;
    for (; s_scan_cursor < end; ++s_scan_cursor) {
        if ((examined & 31) == 0 && budget.expired()) break;
        ++examined;
        const GlobalObjectItem& item = objects->objects[s_scan_cursor];
        if (!item.object || (static_cast<uint32_t>(item.flags) & 0x30000000u) ||
            !NamedPlatform(item.object)) continue;
        const auto exists = std::find_if(s_targets.begin(), s_targets.end(),
            [&](const Target& t) { return t.actor == item.object &&
                                          t.serial == item.serial_number; });
        if (exists != s_targets.end()) continue;
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
            s_targets.empty() || Config().speed_percent != 100 ? 5000 : 37000, s_next_scan);
        if (!s_discovery_reported) {
            s_discovery_reported = true;
            Log((L"Platforms: found " + std::to_wstring(s_targets.size()) +
                 L" BP_Lift/BP_Mover actors").c_str());
        }
    }
}
// Replay lift contacts are independent of the cosmetic platform controls.
// A lift's cooked BP_Lift/BP_Mover graph calls AddBasedCharacter and
// RemoveBasedCharacter and drives its own LiftMover timeline.
struct ReplayLiftContact {
    void* actor = nullptr;
    int32_t registry_index = -1;
    int32_t serial = 0;
    bool based = false;
    ghost_replay_tile_contact::Bounds bounds{};
};
struct ReplayLiftRuntime {
    MyCharacter* player = nullptr;
    int resets = -1;
    int scan_cursor = 0;
    int count = 0;
    int contacts_started = 0;
    int invalid_bounds = 0;
    ULONGLONG start_ms = 0;
    void* bounds_fn = nullptr;
    int origin_offset = -1, extent_offset = -1;
    bool ready = false;
    bool unavailable = false;
    std::array<ReplayLiftContact, kMaximumTargets> entries{};
};
ReplayLiftRuntime s_replay_lifts{};

bool ReplayLiftValid(const ReplayLiftContact& lift,
                     const GlobalObjectArray* objects) {
    return lift.actor && objects && lift.registry_index >= 0 &&
        lift.registry_index < objects->num_elements &&
        objects->objects[lift.registry_index].object == lift.actor &&
        objects->objects[lift.registry_index].serial_number == lift.serial &&
        IsReadable(lift.actor, sizeof(UObjectHeader));
}

bool ReadReplayLiftBounds(void* actor,
                          ghost_replay_tile_contact::Bounds& result) {
    const auto& cache = s_replay_lifts;
    if (!g_process_event || !IsReadable(actor,sizeof(UObjectHeader)) ||
        !cache.bounds_fn || cache.origin_offset < 0 || cache.extent_offset < 0)
        return false;
    alignas(16) std::array<uint8_t,512> args{};
    g_process_event(actor,cache.bounds_fn,args.data());
    std::memcpy(&result.center,args.data()+cache.origin_offset,sizeof(Vec3));
    std::memcpy(&result.extent,args.data()+cache.extent_offset,sizeof(Vec3));
    return ghost_replay_tile_contact::Valid(result);
}

} // namespace

bool GhostReplayLiftsChanged() { return s_replay_lifts.contacts_started > 0; }

void ResetGhostReplayLifts() {
    // Called when replay stops or rewinds. Remove only OUR live contacts.
    // Never dispatch into stale UObjects after the map has restarted.
    auto& replay=s_replay_lifts;
    const LevelState* state=PracticeLevelState();
    const auto* objects=Objects();
    if (replay.contacts_started > 0)
        Log((L"Ghost replay: triggered " + std::to_wstring(replay.contacts_started) +
             L" lift/mover contacts").c_str());
    if (replay.player && replay.player == g_player &&
        IsReadable(replay.player,sizeof(MyCharacter)) &&
        state && state->resets == replay.resets &&
        g_remove_based_character && !g_restart_requested &&
        !g_full_level_reload_requested) {
        for (int i=0;i<replay.count;++i) {
            auto& item=replay.entries[i];
            if (item.based && ReplayLiftValid(item,objects))
                g_remove_based_character(item.actor,replay.player);
        }
    }
    replay={};
}

GhostReplayLiftStatus PrepareGhostReplayLifts(MyCharacter* player) {
    auto& replay=s_replay_lifts;
    const LevelState* state=PracticeLevelState();
    if (!player || !state || !g_process_event ||
        !(g_original_add_based_character || g_add_based_character) ||
        !g_remove_based_character)
        return GhostReplayLiftStatus::Unavailable;
    if (replay.player != player || replay.resets != state->resets) {
        ResetGhostReplayLifts();
        replay.player=player;
        replay.resets=state->resets;
        replay.start_ms=GetTickCount64();
    }
    if (replay.unavailable) return GhostReplayLiftStatus::Unavailable;
    if (replay.ready) return GhostReplayLiftStatus::Ready;
    const auto* objects=Objects();
    if (!objects) return GhostReplayLiftStatus::Pending;
    if (GetTickCount64()-replay.start_ms>12000) {
        replay.unavailable=true;
        Log(L"Ghost replay: lift actor discovery timed out; buttons/tiles still replay");
        return GhostReplayLiftStatus::Unavailable;
    }
    // Scan bounded registry slices, with time budget. No object discovery
    // or filesystem operations in the native contact callback.
    ColourWorkBudget budget(750);
    int visited=0;
    for (;replay.scan_cursor<objects->num_elements && visited<1024;
         ++replay.scan_cursor,++visited) {
        if ((visited & 63)==0 && visited && budget.expired()) break;
        const auto& entry=objects->objects[replay.scan_cursor];
        if (!entry.object || !NamedPlatform(entry.object) ||
            !InheritsClass(entry.object,L"LiftBase")) continue;
        if (replay.count==static_cast<int>(replay.entries.size())) break;
        if (!replay.bounds_fn) {
            replay.bounds_fn=FindReflectedFunction(entry.object,L"GetActorBounds");
            if (!replay.bounds_fn ||
                (replay.origin_offset=PlatformParam(replay.bounds_fn,L"Origin",L"StructProperty",512))<0 ||
                (replay.extent_offset=PlatformParam(replay.bounds_fn,L"BoxExtent",L"StructProperty",512))<0) {
                replay.unavailable=true;
                Log(L"Ghost replay: lift GetActorBounds ABI unavailable");
                return GhostReplayLiftStatus::Unavailable;
            }
        }
        ReplayLiftContact item{};
        item.actor=entry.object;
        item.registry_index=replay.scan_cursor;
        item.serial=entry.serial_number;
        if (!ReadReplayLiftBounds(item.actor,item.bounds)) {
            ++replay.invalid_bounds;
            continue;
        }
        replay.entries[replay.count++]=item;
    }
    if (replay.scan_cursor<objects->num_elements &&
        replay.count<static_cast<int>(replay.entries.size()))
        return GhostReplayLiftStatus::Pending;
    replay.ready=true;
    Log((L"Ghost replay: cached " + std::to_wstring(replay.count) +
         L" BP_Lift/BP_Mover platforms for native contact playback (" +
         std::to_wstring(replay.invalid_bounds) +
         L" skipped: invalid bounds)").c_str());
    return GhostReplayLiftStatus::Ready;
}

void UpdateGhostReplayLifts(MyCharacter* player,const Vec3& previous,
                            const Vec3& current,bool continuous) {
    auto& replay=s_replay_lifts;
    const LevelState* state=PracticeLevelState();
    const auto* objects=Objects();
    if (!replay.ready || player!=replay.player || !state ||
        state->resets!=replay.resets || !objects ||
        !g_remove_based_character) return;
    auto add=g_original_add_based_character ?
        g_original_add_based_character : g_add_based_character;
    if (!add) return;
    using ghost_replay_tile_contact::Point;
    const Point from=continuous ? Point{previous.x,previous.y,previous.z} :
                                   Point{current.x,current.y,current.z};
    const Point to{current.x,current.y,current.z};
    int bounds_checks=0,transitions=0;
    for (int i=0;i<replay.count;++i) {
        auto& item=replay.entries[i];
        if (!ReplayLiftValid(item,objects)) {item.based=false;continue;}
        // A previously entered lift still needs its current position checked
        // so it can continue rising while the recorded ghost rides upward.
        if (!item.based &&
            !ghost_replay_tile_contact::CrossedXY(from,to,item.bounds)) continue;
        if (++bounds_checks>12) break;
        ghost_replay_tile_contact::Bounds live{};
        if (!ReadReplayLiftBounds(item.actor,live)) continue;
        item.bounds=live;
        const bool touching=ghost_replay_tile_contact::Crossed(from,to,live);
        // Release as soon as the recorded ghost steps off; avoid keeping the
        // physical player based on this platform after spectator movement.
        if (item.based && !touching) {
            if (++transitions>4) break;
            g_remove_based_character(item.actor,player);
            item.based=false;
        } else if (!item.based && touching) {
            if (++transitions>4) break;
            add(item.actor,player);
            item.based=true;
            ++replay.contacts_started;
        }
    }
}

bool IsPlatformCustomColourEnabled() { return Config().custom_colour; }
void SetPlatformCustomColourEnabled(bool enabled) {
    if (Config().custom_colour == enabled) return;
    Config().custom_colour = enabled;
    s_restore_pending = !enabled;
    Changed(enabled ? L"Platform colours: Custom" : L"Platform colours: Game default", false);
}
std::wstring GetPlatformCustomColourHex() { return PlatformHex(Config().rgb); }
void SetPlatformCustomColourHex(uint32_t rgb) {
    if (rgb > 0xFFFFFFu || Config().rgb == rgb) return;
    Config().rgb = rgb;
    for (auto& t : s_targets) t.colour_unsupported = false;
    Changed(L"Platform colour: " + PlatformHex(rgb), false);
}
void OpenPlatformColourPicker() {
    if (s_picker_open.exchange(true, std::memory_order_acq_rel)) {
        Notify(L"Platform colour picker already open");
        return;
    }
    s_picker_initial.store(Config().rgb, std::memory_order_relaxed);
    s_picker_result.store(kNoPickerResult, std::memory_order_release);
    HANDLE thread = CreateThread(nullptr, 0, PlatformPickerThread, nullptr, 0, nullptr);
    if (!thread) {
        s_picker_open.store(false, std::memory_order_release);
        Notify(L"Could not open platform colour picker");
    } else CloseHandle(thread);
}
void SetPlatformSpeedProfilePercent(int percent) {
    if (percent < 25 || Config().speed_percent == percent) return;
    Config().speed_percent = percent;
    Changed(L"Platform speed: " + std::to_wstring(percent) + L"%", true);
}
int GetPlatformSpeedPercent() { return Config().speed_percent; }
bool IsPlatformSpeedModified() { return GetPlatformSpeedPercent() != 100; }
void AdjustPlatformSpeed(int direction) {
    if (!direction) return;
    const int current = Config().speed_percent;
    constexpr int step = 25;
    const int next = direction < 0
        ? (std::max)(25, current - step)
        : (current <= (std::numeric_limits<int>::max)() - step ? current + step : current);
    if (current == next) return;
    Config().speed_percent = next;
    Changed(L"Platform speed: " + std::to_wstring(next) + L"%", true);
}
void ResetPlatformSpeed() {
    if (Config().speed_percent == 100) return;
    Config().speed_percent = 100;
    Changed(L"Platform speed: 100% (normal)", true);
}
void UpdatePlatforms(MyCharacter* player) {
    FlushDeferredPlatformSave();
    const uint32_t picker = s_picker_result.exchange(kNoPickerResult,
                                                     std::memory_order_acq_rel);
    if (picker <= 0xFFFFFFu) SetPlatformCustomColourHex(picker);
    if (!player || g_restart_requested || g_full_level_reload_requested) return;
    const auto& settings = Config();
    if (!settings.custom_colour && settings.speed_percent == 100 &&
        !s_restore_pending && s_targets.empty()) return;
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
        s_next_speed_check = 0;
    }
    const ULONGLONG now = GetTickCount64();
    const auto* objects = Objects();
    if (!objects) return;
    if (s_scan_cursor < 0 &&
        (settings.custom_colour || settings.speed_percent != 100))
        s_scan_cursor = s_discovery_watermark.Resume(objects->objects, objects->num_elements, now, s_next_scan);
    // Discovery and colour application have separate clocks. The old scan
    // did only 256 objects each 50 ms, causing several seconds of latency.
    if (s_scan_cursor >= 0 && (settings.custom_colour || settings.speed_percent != 100))
        PlatformDiscover(objects);
    else if (!settings.custom_colour && settings.speed_percent == 100)
        s_scan_cursor = -1; // Don't keep discovering after Game Default.
    if (now < s_next_update && s_pending_colour == 0) return;
    const bool urgent = s_pending_colour != 0;
    s_next_update = now + (urgent ? 0 : kUpdateMs);
    const bool check_speed = now >= s_next_speed_check;
    if (check_speed) s_next_speed_check = now + kUpdateMs;
    if (s_targets.empty()) return;
    ColourWorkBudget apply_budget(1100);
    size_t checked = 0;
    const size_t count = s_targets.size();
    const size_t batch = urgent ? 5 : static_cast<size_t>(kApplyBatch);
    while (checked < (std::min)(batch, count)) {
        if (checked > 0 && apply_budget.expired()) break;
        ++checked;
        if (s_apply_cursor >= s_targets.size()) s_apply_cursor = 0;
        Target& target = s_targets[s_apply_cursor++];
        if (s_pending_colour) --s_pending_colour;
        if (!Valid(target, objects)) { target = {}; s_next_scan = 0; continue; }
        if (settings.custom_colour || target.material_instance)
            ColourTarget(target, settings.custom_colour, settings.rgb);
        // Physics update is independent of cosmetic work scheduling.
        if (check_speed && (settings.speed_percent != 100 || target.rate_modified))
            SpeedTarget(target, settings.speed_percent);
    }
    // Keep only live targets, including targets undergoing restoration.
    s_targets.erase(std::remove_if(s_targets.begin(), s_targets.end(),
        [](const Target& target) { return target.actor == nullptr; }), s_targets.end());
    if (s_apply_cursor >= s_targets.size()) s_apply_cursor = 0;
    if (!settings.custom_colour && settings.speed_percent == 100) {
        s_restore_pending = std::any_of(s_targets.begin(), s_targets.end(),
            [](const Target& t) { return t.material_instance || t.rate_modified; });
        if (!s_restore_pending) {
            s_targets.clear();
            s_scan_cursor = -1;
        }
    }
}
} // namespace refunct

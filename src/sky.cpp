#include "refunct/internal_api.hpp"
#include <commdlg.h>

namespace refunct {
namespace {
// The shipped WorldBase map instantiates BP_DynamicSky_C. Its SkyMaterial is
// a MID of /Engine/EngineSky/M_Sky_Panning_Clouds2, with vector parameters
// "Zenith Color", "Horizon color", "Sun color", and "Cloud color"
// (verified in the cooked PAK).
// The cooked material exposes "Sun brightness" for the sky disc, plus
// "Cloud opacity" and "Stars brightness" as independent scalar parameters.
// Verified in Engine/Content/EngineSky/M_Sky_Panning_Clouds2.uasset.
// Note: the material parameter must also be available as a native runtime
// FName override. If unavailable, log and leave the original sky untouched.
// There is no verified separate moon-disc material parameter, so the single
// visibility option controls this shared disc regardless of time of day.
// Never tint the entire scene, edit a shared material, or guess UObject RVAs.
constexpr wchar_t kSection[] = L"SkyColours";
constexpr size_t kSkyRegionCount = 4;
// Keep the legacy keys for migrating settings from the two-toggle version.
constexpr wchar_t kDiscVisibleKey[] = L"SunMoonVisible";
constexpr wchar_t kLegacySunVisibleKey[] = L"SunDiscVisible";
constexpr wchar_t kLegacyMoonVisibleKey[] = L"MoonDiscVisible";
constexpr wchar_t kDiscBrightnessParameter[] = L"Sun brightness";
constexpr size_t kSkyEffectCount = 2;
constexpr const wchar_t* kEffectParameterNames[kSkyEffectCount] = {
    L"Cloud opacity", L"Stars brightness"
};
constexpr const wchar_t* kEffectKeys[kSkyEffectCount] = {
    L"CloudsVisible", L"StarsVisible"
};
constexpr const wchar_t* kEffectLabels[kSkyEffectCount] = {
    L"Clouds", L"Stars"
};
constexpr const wchar_t* kParameterNames[kSkyRegionCount] = {
    L"Zenith Color", L"Horizon color", L"Sun color", L"Cloud color"
};
constexpr const wchar_t* kEnabledKeys[kSkyRegionCount] = {
    L"ZenithEnabled", L"HorizonEnabled", L"SunEnabled", L"CloudEnabled"
};
constexpr const wchar_t* kHexKeys[kSkyRegionCount] = {
    L"ZenithHex", L"HorizonHex", L"SunHex", L"CloudHex"
};
constexpr const wchar_t* kDefaultHex[kSkyRegionCount] = {
    L"75BFFF", L"D7A9E8", L"FFF1B3", L"FFFFFF"
};
constexpr const wchar_t* kSkyRegionLabels[kSkyRegionCount] = {
    L"Zenith", L"Horizon", L"Sun", L"Clouds"
};
constexpr uint32_t kNoPickerResult = 0xFFFFFFFFu;
constexpr uint32_t kPickerCancelled = 0xFFFFFFFEu;
constexpr uint32_t kPickerError = 0xFFFFFFFDu;
constexpr ULONGLONG kSaveDebounce = 350;
constexpr ULONGLONG kScanBudgetMs = 2;
constexpr int32_t kScanBatch = 1024;

struct SkySettings {
    bool loaded = false;
    std::array<bool, kSkyRegionCount> enabled{};
    bool disc_visible = true;
    std::array<bool, kSkyEffectCount> effect_visible{{true, true}};
    std::array<uint32_t, kSkyRegionCount> colour{0x75BFFFu, 0xD7A9E8u, 0xFFF1B3u, 0xFFFFFFu};
    bool save_pending = false;
    ULONGLONG save_at = 0;
} s_settings;

struct SkyValue {
    FNameView name{};
    bool name_known = false;
    LinearColor original{};
    LinearColor last_written{};
    bool modified = false;
    uint32_t desired_rgb = 0xFFFFFFFFu;
    LinearColor desired_colour{};
};
struct SkyScalar {
    FNameView name{};
    bool name_known = false;
    float original = 0.0f;
    float last_written = 0.0f;
    bool modified = false;
    bool has_original = false; // Only restore a genuine non-zero brightness.
};
struct SkyMaterial {
    void* actor = nullptr;
    int32_t actor_index = -1;
    int32_t actor_serial = 0;
    int32_t material_offset = -1;
    void* mid = nullptr;
    int32_t mid_index = -1;
    int32_t mid_serial = 0;
    std::array<SkyValue, kSkyRegionCount> values{};
    SkyScalar sun_brightness{};
    std::array<SkyScalar, kSkyEffectCount> effects{};
} s_target;

void* s_name_library = nullptr;
int32_t s_scan_cursor = 0;
ULONGLONG s_rescan_at = 0;
bool s_logged_ready = false;
bool s_logged_unavailable = false;
// Emit one diagnostic per region/stage rather than silently retrying each tick.
// Values are reset when the world replaces the sky material.
std::array<int, kSkyRegionCount> s_diagnostic_stage{{-1, -1, -1, -1}};
int s_disc_diagnostic_stage = -1;
ULONGLONG s_disc_retry_after = 0;
std::array<ULONGLONG, kSkyEffectCount> s_effect_retry_after{};
std::array<int, kSkyEffectCount> s_effect_stage{{-1, -1}};
void EffectStage(size_t index, int stage, const wchar_t* message) {
    if (index >= kSkyEffectCount || s_effect_stage[index] == stage) return;
    s_effect_stage[index] = stage;
    Log((std::wstring(L"Sky ") + kEffectLabels[index] + L": " + message).c_str());
}
void DiscStage(int stage, const wchar_t* message) {
    if (stage == s_disc_diagnostic_stage) return;
    s_disc_diagnostic_stage = stage;
    Log((std::wstring(L"Sky discs: ") + message).c_str());
}
std::array<ULONGLONG, kSkyRegionCount> s_native_name_retry_after{};
void SkyStage(size_t region, int stage, const wchar_t* detail) {
    if (region >= kSkyRegionCount || s_diagnostic_stage[region] == stage) return;
    s_diagnostic_stage[region] = stage;
    const std::wstring line = std::wstring(L"Sky colours: ") +
        kSkyRegionLabels[region] + L": " + detail;
    Log(line.c_str());
}

std::atomic_bool s_picker_open{false};
std::atomic_uint32_t s_picker_initial{0};
std::atomic_uint32_t s_picker_result{kNoPickerResult};
std::atomic_int s_picker_region{0};

std::wstring SkyName(const void* object) {
    return IsReadable(object, sizeof(UObjectHeader))
        ? UnrealName(static_cast<const UObjectHeader*>(object)->name) : L"";
}
bool SkyIsA(const void* object, const wchar_t* class_name) {
    if (!IsReadable(object, sizeof(UObjectHeader))) return false;
    void* cls = static_cast<const UObjectHeader*>(object)->class_ptr;
    for (int depth = 0; depth < 12 && IsReadable(cls, sizeof(UStructHeader)); ++depth) {
        if (SkyName(cls) == class_name) return true;
        cls = static_cast<const UStructHeader*>(cls)->super_struct;
    }
    return false;
}
bool SkyField(void* function, const wchar_t* property, const wchar_t* kind,
              int32_t& offset, size_t bytes) {
    if (!IsReadable(function, sizeof(UStructHeader))) return false;
    void* field = static_cast<const UStructHeader*>(function)->children;
    for (int i = 0; i < 64 && IsReadable(field, sizeof(UFieldHeader)); ++i) {
        const auto* header = static_cast<const UFieldHeader*>(field);
        if (SkyName(field) == property && SkyName(header->object.class_ptr) == kind) {
            const int32_t result = FindClassPropertyOffset(function, property);
            if (result < 0 || result > 128 - static_cast<int32_t>(bytes)) return false;
            offset = result;
            return true;
        }
        field = header->next;
    }
    return false;
}
const GlobalObjectArray* SkyObjects() {
    if (!g_game_module) return nullptr;
    const auto* array = reinterpret_cast<const GlobalObjectArray*>(
        reinterpret_cast<uintptr_t>(g_game_module) + kGlobalObjectArrayRva);
    if (!IsReadable(array, sizeof(*array)) || !array->objects ||
        array->num_elements < 0 || array->num_elements > 1000000 ||
        array->max_elements < array->num_elements ||
        !IsReadable(array->objects, static_cast<size_t>(array->num_elements) *
                                   sizeof(GlobalObjectItem))) return nullptr;
    return array;
}
bool Live(const GlobalObjectArray* objects, const void* object,
          int32_t index, int32_t serial) {
    return objects && index >= 0 && index < objects->num_elements &&
        objects->objects[index].object == object &&
        objects->objects[index].serial_number == serial &&
        IsReadable(object, sizeof(UObjectHeader)) &&
        (static_cast<uint32_t>(static_cast<const UObjectHeader*>(object)->flags) & 0x30u) == 0;
}
void ResetTarget() {
    s_target = {};
    s_logged_ready = false;
    s_diagnostic_stage.fill(-1);
    s_disc_diagnostic_stage = -1;
    s_disc_retry_after = 0;
    s_effect_retry_after = {};
    s_effect_stage.fill(-1);
    s_native_name_retry_after = {};
}
// A live sky MID must inherit from the expected engine sky material.
bool IsVerifiedSkyMID(void* mid) {
    if (!SkyIsA(mid, L"MaterialInstanceDynamic")) return false;
    void* current = mid;
    for (int i = 0; i < 6 && IsReadable(current, sizeof(UObjectHeader)); ++i) {
        if (SkyName(current) == L"M_Sky_Panning_Clouds2") return true;
        if (!SkyIsA(current, L"MaterialInstance")) break;
        void* parent = ReadReflectedObject(current, L"Parent");
        if (!parent || parent == current) break;
        current = parent;
    }
    return false;
}
bool TargetLive(const GlobalObjectArray* objects) {
    if (!Live(objects, s_target.actor, s_target.actor_index, s_target.actor_serial) ||
        !Live(objects, s_target.mid, s_target.mid_index, s_target.mid_serial) ||
        s_target.material_offset < 0) return false;
    const auto* field = static_cast<const uint8_t*>(s_target.actor) + s_target.material_offset;
    void* current = nullptr;
    if (!IsReadable(field, sizeof(current))) return false;
    std::memcpy(&current, field, sizeof(current));
    return current == s_target.mid;
}
void FindSky(const GlobalObjectArray* objects) {
    if (!objects || s_scan_cursor < 0) return;
    const int32_t end = (std::min)(s_scan_cursor + kScanBatch, objects->num_elements);
    const ULONGLONG start = GetTickCount64();
    for (; s_scan_cursor < end; ++s_scan_cursor) {
        if ((s_scan_cursor & 127) == 0 && GetTickCount64() - start > kScanBudgetMs) break;
        const auto& item = objects->objects[s_scan_cursor];
        if (!IsReadable(item.object, sizeof(UObjectHeader)) ||
            (static_cast<uint32_t>(item.flags) & 0x30000000u) != 0) continue;
        if (!s_name_library && SkyName(item.object) == L"Default__KismetStringLibrary" &&
            SkyIsA(item.object, L"KismetStringLibrary"))
            s_name_library = item.object;
        if (s_target.actor || (SkyName(item.object) != L"BP_DynamicSky_C" &&
            !SkyIsA(item.object, L"BP_DynamicSky_C"))) continue;
        if ((static_cast<uint32_t>(item.object->flags) & 0x30u) != 0) continue;
        void* mid = ReadReflectedObject(item.object, L"SkyMaterial");
        if (!mid || !IsVerifiedSkyMID(mid)) continue;
        // Cache only a verified reflected ObjectProperty offset; avoid
        // repeating name reflection work on every game tick.
        void* cls = item.object->class_ptr;
        int32_t material_offset = FindClassPropertyOffset(cls, L"SkyMaterial");
        if (material_offset < 0 || material_offset > 0x3FFC ||
            !IsReadable(reinterpret_cast<const uint8_t*>(item.object) + material_offset,
                        sizeof(void*))) continue;
        const auto* mid_header = static_cast<const UObjectHeader*>(mid);
        const int32_t mid_index = mid_header->index;
        if (mid_index < 0 || mid_index >= objects->num_elements ||
            objects->objects[mid_index].object != mid) continue;
        s_target.actor = item.object;
        s_target.actor_index = s_scan_cursor;
        s_target.actor_serial = item.serial_number;
        s_target.material_offset = material_offset;
        s_target.mid = mid;
        s_target.mid_index = mid_index;
        s_target.mid_serial = objects->objects[mid_index].serial_number;
        if (!s_logged_ready) {
            Log(L"Sky colours: verified BP_DynamicSky sky MID (Zenith/Horizon/Sun)");
            s_logged_ready = true;
        }
    }
    if (s_scan_cursor >= objects->num_elements) {
        s_scan_cursor = -1;
        s_rescan_at = GetTickCount64() + 3000;
        if (!s_target.actor && !s_logged_unavailable) {
            Log(L"Sky colours: BP_DynamicSky SkyMaterial MID not found; will retry");
            s_logged_unavailable = true;
        } else if (s_target.actor && !s_name_library && !s_logged_unavailable) {
            Log(L"Sky colours: FName conversion library not ready; will retry");
            s_logged_unavailable = true;
        }
    }
}
// Refunct's legacy FVectorParameterValue layout is FName (8 bytes),
// FLinearColor (16 bytes), FGuid (16 bytes). BP_DynamicSky writes its
// runtime overrides into the sky MID, so obtain the *actual* FName rather
// than depending entirely on KismetStringLibrary's versioned UFunction ABI.
// Cooked PAK name-map indices are not executable FName indices.
constexpr size_t kSkyVectorEntrySize = sizeof(FNameView) + sizeof(LinearColor) + 16;
static_assert(kSkyVectorEntrySize == 40);

bool SkyVectorValuesOffset(void* instance, int32_t& offset) {
    offset = -1;
    if (!SkyIsA(instance, L"MaterialInstance")) return false;
    void* cls = static_cast<const UObjectHeader*>(instance)->class_ptr;
    for (int depth = 0; depth < 16 && IsReadable(cls, sizeof(UStructHeader)); ++depth) {
        void* field = static_cast<const UStructHeader*>(cls)->children;
        for (int i = 0; i < 2048 && IsReadable(field, sizeof(UFieldHeader)); ++i) {
            const auto* property = static_cast<const UFieldHeader*>(field);
            if (SkyName(field) == L"VectorParameterValues") {
                if (SkyName(property->object.class_ptr) != L"ArrayProperty") return false;
                const int32_t candidate = FindClassPropertyOffset(cls, L"VectorParameterValues");
                if (candidate < 0 || candidate > 0x800) return false;
                offset = candidate;
                return true;
            }
            field = property->next;
        }
        cls = static_cast<const UStructHeader*>(cls)->super_struct;
    }
    return false;
}

bool ResolveNameFromSkyMID(SkyValue& value, void* mid,
                           const wchar_t* name, size_t region) {
    // Only inspect the already-verified sky MID and its MaterialInstance parents.
    // A parent material may supply an override even if the MID has none yet.
    void* instance = mid;
    for (int depth = 0; depth < 6 && SkyIsA(instance, L"MaterialInstance"); ++depth) {
        int32_t offset = -1;
        if (SkyVectorValuesOffset(instance, offset)) {
            const auto* address = static_cast<const uint8_t*>(instance) + offset;
            if (IsReadable(address, sizeof(UObjectPointerArray))) {
                UObjectPointerArray entries{};
                std::memcpy(&entries, address, sizeof(entries));
                if (entries.count > 0 && entries.count <= 32 &&
                    entries.capacity >= entries.count && entries.capacity <= 512 &&
                    IsReadable(entries.data,
                               static_cast<size_t>(entries.count) * kSkyVectorEntrySize)) {
                    for (int32_t i = 0; i < entries.count; ++i) {
                        FNameView candidate{};
                        const auto* entry = reinterpret_cast<const uint8_t*>(entries.data) +
                            static_cast<size_t>(i) * kSkyVectorEntrySize;
                        std::memcpy(&candidate, entry, sizeof(candidate));
                        // Never pass arbitrary array bytes into UE4's FName resolver.
                        if (candidate.index == 0 || candidate.index >= 65536 ||
                            candidate.number > 255) continue;
                        const std::wstring actual = UnrealName(candidate);
                        if (_wcsicmp(actual.c_str(), name) != 0) continue;
                        value.name = candidate;
                        value.name_known = true;
                        SkyStage(region, 13, L"resolved native sky MID vector parameter");
                        return true;
                    }
                }
            }
        }
        void* parent = ReadReflectedObject(instance, L"Parent");
        if (!parent || parent == instance) break;
        instance = parent;
    }
    return false;
}

// Refunct's 32-bit legacy FScalarParameterValue stores FName (8), float
// (4), and FGuid (16) in the MaterialInstance ScalarParameterValues array.
constexpr size_t kSkyScalarEntrySize = sizeof(FNameView) + sizeof(float) + 16;
static_assert(kSkyScalarEntrySize == 28);

bool SkyScalarValuesOffset(void* instance, int32_t& offset) {
    offset = -1;
    if (!SkyIsA(instance, L"MaterialInstance")) return false;
    void* cls = static_cast<const UObjectHeader*>(instance)->class_ptr;
    for (int depth = 0; depth < 16 && IsReadable(cls, sizeof(UStructHeader)); ++depth) {
        void* field = static_cast<const UStructHeader*>(cls)->children;
        for (int i = 0; i < 2048 && IsReadable(field, sizeof(UFieldHeader)); ++i) {
            const auto* property = static_cast<const UFieldHeader*>(field);
            if (SkyName(field) == L"ScalarParameterValues") {
                if (SkyName(property->object.class_ptr) != L"ArrayProperty") return false;
                const int32_t found = FindClassPropertyOffset(cls, L"ScalarParameterValues");
                if (found < 0 || found > 0x800) return false;
                offset = found;
                return true;
            }
            field = property->next;
        }
        cls = static_cast<const UStructHeader*>(cls)->super_struct;
    }
    return false;
}

bool ResolveScalarNameFromMID(void* mid, SkyScalar& scalar,
                              const wchar_t* parameter) {
    if (scalar.name_known) return true;
    void* instance = mid;
    for (int depth = 0; depth < 6 && SkyIsA(instance, L"MaterialInstance"); ++depth) {
        int32_t offset = -1;
        if (SkyScalarValuesOffset(instance, offset)) {
            const auto* address = static_cast<const uint8_t*>(instance) + offset;
            if (IsReadable(address, sizeof(UObjectPointerArray))) {
                UObjectPointerArray entries{};
                std::memcpy(&entries, address, sizeof(entries));
                if (entries.count > 0 && entries.count <= 64 &&
                    entries.capacity >= entries.count && entries.capacity <= 512 &&
                    IsReadable(entries.data,
                        static_cast<size_t>(entries.count) * kSkyScalarEntrySize)) {
                    for (int32_t i = 0; i < entries.count; ++i) {
                        const auto* data = reinterpret_cast<const uint8_t*>(entries.data) +
                            static_cast<size_t>(i) * kSkyScalarEntrySize;
                        FNameView candidate{};
                        float default_value = 0.0f;
                        std::memcpy(&candidate, data, sizeof(candidate));
                        std::memcpy(&default_value, data + sizeof(candidate),
                                    sizeof(default_value));
                        if (candidate.index == 0 || candidate.index >= 65536 ||
                            candidate.number > 255 || !std::isfinite(default_value)) continue;
                        if (_wcsicmp(UnrealName(candidate).c_str(), parameter) != 0) continue;
                        scalar.name = candidate;
                        scalar.name_known = true;
                        return true;
                    }
                }
            }
        }
        void* parent = ReadReflectedObject(instance, L"Parent");
        if (!parent || parent == instance) break;
        instance = parent;
    }
    return false;
}

// Some cooked scalar parameters are defined by M_Sky_Panning_Clouds2 but
// not pre-populated in the dynamic instance's override array. Prefer native
// array lookup; use an independently validated reflected conversion only
// when no native FName exists. Never copy FName indices from the cooked PAK.
bool ResolveEffectScalarName(void* mid, SkyScalar& scalar, const wchar_t* parameter) {
    if (ResolveScalarNameFromMID(mid, scalar, parameter)) return true;
    if (!g_process_event || !IsReadable(s_name_library, sizeof(UObjectHeader)) ||
        !SkyIsA(s_name_library, L"KismetStringLibrary")) return false;
    void* fn = FindReflectedFunction(s_name_library, L"Conv_StringToName");
    if (!fn) return false;
    int32_t in = -1, out = -1;
    const bool in_valid =
        SkyField(fn, L"InString", L"StrProperty", in, sizeof(FStringView)) ||
        SkyField(fn, L"InString", L"StringProperty", in, sizeof(FStringView));
    const bool out_valid =
        SkyField(fn, L"ReturnValue", L"NameProperty", out, sizeof(FNameView)) ||
        SkyField(fn, L"ReturnValue", L"FNameProperty", out, sizeof(FNameView));
    if (!in_valid || !out_valid ||
        (in < out + static_cast<int32_t>(sizeof(FNameView)) &&
         out < in + static_cast<int32_t>(sizeof(FStringView)))) return false;
    const int32_t chars = static_cast<int32_t>(wcslen(parameter) + 1);
    FStringView value{const_cast<wchar_t*>(parameter), chars, chars};
    alignas(16) std::array<uint8_t, 128> args{};
    std::memcpy(args.data() + in, &value, sizeof(value));
    g_process_event(s_name_library, fn, args.data());
    FNameView name{};
    std::memcpy(&name, args.data() + out, sizeof(name));
    if (name.index == 0 || _wcsicmp(UnrealName(name).c_str(), parameter) != 0)
        return false;
    scalar.name = name;
    scalar.name_known = true;
    return true;
}

struct SkyScalarMethod {
    void* klass = nullptr;
    void* function = nullptr;
    int32_t name = -1;
    int32_t value = -1;
};
bool ResolveScalarMethod(void* mid, SkyScalarMethod& method, bool getter) {
    if (!IsReadable(mid, sizeof(UObjectHeader))) return false;
    void* cls = static_cast<const UObjectHeader*>(mid)->class_ptr;
    if (method.klass == cls && method.name >= 0 && method.value >= 0 &&
        IsReadable(method.function, sizeof(UFieldHeader))) return true;
    method = {};
    void* fn = FindReflectedFunction(mid, getter ? L"K2_GetScalarParameterValue" :
                                                 L"SetScalarParameterValue");
    if (!fn) fn = FindReflectedFunction(mid, getter ? L"GetScalarParameterValue" :
                                                      L"K2_SetScalarParameterValue");
    int32_t name_offset = -1, value_offset = -1;
    if (!SkyField(fn, L"ParameterName", L"NameProperty", name_offset,
                  sizeof(FNameView))) return false;
    if (getter) {
        if (!SkyField(fn, L"ReturnValue", L"FloatProperty", value_offset,
                      sizeof(float))) return false;
    } else if (!SkyField(fn, L"Value", L"FloatProperty", value_offset, sizeof(float)) &&
               !SkyField(fn, L"ParameterValue", L"FloatProperty", value_offset,
                         sizeof(float))) return false;
    method = {cls, fn, name_offset, value_offset};
    return true;
}
bool GetMIDScalar(void* mid, FNameView name, float& result) {
    if (!g_process_event) return false;
    static SkyScalarMethod getter{}; // The only caller is the game-tick thread.
    if (!ResolveScalarMethod(mid, getter, true)) return false;
    alignas(16) std::array<uint8_t, 128> args{};
    std::memcpy(args.data() + getter.name, &name, sizeof(name));
    g_process_event(mid, getter.function, args.data());
    std::memcpy(&result, args.data() + getter.value, sizeof(result));
    return std::isfinite(result) && std::fabs(result) <= 1000000.0f;
}
bool SetMIDScalar(void* mid, FNameView name, float value) {
    if (!g_process_event || !std::isfinite(value)) return false;
    static SkyScalarMethod setter{}; // The only caller is the game-tick thread.
    if (!ResolveScalarMethod(mid, setter, false)) return false;
    alignas(16) std::array<uint8_t, 128> args{};
    std::memcpy(args.data() + setter.name, &name, sizeof(name));
    std::memcpy(args.data() + setter.value, &value, sizeof(value));
    g_process_event(mid, setter.function, args.data());
    return true;
}

// Hidden effects are forced to zero after native sky updates. ON restores the
// captured nonzero value once, then gives control back to BP_DynamicSky.
// This never touches the Sun/Moon scalar or the editable sky colours.
void UpdateSkyEffectVisibility(size_t index) {
    if (index >= kSkyEffectCount) return;
    SkyScalar& scalar = s_target.effects[index];
    if (s_settings.effect_visible[index] && !scalar.modified) return;

    const ULONGLONG now = GetTickCount64();
    if (!scalar.name_known && now >= s_effect_retry_after[index]) {
        s_effect_retry_after[index] = now + 1000;
        ResolveEffectScalarName(s_target.mid, scalar, kEffectParameterNames[index]);
    }
    if (!scalar.name_known) {
        EffectStage(index, 1, L"native scalar parameter not found on sky MID; no changes applied");
        return;
    }

    float current = 0.0f;
    if (!GetMIDScalar(s_target.mid, scalar.name, current)) {
        EffectStage(index, 2, L"scalar getter unavailable; no changes applied");
        return;
    }
    constexpr float kNearZero = 0.001f;
    if (!s_settings.effect_visible[index]) {
        if (std::fabs(current) > kNearZero) {
            // The Blueprint may change brightness across the day/night cycle.
            // Never replace the original with the zero written by this module.
            scalar.original = current;
            scalar.has_original = true;
            if (!SetMIDScalar(s_target.mid, scalar.name, 0.0f)) {
                EffectStage(index, 3, L"scalar setter unavailable; no change applied");
                return;
            }
            scalar.modified = true;
            scalar.last_written = 0.0f;
            float readback = 0.0f;
            if (!GetMIDScalar(s_target.mid, scalar.name, readback) ||
                std::fabs(readback) > kNearZero) {
                EffectStage(index, 4, L"zero write not confirmed; will retry");
                return;
            }
        }
        EffectStage(index, 5, L"effect hidden (visual only)");
        return;
    }

    if (std::fabs(current) <= kNearZero) {
        if (!scalar.has_original) {
            EffectStage(index, 6, L"awaiting a valid native brightness to restore");
            return;
        }
        if (!SetMIDScalar(s_target.mid, scalar.name, scalar.original)) {
            EffectStage(index, 7, L"restore write failed; will retry");
            return;
        }
        float readback = 0.0f;
        if (!GetMIDScalar(s_target.mid, scalar.name, readback) ||
            std::fabs(readback - scalar.original) > kNearZero) {
            EffectStage(index, 8, L"restore read-back failed; will retry");
            return;
        }
    }
    scalar.modified = false;
    EffectStage(index, 9, L"original brightness restored; native control resumed");
}

void UpdateDiscVisibility() {
    auto& brightness = s_target.sun_brightness;
    // When visible and no prior override remains, leave the game's dynamic
    // day/night brightness completely alone.
    if (s_settings.disc_visible && !brightness.modified) return;

    const ULONGLONG now = GetTickCount64();
    if (!brightness.name_known && now >= s_disc_retry_after) {
        s_disc_retry_after = now + 1000;
        ResolveScalarNameFromMID(s_target.mid, brightness, kDiscBrightnessParameter);
    }
    if (!brightness.name_known) {
        DiscStage(1, L"native 'Sun brightness' scalar not found on sky MID");
        return;
    }

    float current = 0.0f;
    if (!GetMIDScalar(s_target.mid, brightness.name, current)) {
        DiscStage(2, L"scalar getter unavailable; no visibility change");
        return;
    }
    constexpr float kNearZero = 0.001f;
    if (!s_settings.disc_visible) {
        // Only snapshot a real brightness: capturing zero (e.g. at night or
        // after our previous write) made the old toggle restore zero forever.
        if (std::fabs(current) > kNearZero) {
            brightness.original = current;
            brightness.has_original = true;
            if (!SetMIDScalar(s_target.mid, brightness.name, 0.0f)) {
                DiscStage(5, L"scalar setter unavailable; disc brightness unchanged");
                return;
            }
            // Even if read-back fails, the setter may have already hidden the
            // disc. Track the write so re-enabling still attempts restoration.
            brightness.modified = true;
            brightness.last_written = 0.0f;
            float readback = 0.0f;
            if (!GetMIDScalar(s_target.mid, brightness.name, readback) ||
                std::fabs(readback) > kNearZero) {
                DiscStage(9, L"zero brightness setter ran, but native read-back did not confirm it");
                return;
            }
        }
        // If the game's native brightness is already zero, do not overwrite
        // a previously saved non-zero original with that zero.
        DiscStage(6, L"Moon/Sun disc brightness suppressed (visual only)");
        return;
    }

    // Re-enable: keep trying until a non-zero native value is confirmed.
    // Previously we cleared `modified` after an attempted setter (including
    // attempts to restore zero), which could make the disc stay invisible.
    if (std::fabs(current) <= kNearZero) {
        if (!brightness.has_original) {
            DiscStage(10, L"awaiting original non-zero brightness before restoration");
            return;
        }
        if (!SetMIDScalar(s_target.mid, brightness.name, brightness.original)) {
            DiscStage(7, L"could not restore original sky disc brightness; will retry");
            return;
        }
        float readback = 0.0f;
        if (!GetMIDScalar(s_target.mid, brightness.name, readback) ||
            std::fabs(readback - brightness.original) > kNearZero) {
            DiscStage(11, L"disc brightness restore not confirmed; will retry");
            return;
        }
    }
    // Either our original value was restored, or the game has already resumed
    // writing a non-zero dynamic value. Both mean the override can be released.
    brightness.modified = false;
    DiscStage(8, L"Moon/Sun disc brightness restored and native control resumed");
}

// Fallback for sky MIDs with no native vector override for the requested name.
// Validate the reflected signature before calling the Kismet conversion.
bool ResolveName(SkyValue& value, void* mid, const wchar_t* name, size_t region) {
    if (value.name_known) return true;
    // An unavailable parameter may be populated by the Blueprint later. Avoid
    // rescanning its entire vector array on every game tick while waiting.
    const ULONGLONG now = GetTickCount64();
    if (region >= s_native_name_retry_after.size() ||
        now < s_native_name_retry_after[region]) return false;
    s_native_name_retry_after[region] = now + 1000;
    if (ResolveNameFromSkyMID(value, mid, name, region)) return true;
    if (!g_process_event) {
        SkyStage(region, 1, L"cannot resolve FName (ProcessEvent unavailable)");
        return false;
    }
    if (!IsReadable(s_name_library, sizeof(UObjectHeader))) {
        SkyStage(region, 2, L"waiting for KismetStringLibrary");
        return false;
    }
    void* fn = FindReflectedFunction(s_name_library, L"Conv_StringToName");
    if (!fn) {
        SkyStage(region, 3, L"Conv_StringToName not found");
        return false;
    }
    int32_t in = -1, out = -1;
    if (!SkyField(fn, L"InString", L"StrProperty", in, sizeof(FStringView)) ||
        !SkyField(fn, L"ReturnValue", L"NameProperty", out, sizeof(FNameView))) {
        SkyStage(region, 4, L"Conv_StringToName argument signature not recognised");
        return false;
    }
    const FStringView string{const_cast<wchar_t*>(name),
                             static_cast<int32_t>(wcslen(name) + 1),
                             static_cast<int32_t>(wcslen(name) + 1)};
    alignas(16) std::array<uint8_t, 128> args{};
    std::memcpy(args.data() + in, &string, sizeof(string));
    g_process_event(s_name_library, fn, args.data());
    std::memcpy(&value.name, args.data() + out, sizeof(value.name));
    const std::wstring runtime_name = UnrealName(value.name);
    // FNames compare case-insensitively, and their stored spelling may differ
    // from the capitalization in the cooked material parameter name.
    value.name_known = value.name.index != 0 &&
        _wcsicmp(runtime_name.c_str(), name) == 0;
    if (!value.name_known) {
        SkyStage(region, 5, L"Conv_StringToName returned a different/invalid name");
        return false;
    }
    SkyStage(region, 6, L"resolved native material parameter name");
    return true;
}

struct SkyMethod {
    void* klass = nullptr;
    void* function = nullptr;
    int32_t name = -1;
    int32_t colour = -1;
};
bool ResolveMethod(void* mid, SkyMethod& method, bool getter) {
    if (!IsReadable(mid, sizeof(UObjectHeader))) return false;
    void* klass = static_cast<const UObjectHeader*>(mid)->class_ptr;
    if (method.klass == klass && method.name >= 0 && method.colour >= 0 &&
        IsReadable(method.function, sizeof(UFieldHeader))) return true;
    method = {};
    void* fn = FindReflectedFunction(mid, getter ? L"K2_GetVectorParameterValue" :
                                                  L"SetVectorParameterValue");
    if (!fn) fn = FindReflectedFunction(mid, getter ? L"GetVectorParameterValue" :
                                                      L"K2_SetVectorParameterValue");
    int32_t param = -1, colour = -1;
    if (!SkyField(fn, L"ParameterName", L"NameProperty", param, sizeof(FNameView)) ||
        !SkyField(fn, getter ? L"ReturnValue" : L"Value", L"StructProperty",
                  colour, sizeof(LinearColor))) return false;
    method = {klass, fn, param, colour};
    return true;
}
bool GetMIDColor(void* mid, FNameView name, LinearColor& result) {
    if (!g_process_event) return false;
    static SkyMethod cached{}; // Only called on the character/game-tick thread.
    if (!ResolveMethod(mid, cached, true)) return false;
    alignas(16) std::array<uint8_t, 128> args{};
    std::memcpy(args.data() + cached.name, &name, sizeof(name));
    g_process_event(mid, cached.function, args.data());
    std::memcpy(&result, args.data() + cached.colour, sizeof(result));
    return std::isfinite(result.r) && std::isfinite(result.g) &&
           std::isfinite(result.b) && std::isfinite(result.a) &&
           result.r >= 0.0f && result.g >= 0.0f && result.b >= 0.0f &&
           result.r <= 100.0f && result.g <= 100.0f && result.b <= 100.0f;
}
bool SetMIDColor(void* mid, FNameView name, const LinearColor& colour) {
    if (!g_process_event) return false;
    static SkyMethod cached{}; // Only called on the character/game-tick thread.
    if (!ResolveMethod(mid, cached, false)) return false;
    alignas(16) std::array<uint8_t, 128> args{};
    std::memcpy(args.data() + cached.name, &name, sizeof(name));
    std::memcpy(args.data() + cached.colour, &colour, sizeof(colour));
    g_process_event(mid, cached.function, args.data());
    return true;
}
LinearColor ColourFromHex(uint32_t rgb) {
    const auto linear = [](uint32_t byte) {
        const float x = static_cast<float>(byte & 255u) / 255.0f;
        return x <= 0.04045f ? x / 12.92f : std::pow((x + 0.055f) / 1.055f, 2.4f);
    };
    return {linear(rgb >> 16), linear(rgb >> 8), linear(rgb), 1.0f};
}
bool SameColour(const LinearColor& a, const LinearColor& b) {
    return std::fabs(a.r - b.r) < 0.002f && std::fabs(a.g - b.g) < 0.002f &&
           std::fabs(a.b - b.b) < 0.002f;
}
SkySettings& Settings() {
    if (s_settings.loaded || g_settings_path.empty()) return s_settings;
    for (size_t i = 0; i < kSkyRegionCount; ++i) {
        s_settings.enabled[i] = GetPrivateProfileIntW(kSection, kEnabledKeys[i], 0,
                                                      g_settings_path.c_str()) != 0;
        wchar_t buffer[16]{};
        GetPrivateProfileStringW(kSection, kHexKeys[i], kDefaultHex[i],
                                buffer, static_cast<DWORD>(std::size(buffer)),
                                g_settings_path.c_str());
        wchar_t* end = nullptr;
        const wchar_t* digits = buffer[0] == L'#' ? buffer + 1 : buffer;
        const unsigned long color = wcstoul(digits, &end, 16);
        if (end && *end == L'\0' && wcslen(digits) == 6 && color <= 0xFFFFFFu)
            s_settings.colour[i] = static_cast<uint32_t>(color);
    }
    // One new setting. Preserve legacy users' OFF choice if either of the
    // previous independent controls was disabled.
    const int saved = GetPrivateProfileIntW(kSection, kDiscVisibleKey, -1,
                                           g_settings_path.c_str());
    if (saved != -1) {
        s_settings.disc_visible = saved != 0;
    } else {
        const bool sun = GetPrivateProfileIntW(kSection, kLegacySunVisibleKey, 1,
                                               g_settings_path.c_str()) != 0;
        const bool moon = GetPrivateProfileIntW(kSection, kLegacyMoonVisibleKey, 1,
                                                g_settings_path.c_str()) != 0;
        s_settings.disc_visible = sun && moon;
    }
    for (size_t i = 0; i < kSkyEffectCount; ++i)
        s_settings.effect_visible[i] =
            GetPrivateProfileIntW(kSection, kEffectKeys[i], 1,
                                  g_settings_path.c_str()) != 0;
    s_settings.loaded = true;
    return s_settings;
}
void SaveSoon() {
    Settings().save_pending = true;
    s_settings.save_at = GetTickCount64() + kSaveDebounce;
}
void FlushSettings() {
    if (!s_settings.save_pending || g_settings_path.empty() ||
        GetTickCount64() < s_settings.save_at) return;
    bool ok = true;
    for (size_t i = 0; i < kSkyRegionCount; ++i) {
        wchar_t hex[7]{};
        swprintf_s(hex, L"%06X", s_settings.colour[i] & 0xFFFFFFu);
        ok &= QueueSettingsValue(kSection, kEnabledKeys[i],
            s_settings.enabled[i] ? L"1" : L"0") != 0;
        ok &= QueueSettingsValue(kSection, kHexKeys[i], hex) != 0;
    }
    for (size_t i = 0; i < kSkyEffectCount; ++i)
        ok &= QueueSettingsValue(kSection, kEffectKeys[i],
            s_settings.effect_visible[i] ? L"1" : L"0") != 0;
    const wchar_t* disc_value = s_settings.disc_visible ? L"1" : L"0";
    ok &= QueueSettingsValue(kSection, kDiscVisibleKey, disc_value) != 0;
    // Keep old settings coherent if a user returns to an earlier ASI build.
    ok &= QueueSettingsValue(kSection, kLegacySunVisibleKey, disc_value) != 0;
    ok &= QueueSettingsValue(kSection, kLegacyMoonVisibleKey, disc_value) != 0;
    s_settings.save_pending = !ok;
    if (!ok) s_settings.save_at = GetTickCount64() + 3000;
}
DWORD WINAPI PickerThread(LPVOID) {
    COLORREF swatches[16]{};
    const uint32_t initial = s_picker_initial.load(std::memory_order_relaxed);
    CHOOSECOLORW chooser{};
    chooser.lStructSize = sizeof(chooser);
    chooser.lpCustColors = swatches;
    chooser.rgbResult = RGB((initial >> 16) & 255, (initial >> 8) & 255, initial & 255);
    chooser.Flags = CC_ANYCOLOR | CC_FULLOPEN | CC_RGBINIT;
    if (ChooseColorW(&chooser)) {
        const uint32_t result = (static_cast<uint32_t>(GetRValue(chooser.rgbResult)) << 16) |
            (static_cast<uint32_t>(GetGValue(chooser.rgbResult)) << 8) |
            static_cast<uint32_t>(GetBValue(chooser.rgbResult));
        s_picker_result.store(result, std::memory_order_release);
    } else {
        s_picker_result.store(CommDlgExtendedError() ? kPickerError : kPickerCancelled,
                              std::memory_order_release);
    }
    s_picker_open.store(false, std::memory_order_release);
    return 0;
}
void PollPicker() {
    const uint32_t picked = s_picker_result.exchange(kNoPickerResult,
                                                     std::memory_order_acq_rel);
    if (picked <= 0xFFFFFFu) {
        SetSkyColourHex(static_cast<SkyRegion>(s_picker_region.load()), picked);
    } else if (picked == kPickerError) Notify(L"Sky colour picker could not open");
}
} // namespace

bool IsSkyDiscVisible() {
    return Settings().disc_visible;
}
void SetSkyDiscVisible(bool visible) {
    if (Settings().disc_visible == visible) return;
    s_settings.disc_visible = visible;
    SaveSoon();
    Notify(visible ? L"Moon/Sun sky disc: ON (visual only)" :
                     L"Moon/Sun sky disc: OFF (visual only)");
}
void ResetSkyDiscVisibility() {
    SetSkyDiscVisible(true);
}

bool IsSkyEffectVisible(SkyEffect effect) {
    const size_t index = static_cast<size_t>(effect);
    return index < kSkyEffectCount ? Settings().effect_visible[index] : true;
}
void SetSkyEffectVisible(SkyEffect effect, bool visible) {
    const size_t index = static_cast<size_t>(effect);
    if (index >= kSkyEffectCount || Settings().effect_visible[index] == visible) return;
    s_settings.effect_visible[index] = visible;
    SaveSoon();
    Notify(std::wstring(kEffectLabels[index]) +
           (visible ? L" visible: ON (visual only)" : L" visible: OFF (visual only)"));
}
void ResetSkyEffectVisibility() {
    SetSkyEffectVisible(SkyEffect::Clouds, true);
    SetSkyEffectVisible(SkyEffect::Stars, true);
}

bool IsSkyColourEnabled(SkyRegion region) {
    const auto i = static_cast<size_t>(region);
    return i < kSkyRegionCount && Settings().enabled[i];
}
void SetSkyColourEnabled(SkyRegion region, bool enabled) {
    const size_t i = static_cast<size_t>(region);
    if (i >= kSkyRegionCount || Settings().enabled[i] == enabled) return;
    s_settings.enabled[i] = enabled;
    s_native_name_retry_after[i] = 0;
    SaveSoon();
    Notify(enabled ? L"Custom sky colour enabled (visual only)" :
                     L"Game-default sky colour restored (visual only)");
}
std::wstring GetSkyColourHex(SkyRegion region) {
    const size_t i = static_cast<size_t>(region);
    if (i >= kSkyRegionCount) return L"#000000";
    wchar_t hex[8]{};
    swprintf_s(hex, L"#%06X", Settings().colour[i]);
    return hex;
}
void SetSkyColourHex(SkyRegion region, uint32_t rgb) {
    const size_t i = static_cast<size_t>(region);
    if (i >= kSkyRegionCount || rgb > 0xFFFFFFu) return;
    Settings();
    s_settings.colour[i] = rgb;
    s_settings.enabled[i] = true;
    s_native_name_retry_after[i] = 0;
    SaveSoon();
    Notify(i == 0 ? L"Sky zenith colour set (visual only)" :
           i == 1 ? L"Sky horizon colour set (visual only)" :
           i == 2 ? L"Sky sun colour set (visual only)" :
                    L"Sky cloud colour set (visual only)");
}
void ResetSkyColours() {
    SetSkyColourEnabled(SkyRegion::Zenith, false);
    SetSkyColourEnabled(SkyRegion::Horizon, false);
    SetSkyColourEnabled(SkyRegion::Sun, false);
    SetSkyColourEnabled(SkyRegion::Clouds, false);
}
void OpenSkyColourPicker(SkyRegion region) {
    const size_t i = static_cast<size_t>(region);
    if (i >= kSkyRegionCount) return;
    if (s_picker_open.exchange(true)) {
        Notify(L"Sky colour picker already open");
        return;
    }
    // A completed dialog must be consumed before the next one starts.
    PollPicker();
    s_picker_region.store(static_cast<int>(i));
    s_picker_initial.store(Settings().colour[i]);
    s_picker_result.store(kNoPickerResult);
    HANDLE worker = CreateThread(nullptr, 0, PickerThread, nullptr, 0, nullptr);
    if (worker) CloseHandle(worker);
    else { s_picker_open.store(false); Notify(L"Could not create sky picker thread"); }
}
void UpdateSkyColours(MyCharacter* player) {
    PollPicker();
    Settings();
    FlushSettings();
    bool needed = false;
    for (size_t i = 0; i < kSkyRegionCount; ++i)
        needed |= s_settings.enabled[i] || s_target.values[i].modified;
    needed |= !s_settings.disc_visible || s_target.sun_brightness.modified;
    for (size_t i = 0; i < kSkyEffectCount; ++i)
        needed |= !s_settings.effect_visible[i] || s_target.effects[i].modified;
    if (!needed || !player || !IsReadable(player, sizeof(MyCharacter))) return;
    const auto* objects = SkyObjects();
    if (!objects) return;
    bool live = s_target.actor && TargetLive(objects);
    if (s_target.actor && !live) {
        ResetTarget();
        s_scan_cursor = 0;
    }
    const ULONGLONG now = GetTickCount64();
    // Respect the missing-target retry deadline instead of restarting a full
    // object scan immediately after every unsuccessful pass.
    // Once actor and converter are found, another global object scan cannot
    // resolve an absent material parameter. ResolveName handles that locally.
    if (!s_target.actor || !s_name_library) {
        if (s_scan_cursor < 0 && now >= s_rescan_at) s_scan_cursor = 0;
        if (s_scan_cursor >= 0) {
            FindSky(objects);
            if (!live) live = TargetLive(objects);
        }
    } else {
        s_scan_cursor = -1;
    }
    if (!live) {
        if (s_target.actor && !s_logged_unavailable) {
            Log(L"Sky colours: sky MID was discovered but failed the live-object recheck");
            s_logged_unavailable = true;
        }
        return;
    }
    UpdateDiscVisibility();
    for (size_t i = 0; i < kSkyEffectCount; ++i)
        UpdateSkyEffectVisibility(i);
    for (size_t i = 0; i < kSkyRegionCount; ++i) {
        auto& value = s_target.values[i];
        if (!s_settings.enabled[i] && !value.modified) continue;
        if (!ResolveName(value, s_target.mid, kParameterNames[i], i)) continue;
        LinearColor current{};
        if (!GetMIDColor(s_target.mid, value.name, current)) {
            SkyStage(i, 7, L"MID vector getter unavailable or returned invalid colour");
            continue;
        }
        if (s_settings.enabled[i]) {
            if (!value.modified) value.original = current;
            if (value.desired_rgb != s_settings.colour[i]) {
                value.desired_rgb = s_settings.colour[i];
                value.desired_colour = ColourFromHex(value.desired_rgb);
            }
            const LinearColor desired = value.desired_colour;
            // Native BP_DynamicSky may update the MID during its own tick.
            // Restore the user's chosen colour after native updates, without
            // writing every frame when the material already matches.
            if (!SameColour(current, desired) &&
                !SetMIDColor(s_target.mid, value.name, desired)) {
                SkyStage(i, 8, L"MID vector setter unavailable; no colour written");
                continue;
            }
            if (!value.modified || !SameColour(value.last_written, desired)) {
                wchar_t colour[7]{};
                swprintf_s(colour, L"%06X", s_settings.colour[i] & 0xFFFFFFu);
                SkyStage(i, 9, (std::wstring(L"override sent to sky MID: #") + colour).c_str());
            }
            value.modified = true;
            value.last_written = desired;
        } else if (value.modified) {
            // Don't overwrite a newer native change made after our own write.
            if (SameColour(current, value.last_written)) {
                if (SetMIDColor(s_target.mid, value.name, value.original))
                    SkyStage(i, 10, L"restored original sky MID colour");
                else SkyStage(i, 11, L"could not restore original sky MID colour");
            } else {
                SkyStage(i, 12, L"native sky colour changed since override; keeping native value");
            }
            value.modified = false;
        }
    }
}
} // namespace refunct

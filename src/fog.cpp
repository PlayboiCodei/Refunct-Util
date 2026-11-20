#include "refunct/internal_api.hpp"
#include <commdlg.h>

namespace refunct {
namespace {
// Verified against the cooked Refunct WorldBase.umap: ExponentialHeightFog,
// HeightFogComponent0, FogInscatteringColor, DirectionalInscatteringColor.
// BP_DynamicSky also references C_FogColor and C_FogDirectionalColor.
constexpr wchar_t kSection[] = L"FogColours";
constexpr size_t kRegionCount = 2;
constexpr const wchar_t* kProperties[kRegionCount] = {
    L"FogInscatteringColor", L"DirectionalInscatteringColor"
};
constexpr const wchar_t* kSetters[kRegionCount] = {
    L"SetFogInscatteringColor", L"SetDirectionalInscatteringColor"
};
constexpr const wchar_t* kEnabledKeys[kRegionCount] = {
    L"GeneralEnabled", L"DirectionalEnabled"
};
constexpr const wchar_t* kVisibleKeys[kRegionCount] = {
    L"GeneralVisible", L"DirectionalVisible"
};
constexpr const wchar_t* kHexKeys[kRegionCount] = {
    L"GeneralHex", L"DirectionalHex"
};
constexpr const wchar_t* kDefaultHex[kRegionCount] = {
    L"C2CBE0", L"FFE3BD"
};
constexpr const wchar_t* kLabels[kRegionCount] = {
    L"Fog", L"Directional fog"
};
constexpr uint32_t kNoPickerResult = 0xFFFFFFFFu;
constexpr uint32_t kPickerCancelled = 0xFFFFFFFEu;
constexpr uint32_t kPickerError = 0xFFFFFFFDu;
constexpr int32_t kScanBatch = 1024;
constexpr ULONGLONG kScanBudgetMs = 2;

struct FogSettings {
    bool loaded = false;
    std::array<bool, kRegionCount> enabled{};
    std::array<bool, kRegionCount> visible{{true, true}};
    std::array<uint32_t, kRegionCount> colours{0xC2CBE0u, 0xFFE3BDu};
    bool save_pending = false;
    ULONGLONG save_due = 0;
} s_settings;

struct FogValue {
    int32_t colour_offset = -1;
    void* setter = nullptr;
    int32_t setter_arg_offset = -1;
    LinearColor original{};
    LinearColor last_written{};
    uint32_t desired_rgb = 0xFFFFFFFFu;
    LinearColor desired_colour{};
    bool modified = false;
    int diagnostic = -1;
    ULONGLONG retry_at = 0;
};
// Native general fog density; setting zero suppresses all exponential height fog.
// Keep this separate from colour overrides so turning fog back on restores
// the exact original density without disabling custom fog colours.
struct FogDensity {
    int32_t property_offset = -1;
    void* setter = nullptr;
    int32_t arg_offset = -1;
    float original = 0.0f;
    bool modified = false;
    ULONGLONG retry_at = 0;
    int diagnostic = -1;
};
struct FogTarget {
    void* component = nullptr;
    int32_t component_index = -1;
    int32_t component_serial = 0;
    void* actor = nullptr;
    int32_t actor_index = -1;
    int32_t actor_serial = 0;
    void* component_class = nullptr;
    void* actor_class = nullptr;
    std::array<FogValue, kRegionCount> values{};
    FogDensity density{};
} s_target;
int32_t s_scan_cursor = -1;
ULONGLONG s_next_scan = 0;
bool s_logged_absent = false;

std::atomic_bool s_picker_open{false};
std::atomic_uint32_t s_picker_initial{0};
std::atomic_uint32_t s_picker_result{kNoPickerResult};
std::atomic_int s_picker_region{0};

void Report(size_t region, int stage, const wchar_t* message) {
    auto& value = s_target.values[region];
    if (value.diagnostic == stage) return;
    value.diagnostic = stage;
    const std::wstring text = std::wstring(L"Fog colours: ") + kLabels[region] +
        L": " + message;
    Log(text.c_str());
}
std::wstring NameOf(const void* object) {
    return IsReadable(object, sizeof(UObjectHeader))
        ? UnrealName(static_cast<const UObjectHeader*>(object)->name) : L"";
}
bool IsA(const void* object, const wchar_t* class_name) {
    if (!IsReadable(object, sizeof(UObjectHeader))) return false;
    void* cls = static_cast<const UObjectHeader*>(object)->class_ptr;
    for (int depth = 0; depth < 16 && IsReadable(cls, sizeof(UStructHeader)); ++depth) {
        if (NameOf(cls) == class_name) return true;
        cls = static_cast<const UStructHeader*>(cls)->super_struct;
    }
    return false;
}
const GlobalObjectArray* Objects() {
    if (!g_game_module) return nullptr;
    const auto* objects = reinterpret_cast<const GlobalObjectArray*>(
        reinterpret_cast<uintptr_t>(g_game_module) + kGlobalObjectArrayRva);
    if (!IsReadable(objects, sizeof(*objects)) || !objects->objects ||
        objects->num_elements < 0 || objects->num_elements > 1000000 ||
        objects->max_elements < objects->num_elements ||
        !IsReadable(objects->objects, static_cast<size_t>(objects->num_elements) *
                                     sizeof(GlobalObjectItem))) return nullptr;
    return objects;
}
bool Live(const GlobalObjectArray* objects, const void* object,
          int32_t index, int32_t serial) {
    return objects && object && index >= 0 && index < objects->num_elements &&
        objects->objects[index].object == object &&
        objects->objects[index].serial_number == serial &&
        IsReadable(object, sizeof(UObjectHeader)) &&
        (static_cast<uint32_t>(static_cast<const UObjectHeader*>(object)->flags) & 0x30u) == 0;
}
bool TargetLive(const GlobalObjectArray* objects) {
    if (!Live(objects, s_target.component, s_target.component_index, s_target.component_serial) ||
        !Live(objects, s_target.actor, s_target.actor_index, s_target.actor_serial)) return false;
    // Discovery already verified both inheritance chains. Slot, serial, class
    // and ownership checks retain lifetime safety without name walks each tick.
    const auto* component = static_cast<const UObjectHeader*>(s_target.component);
    const auto* actor = static_cast<const UObjectHeader*>(s_target.actor);
    return component->outer == s_target.actor && s_target.component_class &&
        s_target.actor_class && component->class_ptr == s_target.component_class &&
        actor->class_ptr == s_target.actor_class;
}
void ResetTarget() {
    s_target = {};
    s_logged_absent = false;
}

// Accept only a reflected FLinearColor StructProperty on this specific
// native fog component class. An unknown property layout is not writable.
int32_t ColourPropertyOffset(void* component, const wchar_t* property_name) {
    if (!IsReadable(component, sizeof(UObjectHeader))) return -1;
    void* cls = static_cast<const UObjectHeader*>(component)->class_ptr;
    for (int depth = 0; depth < 16 && IsReadable(cls, sizeof(UStructHeader)); ++depth) {
        void* field = static_cast<const UStructHeader*>(cls)->children;
        for (int count = 0; count < 2048 && IsReadable(field, sizeof(UFieldHeader)); ++count) {
            auto* ufield = static_cast<const UFieldHeader*>(field);
            if (NameOf(field) == property_name) {
                if (NameOf(ufield->object.class_ptr) != L"StructProperty") return -1;
                const int32_t offset = FindClassPropertyOffset(cls, property_name);
                if (offset < 0 || offset > 0x1FF0 ||
                    !IsReadable(static_cast<uint8_t*>(component) + offset, sizeof(LinearColor)))
                    return -1;
                return offset;
            }
            field = ufield->next;
        }
        cls = static_cast<const UStructHeader*>(cls)->super_struct;
    }
    return -1;
}

// Both UE4 fog setters accept one FLinearColor struct argument. Resolve its
// actual offset from UFunction reflection; never guess an argument offset.
int32_t SetterColourArgument(void* function) {
    if (!IsReadable(function, sizeof(UStructHeader))) return -1;
    void* field = static_cast<const UStructHeader*>(function)->children;
    int32_t result = -1;
    for (int count = 0; count < 64 && IsReadable(field, sizeof(UFieldHeader)); ++count) {
        const auto* f = static_cast<const UFieldHeader*>(field);
        if (NameOf(f->object.class_ptr) == L"StructProperty") {
            // UProperty::PropertyFlags at 0x28 on Refunct's 32-bit UE4 ABI.
            constexpr size_t kFlagsOffset = 0x28;
            if (!IsReadable(field, kFlagsOffset + sizeof(uint64_t))) return -1;
            uint64_t flags = 0;
            std::memcpy(&flags, static_cast<const uint8_t*>(field) + kFlagsOffset,
                        sizeof(flags));
            if ((flags & 0x80u) != 0 && (flags & 0x400u) == 0) {
                const std::wstring name = NameOf(field);
                const int32_t offset = FindClassPropertyOffset(function, name.c_str());
                if (result >= 0 || offset < 0 || offset > 128 -
                        static_cast<int32_t>(sizeof(LinearColor))) return -1;
                result = offset;
            }
        }
        field = f->next;
    }
    return result;
}
// The native UE4 ExponentialHeightFogComponent exposes FogDensity as a
// FloatProperty with a reflected SetFogDensity(float) function. The x86
// reflection ABI is checked before either address is accessed.
int32_t FloatPropertyOffset(void* component, const wchar_t* property_name) {
    if (!IsReadable(component, sizeof(UObjectHeader))) return -1;
    void* cls = static_cast<const UObjectHeader*>(component)->class_ptr;
    for (int depth = 0; depth < 16 && IsReadable(cls, sizeof(UStructHeader)); ++depth) {
        void* field = static_cast<const UStructHeader*>(cls)->children;
        for (int count = 0; count < 2048 && IsReadable(field, sizeof(UFieldHeader)); ++count) {
            const auto* header = static_cast<const UFieldHeader*>(field);
            if (NameOf(field) == property_name) {
                if (NameOf(header->object.class_ptr) != L"FloatProperty") return -1;
                const int32_t offset = FindClassPropertyOffset(cls, property_name);
                return offset >= 0 && offset <= 0x1FFC &&
                    IsReadable(static_cast<uint8_t*>(component) + offset, sizeof(float))
                        ? offset : -1;
            }
            field = header->next;
        }
        cls = static_cast<const UStructHeader*>(cls)->super_struct;
    }
    return -1;
}
int32_t SetterFloatArgument(void* function) {
    if (!IsReadable(function, sizeof(UStructHeader))) return -1;
    void* field = static_cast<const UStructHeader*>(function)->children;
    int32_t result = -1;
    for (int count = 0; count < 64 && IsReadable(field, sizeof(UFieldHeader)); ++count) {
        const auto* header = static_cast<const UFieldHeader*>(field);
        if (NameOf(header->object.class_ptr) == L"FloatProperty") {
            constexpr size_t kFlagsOffset = 0x28;
            if (!IsReadable(field, kFlagsOffset + sizeof(uint64_t))) return -1;
            uint64_t flags = 0;
            std::memcpy(&flags, static_cast<const uint8_t*>(field) + kFlagsOffset,
                        sizeof(flags));
            if ((flags & 0x80u) && !(flags & 0x400u)) {
                const auto name = NameOf(field);
                const int32_t offset = FindClassPropertyOffset(function, name.c_str());
                if (result >= 0 || offset < 0 || offset >
                    128 - static_cast<int32_t>(sizeof(float))) return -1;
                result = offset;
            }
        }
        field = header->next;
    }
    return result;
}
void ReportDensity(int stage, const wchar_t* message) {
    auto& density = s_target.density;
    if (density.diagnostic == stage) return;
    density.diagnostic = stage;
    Log((std::wstring(L"Fog visibility: ") + message).c_str());
}
void ResolveDensity() {
    auto& density = s_target.density;
    density.property_offset = FloatPropertyOffset(s_target.component, L"FogDensity");
    if (density.property_offset < 0) {
        ReportDensity(1, L"FogDensity FloatProperty unavailable");
        return;
    }
    density.setter = FindReflectedFunction(s_target.component, L"SetFogDensity");
    if (!density.setter) {
        ReportDensity(2, L"SetFogDensity not reflected");
        return;
    }
    density.arg_offset = SetterFloatArgument(density.setter);
    if (density.arg_offset < 0) {
        density.setter = nullptr;
        ReportDensity(3, L"SetFogDensity float argument not recognised");
    } else ReportDensity(4, L"native fog density getter/setter verified");
}
bool ReadDensity(float& result) {
    const auto& density = s_target.density;
    if (density.property_offset < 0 || !s_target.component) return false;
    const void* address = static_cast<const uint8_t*>(s_target.component) + density.property_offset;
    if (!IsReadable(address, sizeof(float))) return false;
    std::memcpy(&result, address, sizeof(float));
    return std::isfinite(result) && result >= 0.0f && result <= 100.0f;
}
bool WriteDensity(float desired) {
    const auto& density = s_target.density;
    if (!g_process_event || !density.setter || density.arg_offset < 0 ||
        !std::isfinite(desired)) return false;
    alignas(16) std::array<uint8_t, 128> args{};
    std::memcpy(args.data() + density.arg_offset, &desired, sizeof(desired));
    g_process_event(s_target.component, density.setter, args.data());
    float read_back = 0.0f;
    return ReadDensity(read_back) && std::fabs(read_back - desired) < 0.00001f;
}
void UpdateFogDensityVisibility() {
    auto& density = s_target.density;
    if (density.property_offset < 0 || !density.setter ||
        (s_settings.visible[0] && !density.modified)) return;
    float current = 0.0f;
    if (!ReadDensity(current)) return;
    if (!s_settings.visible[0]) {
        // Do not snapshot native zero as the restoration value.
        if (current > 0.00001f) density.original = current;
        if (current > 0.00001f && GetTickCount64() >= density.retry_at) {
            if (!WriteDensity(0.0f)) {
                density.retry_at = GetTickCount64() + 1000;
                ReportDensity(5, L"could not suppress fog density; will retry");
                return;
            }
            density.retry_at = 0;
            ReportDensity(6, L"fog density suppressed (read-back verified)");
        }
        if (density.original > 0.00001f) density.modified = true;
    } else if (density.modified) {
        // When the game changes the value itself, respect its newer value.
        if (current <= 0.00001f) {
            if (GetTickCount64() < density.retry_at) return;
            if (!WriteDensity(density.original)) {
                density.retry_at = GetTickCount64() + 1000;
                ReportDensity(7, L"fog density restoration failed; will retry");
                return;
            }
            ReportDensity(8, L"original fog density restored");
        }
        density.modified = false;
        density.original = 0.0f;
        density.retry_at = 0;
    }
}
void ResolveMethods() {
    ResolveDensity();
    for (size_t i = 0; i < kRegionCount; ++i) {
        auto& value = s_target.values[i];
        value.colour_offset = ColourPropertyOffset(s_target.component, kProperties[i]);
        if (value.colour_offset < 0) {
            Report(i, 1, L"native FLinearColor property not found");
            continue;
        }
        value.setter = FindReflectedFunction(s_target.component, kSetters[i]);
        if (!value.setter) {
            Report(i, 2, L"native fog setter not reflected; no colour written");
            continue;
        }
        value.setter_arg_offset = SetterColourArgument(value.setter);
        if (value.setter_arg_offset < 0) {
            Report(i, 3, L"native fog setter signature not recognised");
            value.setter = nullptr;
            continue;
        }
        Report(i, 4, L"verified native fog colour property and setter");
    }
}
void FindFog(const GlobalObjectArray* objects) {
    if (!objects || s_scan_cursor < 0) return;
    const int32_t end = (std::min)(s_scan_cursor + kScanBatch, objects->num_elements);
    const ULONGLONG begin = GetTickCount64();
    for (; s_scan_cursor < end; ++s_scan_cursor) {
        if ((s_scan_cursor & 127) == 0 && GetTickCount64() - begin > kScanBudgetMs) break;
        const auto& item = objects->objects[s_scan_cursor];
        void* component = item.object;
        if (!IsReadable(component, sizeof(UObjectHeader)) ||
            (static_cast<uint32_t>(item.flags) & 0x30000000u) != 0 ||
            NameOf(component) != L"HeightFogComponent0" ||
            !IsA(component, L"ExponentialHeightFogComponent")) continue;
        auto* actor = static_cast<UObjectHeader*>(component)->outer;
        if (!IsA(actor, L"ExponentialHeightFog") ||
            NameOf(actor).rfind(L"Default__", 0) == 0) continue;
        const int32_t actor_index = static_cast<UObjectHeader*>(actor)->index;
        if (actor_index < 0 || actor_index >= objects->num_elements ||
            objects->objects[actor_index].object != actor ||
            !Live(objects, component, s_scan_cursor, item.serial_number) ||
            !Live(objects, actor, actor_index,
                  objects->objects[actor_index].serial_number)) continue;
        ResetTarget();
        s_target.component = component;
        s_target.component_index = s_scan_cursor;
        s_target.component_serial = item.serial_number;
        s_target.actor = actor;
        s_target.actor_index = actor_index;
        s_target.actor_serial = objects->objects[actor_index].serial_number;
        s_target.component_class = static_cast<const UObjectHeader*>(component)->class_ptr;
        s_target.actor_class = static_cast<const UObjectHeader*>(actor)->class_ptr;
        ResolveMethods();
        Log(L"Fog colours: found WorldBase ExponentialHeightFog / HeightFogComponent0");
        s_scan_cursor = -1;
        return;
    }
    if (s_scan_cursor >= objects->num_elements) {
        s_scan_cursor = -1;
        s_next_scan = GetTickCount64() + 3000;
        if (!s_logged_absent) {
            Log(L"Fog colours: native height-fog component not found; will retry");
            s_logged_absent = true;
        }
    }
}

bool ReadColour(const FogValue& value, LinearColor& result) {
    if (value.colour_offset < 0 || !s_target.component) return false;
    const auto* address = static_cast<const uint8_t*>(s_target.component) + value.colour_offset;
    if (!IsReadable(address, sizeof(result))) return false;
    std::memcpy(&result, address, sizeof(result));
    for (float c : {result.r, result.g, result.b, result.a})
        if (!std::isfinite(c) || c < 0.0f || c > 100.0f) return false;
    return true;
}
bool WriteColour(const FogValue& value, const LinearColor& desired) {
    if (!g_process_event || !value.setter || value.setter_arg_offset < 0) return false;
    alignas(16) std::array<uint8_t, 128> args{};
    std::memcpy(args.data() + value.setter_arg_offset, &desired, sizeof(desired));
    g_process_event(s_target.component, value.setter, args.data());
    // ProcessEvent returning does not prove that the engine accepted the value.
    // Require native property read-back before treating an override as applied.
    LinearColor applied{};
    if (!ReadColour(value, applied)) return false;
    return std::fabs(applied.r - desired.r) < 0.002f &&
           std::fabs(applied.g - desired.g) < 0.002f &&
           std::fabs(applied.b - desired.b) < 0.002f;
}
LinearColor FromHex(uint32_t rgb) {
    const auto linear = [](uint32_t byte) {
        const float x = static_cast<float>(byte & 255u) / 255.0f;
        return x <= 0.04045f ? x / 12.92f : std::pow((x + 0.055f) / 1.055f, 2.4f);
    };
    return {linear(rgb >> 16), linear(rgb >> 8), linear(rgb), 1.0f};
}
bool Similar(const LinearColor& a, const LinearColor& b) {
    return std::fabs(a.r - b.r) < 0.002f && std::fabs(a.g - b.g) < 0.002f &&
           std::fabs(a.b - b.b) < 0.002f;
}

FogSettings& Settings() {
    if (s_settings.loaded || g_settings_path.empty()) return s_settings;
    for (size_t i = 0; i < kRegionCount; ++i) {
        s_settings.enabled[i] = GetPrivateProfileIntW(kSection, kEnabledKeys[i], 0,
                                                      g_settings_path.c_str()) != 0;
        s_settings.visible[i] = GetPrivateProfileIntW(kSection, kVisibleKeys[i], 1,
                                                      g_settings_path.c_str()) != 0;
        wchar_t buffer[16]{};
        GetPrivateProfileStringW(kSection, kHexKeys[i], kDefaultHex[i], buffer,
                                static_cast<DWORD>(std::size(buffer)),
                                g_settings_path.c_str());
        const wchar_t* hex = buffer[0] == L'#' ? buffer + 1 : buffer;
        wchar_t* end = nullptr;
        const unsigned long rgb = wcstoul(hex, &end, 16);
        if (end && *end == L'\0' && wcslen(hex) == 6 && rgb <= 0xFFFFFFu)
            s_settings.colours[i] = static_cast<uint32_t>(rgb);
    }
    s_settings.loaded = true;
    return s_settings;
}
void SaveSoon() {
    Settings().save_pending = true;
    s_settings.save_due = GetTickCount64() + 350;
}
void FlushSettings() {
    if (!s_settings.save_pending || g_settings_path.empty() ||
        GetTickCount64() < s_settings.save_due) return;
    bool saved = true;
    for (size_t i = 0; i < kRegionCount; ++i) {
        wchar_t hex[7]{};
        swprintf_s(hex, L"%06X", s_settings.colours[i] & 0xFFFFFFu);
        saved &= QueueSettingsValue(kSection, kEnabledKeys[i],
            s_settings.enabled[i] ? L"1" : L"0") != 0;
        saved &= QueueSettingsValue(kSection, kVisibleKeys[i],
            s_settings.visible[i] ? L"1" : L"0") != 0;
        saved &= QueueSettingsValue(kSection, kHexKeys[i],
            hex) != 0;
    }
    s_settings.save_pending = !saved;
    if (!saved) s_settings.save_due = GetTickCount64() + 3000;
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
        const uint32_t rgb = (static_cast<uint32_t>(GetRValue(chooser.rgbResult)) << 16) |
            (static_cast<uint32_t>(GetGValue(chooser.rgbResult)) << 8) |
            static_cast<uint32_t>(GetBValue(chooser.rgbResult));
        s_picker_result.store(rgb, std::memory_order_release);
    } else {
        s_picker_result.store(CommDlgExtendedError() ? kPickerError : kPickerCancelled,
                              std::memory_order_release);
    }
    s_picker_open.store(false, std::memory_order_release);
    return 0;
}
void PollPicker() {
    const uint32_t result = s_picker_result.exchange(kNoPickerResult,
                                                     std::memory_order_acq_rel);
    if (result <= 0xFFFFFFu)
        SetFogColourHex(static_cast<FogRegion>(s_picker_region.load()), result);
    else if (result == kPickerError) Notify(L"Fog colour picker could not open");
}
} // namespace

bool IsFogVisible(FogRegion region) {
    const auto index = static_cast<size_t>(region);
    return index < kRegionCount ? Settings().visible[index] : true;
}
void SetFogVisible(FogRegion region, bool visible) {
    const auto index = static_cast<size_t>(region);
    if (index >= kRegionCount || Settings().visible[index] == visible) return;
    s_settings.visible[index] = visible;
    SaveSoon();
    Notify(visible ? L"Fog visibility restored (visual only)" :
                     L"Fog visibility disabled (visual only)");
}
void ResetFogVisibility() {
    SetFogVisible(FogRegion::General, true);
    SetFogVisible(FogRegion::Directional, true);
}
bool IsFogColourEnabled(FogRegion region) {
    const auto index = static_cast<size_t>(region);
    return index < kRegionCount && Settings().enabled[index];
}
void SetFogColourEnabled(FogRegion region, bool enabled) {
    const auto index = static_cast<size_t>(region);
    if (index >= kRegionCount || Settings().enabled[index] == enabled) return;
    s_settings.enabled[index] = enabled;
    SaveSoon();
    Notify(enabled ? L"Custom fog colour enabled (visual only)" :
                     L"Game-default fog colour restored (visual only)");
}
std::wstring GetFogColourHex(FogRegion region) {
    const auto index = static_cast<size_t>(region);
    if (index >= kRegionCount) return L"#000000";
    wchar_t hex[8]{};
    swprintf_s(hex, L"#%06X", Settings().colours[index]);
    return hex;
}
void SetFogColourHex(FogRegion region, uint32_t rgb) {
    const auto index = static_cast<size_t>(region);
    if (index >= kRegionCount || rgb > 0xFFFFFFu) return;
    Settings();
    s_settings.colours[index] = rgb;
    s_settings.enabled[index] = true;
    SaveSoon();
    Notify(index == 0 ? L"Fog haze colour set (visual only)" :
                        L"Directional fog colour set (visual only)");
}
void ResetFogColours() {
    SetFogColourEnabled(FogRegion::General, false);
    SetFogColourEnabled(FogRegion::Directional, false);
}
void OpenFogColourPicker(FogRegion region) {
    const auto index = static_cast<size_t>(region);
    if (index >= kRegionCount) return;
    if (s_picker_open.exchange(true)) {
        Notify(L"Fog colour picker already open");
        return;
    }
    PollPicker();
    s_picker_region.store(static_cast<int>(index));
    s_picker_initial.store(Settings().colours[index]);
    s_picker_result.store(kNoPickerResult);
    HANDLE thread = CreateThread(nullptr, 0, PickerThread, nullptr, 0, nullptr);
    if (thread) CloseHandle(thread);
    else { s_picker_open.store(false); Notify(L"Could not create fog picker thread"); }
}
void UpdateFogColours(MyCharacter* player) {
    PollPicker();
    Settings();
    FlushSettings();
    bool needed = false;
    for (size_t i = 0; i < kRegionCount; ++i)
        needed |= s_settings.enabled[i] || !s_settings.visible[i] ||
                  s_target.values[i].modified;
    needed |= s_target.density.modified;
    if (!needed || !player || !IsReadable(player, sizeof(MyCharacter))) return;
    const GlobalObjectArray* objects = Objects();
    if (!objects) return;
    bool live = s_target.component && TargetLive(objects);
    if (s_target.component && !live) {
        ResetTarget();
        s_scan_cursor = 0;
    }
    if (!s_target.component && (s_scan_cursor < 0 && GetTickCount64() >= s_next_scan))
        s_scan_cursor = 0;
    if (s_scan_cursor >= 0 && !s_target.component) {
        FindFog(objects);
        live = TargetLive(objects);
    }
    if (!live) return;
    UpdateFogDensityVisibility();
    for (size_t i = 0; i < kRegionCount; ++i) {
        auto& value = s_target.values[i];
        const bool hide_directional = i == 1 && !s_settings.visible[i];
        if (!s_settings.enabled[i] && !hide_directional && !value.modified) continue;
        LinearColor current{};
        if (!ReadColour(value, current)) {
            Report(i, 5, L"fog component colour read failed");
            continue;
        }
        if (s_settings.enabled[i] || hide_directional) {
            if (!value.modified) value.original = current;
            // The native fog colour can change with time of day; keep its
            // read/write cadence, but convert the user's RGB only on edits.
            if (!hide_directional && value.desired_rgb != s_settings.colours[i]) {
                value.desired_rgb = s_settings.colours[i];
                value.desired_colour = FromHex(value.desired_rgb);
            }
            const LinearColor desired = hide_directional
                ? LinearColor{0.0f, 0.0f, 0.0f, current.a}
                : value.desired_colour;
            if (!Similar(current, desired)) {
                if (GetTickCount64() < value.retry_at) continue;
                if (!WriteColour(value, desired)) {
                    value.retry_at = GetTickCount64() + 1000;
                    Report(i, 6, L"fog setter unavailable or native colour read-back failed");
                    continue;
                }
                value.retry_at = 0;
                Report(i, 7, L"native fog colour setter applied and verified");
            }
            value.modified = true;
            value.last_written = desired;
        } else if (value.modified) {
            // A later native update takes priority over an older custom value.
            if (Similar(current, value.last_written)) {
                if (GetTickCount64() < value.retry_at) continue;
                if (WriteColour(value, value.original)) {
                    Report(i, 8, L"restored original native fog colour");
                    value.modified = false;
                    value.retry_at = 0;
                } else {
                    Report(i, 9, L"could not restore original native fog colour; will retry");
                    value.retry_at = GetTickCount64() + 1000;
                }
            } else {
                Report(i, 10, L"native fog changed; leaving newer game value intact");
                value.modified = false;
            }
        }
    }
}
} // namespace refunct

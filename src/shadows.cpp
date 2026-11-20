#include "refunct/internal_api.hpp"
#include <commdlg.h>

namespace refunct {
namespace {
// Refunct's cooked WorldBase.umap contains a DirectionalLightComponent and
// PostNormal PostProcessVolume. AmbientCubemapTint is ambient *fill*, not a
// true per-cast-shadow material parameter. Never modify PostWater.
constexpr wchar_t kSection[] = L"Shadows";
constexpr ULONGLONG kRetryMs = 1200;
constexpr int32_t kBatchSize = 768;
constexpr ULONGLONG kScanBudgetMs = 2;
constexpr uint32_t kPickerEmpty = 0xFFFFFFFFu;
constexpr uint32_t kPickerCancelled = 0xFFFFFFFEu;
constexpr uint32_t kPickerError = 0xFFFFFFFDu;

struct Config {
    bool loaded = false;
    bool visible = true;
    bool tint_enabled = false;
    uint32_t tint_rgb = 0xFFFFFFu;
    bool dirty = false;
    ULONGLONG save_after = 0;
} s_config;

struct NativeRef {
    void* ptr = nullptr;
    int32_t index = -1;
    int32_t serial = 0;
};
struct LightTarget {
    NativeRef component{};
    NativeRef owner{};
    void* setter = nullptr;
    int32_t setter_arg = -1;
    int32_t prop_offset = -1;
    uint8_t byte_offset = 0;
    uint8_t mask = 0;
    bool original = true;
    bool changed = false;
    bool last_written = false;
    ULONGLONG retry_after = 0;
} s_light;
struct PostTarget {
    NativeRef volume{};
    NativeRef settings_struct{};
    int32_t settings_offset = -1;
    int32_t tint_offset = -1;
    int32_t override_offset = -1;
    uint8_t override_mask = 0;
    LinearColor original{};
    LinearColor last_written{};
    bool original_override = false;
    bool changed = false;
    ULONGLONG retry_after = 0;
} s_post;
int32_t s_scan_index = -1;
ULONGLONG s_rescan_after = 0;
int s_light_log = -1;
int s_post_log = -1;

std::atomic_bool s_picker_open{false};
std::atomic_uint32_t s_picker_result{kPickerEmpty};
std::atomic_uint32_t s_picker_initial{0};

void Trace(bool light, int stage, const wchar_t* text) {
    int& last = light ? s_light_log : s_post_log;
    if (last == stage) return;
    last = stage;
    Log((std::wstring(light ? L"Shadows: light: " : L"Shadows: ambient: ") + text).c_str());
}
std::wstring ObjectName(const void* value) {
    return IsReadable(value, sizeof(UObjectHeader))
        ? UnrealName(static_cast<const UObjectHeader*>(value)->name) : L"";
}
bool IsA(const void* obj, const wchar_t* cls_name) {
    if (!IsReadable(obj, sizeof(UObjectHeader))) return false;
    void* cls = static_cast<const UObjectHeader*>(obj)->class_ptr;
    for (int depth = 0; depth < 14 && IsReadable(cls, sizeof(UStructHeader)); ++depth) {
        if (ObjectName(cls) == cls_name) return true;
        cls = static_cast<const UStructHeader*>(cls)->super_struct;
    }
    return false;
}
const GlobalObjectArray* Objects() {
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
bool Live(const GlobalObjectArray* array, NativeRef ref) {
    return array && ref.ptr && ref.index >= 0 && ref.index < array->num_elements &&
        array->objects[ref.index].object == ref.ptr &&
        array->objects[ref.index].serial_number == ref.serial &&
        IsReadable(ref.ptr, sizeof(UObjectHeader)) &&
        (static_cast<uint32_t>(static_cast<UObjectHeader*>(ref.ptr)->flags) & 0x30u) == 0;
}
NativeRef Ref(const GlobalObjectArray* array, void* ptr) {
    NativeRef out{};
    if (!array || !IsReadable(ptr, sizeof(UObjectHeader))) return out;
    const int32_t index = static_cast<UObjectHeader*>(ptr)->index;
    if (index < 0 || index >= array->num_elements || array->objects[index].object != ptr)
        return out;
    out = {ptr, index, array->objects[index].serial_number};
    return Live(array, out) ? out : NativeRef{};
}
struct Field {
    const uint8_t* descriptor = nullptr;
    int32_t offset = -1;
};
Field FindField(void* type, const wchar_t* name, const wchar_t* expected_class) {
    for (int d = 0; d < 14 && IsReadable(type, sizeof(UStructHeader)); ++d) {
        const auto* st = static_cast<const UStructHeader*>(type);
        void* field = st->children;
        for (int n = 0; n < 4096 && IsReadable(field, sizeof(UFieldHeader)); ++n) {
            const auto* prop = static_cast<const UFieldHeader*>(field);
            if (ObjectName(field) == name) {
                if (ObjectName(prop->object.class_ptr) != expected_class) return {};
                const int32_t offset = FindClassPropertyOffset(type, name);
                return {static_cast<const uint8_t*>(field), offset};
            }
            field = prop->next;
        }
        type = st->super_struct;
    }
    return {};
}
struct BoolBits {
    int32_t address_offset = -1;
    uint8_t mask = 0;
    uint8_t byte_offset = 0;
    size_t descriptor = 0;
};
bool ParseBool(const Field& field, size_t descriptor_offset, BoolBits& out) {
    if (!field.descriptor || field.offset < 0 || field.offset > 0x1FF0 ||
        !IsReadable(field.descriptor, descriptor_offset + 4)) return false;
    const uint8_t* d = field.descriptor + descriptor_offset;
    const uint8_t size = d[0], byte = d[1], mask = d[2], field_mask = d[3];
    if ((size != 1 && size != 2 && size != 4 && size != 8) || byte >= size ||
        !mask || (mask & static_cast<uint8_t>(mask - 1)) != 0 ||
        (field_mask != mask && field_mask != 0xFF)) return false;
    out = {field.offset + byte, mask, byte, descriptor_offset};
    return true;
}
// The old 32-bit UE4 builds differ in BoolProperty descriptor layout. The
// paired post-process fields must have consecutive bits/bytes at one unique
// candidate descriptor offset, otherwise we leave native memory untouched.
bool ResolveAdjacentBool(void* type, const wchar_t* target,
                         const wchar_t* neighbor, BoolBits& chosen) {
    const Field a = FindField(type, target, L"BoolProperty");
    const Field b = FindField(type, neighbor, L"BoolProperty");
    if (!a.descriptor || !b.descriptor) return false;
    const size_t candidates[] = {0x50, 0x54, 0x58, 0x5C, 0x60, 0x64};
    int count = 0;
    for (size_t offset : candidates) {
        BoolBits p{}, q{};
        if (!ParseBool(a, offset, p) || !ParseBool(b, offset, q)) continue;
        const int distance = p.address_offset - q.address_offset;
        if (distance < -1 || distance > 1) continue;
        const bool consecutive = distance == 0 ?
            (p.mask == static_cast<uint8_t>(q.mask << 1) ||
             q.mask == static_cast<uint8_t>(p.mask << 1)) :
            (distance == 1 && q.mask == 0x80 && p.mask == 1) ||
            (distance == -1 && p.mask == 0x80 && q.mask == 1);
        if (consecutive) { chosen = p; ++count; }
    }
    return count == 1;
}
// Light CastShadows is a BoolProperty; only the reflected SetCastShadows is
// used for writes. Never directly patch a bit in a light component.
bool ReadCastShadows(bool& value) {
    if (!s_light.component.ptr || s_light.prop_offset < 0 || !s_light.mask) return false;
    const auto* byte = static_cast<const uint8_t*>(s_light.component.ptr) +
                       s_light.prop_offset + s_light.byte_offset;
    if (!IsReadable(byte, 1)) return false;
    value = (*byte & s_light.mask) != 0;
    return true;
}
bool ConfigureLight() {
    auto& light = s_light;
    const auto* object = static_cast<const UObjectHeader*>(light.component.ptr);
    const Field cast = FindField(object->class_ptr, L"CastShadows", L"BoolProperty");
    if (!cast.descriptor) { Trace(true, 1, L"CastShadows property unavailable"); return false; }
    // Accept a single metadata descriptor whose bit value is consistent with
    // the known default (the shipped directional light casts shadows).
    const size_t candidates[] = {0x50, 0x54, 0x58, 0x5C, 0x60, 0x64};
    int matches = 0;
    BoolBits selected{};
    for (size_t desc : candidates) {
        BoolBits bits{};
        if (!ParseBool(cast, desc, bits) || bits.address_offset > 0x1FFF) continue;
        const auto* byte = static_cast<const uint8_t*>(light.component.ptr) + bits.address_offset;
        if (!IsReadable(byte, 1)) continue;
        // Never require the value to be true: maps may disable native shadows.
        selected = bits;
        ++matches;
    }
    if (matches != 1) { Trace(true, 2, L"CastShadows bit layout ambiguous"); return false; }
    light.prop_offset = cast.offset;
    light.byte_offset = selected.byte_offset;
    light.mask = selected.mask;
    light.setter = FindReflectedFunction(light.component.ptr, L"SetCastShadows");
    if (!light.setter) { Trace(true, 3, L"SetCastShadows not reflected"); return false; }
    const Field arg = FindField(light.setter, L"bNewValue", L"BoolProperty");
    if (!arg.descriptor || arg.offset < 0 || arg.offset > 120) {
        Trace(true, 4, L"SetCastShadows signature unsupported"); return false;
    }
    light.setter_arg = arg.offset;
    Trace(true, 5, L"directional light and setter resolved");
    return true;
}
bool WriteCastShadows(bool visible) {
    if (!g_process_event || !s_light.setter || s_light.setter_arg < 0) return false;
    alignas(16) std::array<uint8_t, 128> args{};
    args[static_cast<size_t>(s_light.setter_arg)] = visible ? 1 : 0;
    g_process_event(s_light.component.ptr, s_light.setter, args.data());
    bool actual = false;
    return ReadCastShadows(actual) && actual == visible;
}
void UpdateLight() {
    if (!s_light.component.ptr || !s_light.setter) return;
    bool actual = false;
    if (!ReadCastShadows(actual)) return;
    if (!s_config.visible) {
        if (!s_light.changed) s_light.original = actual;
        if (actual && GetTickCount64() >= s_light.retry_after) {
            if (!WriteCastShadows(false)) {
                s_light.retry_after = GetTickCount64() + kRetryMs;
                Trace(true, 6, L"shadow disable failed (will retry)");
                return;
            }
            Trace(true, 7, L"directional shadows disabled and verified");
        }
        s_light.changed = true;
        s_light.last_written = false;
    } else if (s_light.changed) {
        if (actual == s_light.last_written && GetTickCount64() >= s_light.retry_after) {
            if (!WriteCastShadows(s_light.original)) {
                s_light.retry_after = GetTickCount64() + kRetryMs;
                Trace(true, 8, L"shadow restoration failed (will retry)");
                return;
            }
            Trace(true, 9, L"original directional shadow setting restored");
        } else if (actual == s_light.last_written) return;
        s_light.changed = false;
        s_light.retry_after = 0;
    }
}
LinearColor FromRgb(uint32_t rgb) {
    auto linear = [](uint32_t value) {
        const float c = static_cast<float>(value & 255u) / 255.0f;
        return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    };
    return {linear(rgb >> 16), linear(rgb >> 8), linear(rgb), 1.0f};
}
bool Similar(const LinearColor& a, const LinearColor& b) {
    return std::fabs(a.r-b.r) < 0.002f && std::fabs(a.g-b.g) < 0.002f &&
           std::fabs(a.b-b.b) < 0.002f;
}
LinearColor* TintAddress() {
    if (s_post.settings_offset < 0 || s_post.tint_offset < 0) return nullptr;
    auto* base = static_cast<uint8_t*>(s_post.volume.ptr) +
                 s_post.settings_offset + s_post.tint_offset;
    return IsReadable(base, sizeof(LinearColor)) && IsWritable(base, sizeof(LinearColor))
        ? reinterpret_cast<LinearColor*>(base) : nullptr;
}
uint8_t* OverrideAddress() {
    if (s_post.settings_offset < 0 || s_post.override_offset < 0) return nullptr;
    auto* base = static_cast<uint8_t*>(s_post.volume.ptr) +
                 s_post.settings_offset + s_post.override_offset;
    return IsReadable(base, 1) && IsWritable(base, 1) ? base : nullptr;
}
bool ConfigurePost() {
    auto& post = s_post;
    const Field settings = FindField(static_cast<UObjectHeader*>(post.volume.ptr)->class_ptr,
                                     L"Settings", L"StructProperty");
    if (settings.offset < 0 || settings.offset > 0x1C00) {
        Trace(false, 1, L"PostNormal Settings not reflected"); return false;
    }
    const Field tint = FindField(post.settings_struct.ptr, L"AmbientCubemapTint",
                                 L"StructProperty");
    BoolBits bits{};
    if (tint.offset < 0 || tint.offset > 0x1C00 ||
        !ResolveAdjacentBool(post.settings_struct.ptr, L"bOverride_AmbientCubemapTint",
                             L"bOverride_AmbientCubemapIntensity", bits)) {
        Trace(false, 2, L"ambient tint/override layout unverified"); return false;
    }
    post.settings_offset = settings.offset;
    post.tint_offset = tint.offset;
    post.override_offset = bits.address_offset;
    post.override_mask = bits.mask;
    if (!TintAddress() || !OverrideAddress()) {
        Trace(false, 3, L"PostNormal tint address not writable"); return false;
    }
    Trace(false, 4, L"PostNormal ambient cubemap tint verified");
    return true;
}
void UpdatePost() {
    auto* tint = TintAddress();
    auto* flag = OverrideAddress();
    if (!tint || !flag || !s_post.override_mask) return;
    LinearColor current{};
    std::memcpy(&current, tint, sizeof(current));
    for (float c : {current.r, current.g, current.b, current.a})
        if (!std::isfinite(c) || c < 0.0f || c > 100.0f) return;
    const bool current_override = (*flag & s_post.override_mask) != 0;
    if (s_config.tint_enabled) {
        if (!s_post.changed) {
            s_post.original = current;
            s_post.original_override = current_override;
        }
        LinearColor desired = FromRgb(s_config.tint_rgb);
        desired.a = current.a;
        if (!Similar(current, desired) || !current_override) {
            if (GetTickCount64() < s_post.retry_after) return;
            *flag |= s_post.override_mask;
            std::memcpy(tint, &desired, sizeof(desired));
            LinearColor after{}; std::memcpy(&after, tint, sizeof(after));
            if (!Similar(after, desired) || (*flag & s_post.override_mask) == 0) {
                s_post.retry_after = GetTickCount64() + kRetryMs;
                Trace(false, 5, L"ambient tint write not verified");
                return;
            }
            Trace(false, 6, L"ambient shadow-fill tint applied");
        }
        s_post.last_written = desired;
        s_post.changed = true;
    } else if (s_post.changed) {
        // Do not undo a newer engine colour; only restore bytes we own.
        if (Similar(current, s_post.last_written))
            std::memcpy(tint, &s_post.original, sizeof(s_post.original));
        if (s_post.original_override) *flag |= s_post.override_mask;
        else *flag &= static_cast<uint8_t>(~s_post.override_mask);
        s_post.changed = false;
        s_post.retry_after = 0;
        Trace(false, 7, L"original post-process ambient tint restored");
    }
}
void Discover(const GlobalObjectArray* array) {
    if (!array || s_scan_index < 0) return;
    const ULONGLONG began = GetTickCount64();
    const int32_t end = (std::min)(array->num_elements, s_scan_index + kBatchSize);
    for (; s_scan_index < end; ++s_scan_index) {
        if ((s_scan_index & 127) == 0 && GetTickCount64() - began > kScanBudgetMs) break;
        const auto& item = array->objects[s_scan_index];
        void* ptr = item.object;
        if (!IsReadable(ptr, sizeof(UObjectHeader)) ||
            (static_cast<uint32_t>(static_cast<UObjectHeader*>(ptr)->flags) & 0x30u) != 0)
            continue;
        const std::wstring name = ObjectName(ptr);
        if (!s_light.component.ptr && name == L"LightComponent0" &&
            IsA(ptr, L"DirectionalLightComponent")) {
            void* actor = static_cast<UObjectHeader*>(ptr)->outer;
            if (IsA(actor, L"DirectionalLight") &&
                ObjectName(actor).rfind(L"Default__", 0) != 0) {
                s_light.component = Ref(array, ptr);
                s_light.owner = Ref(array, actor);
                if (!Live(array, s_light.owner) || !ConfigureLight()) s_light = {};
            }
        }
        if (!s_post.volume.ptr && name == L"PostNormal" &&
            IsA(ptr, L"PostProcessVolume")) {
            s_post.volume = Ref(array, ptr);
        }
        if (!s_post.settings_struct.ptr && name == L"PostProcessSettings" &&
            IsA(ptr, L"ScriptStruct")) {
            s_post.settings_struct = Ref(array, ptr);
        }
    }
    if (s_post.volume.ptr && s_post.settings_struct.ptr &&
        s_post.settings_offset < 0 && !ConfigurePost()) {
        // Try again after the next bounded scan; avoid repeatedly resolving
        // the same invalid reflected metadata every frame.
        s_rescan_after = GetTickCount64() + 3000;
    }
    if (s_scan_index >= array->num_elements) {
        s_scan_index = -1;
        s_rescan_after = GetTickCount64() + 3000;
        if (!s_light.component.ptr) Trace(true, 10, L"directional light not found");
        if (!s_post.volume.ptr || !s_post.settings_struct.ptr)
            Trace(false, 10, L"PostNormal/Settings structure not found");
    }
}
Config& Settings() {
    if (s_config.loaded || g_settings_path.empty()) return s_config;
    s_config.visible = GetPrivateProfileIntW(kSection, L"Visible", 1,
                                              g_settings_path.c_str()) != 0;
    s_config.tint_enabled = GetPrivateProfileIntW(kSection, L"AmbientEnabled", 0,
                                                   g_settings_path.c_str()) != 0;
    wchar_t hex[16]{};
    GetPrivateProfileStringW(kSection, L"AmbientHex", L"FFFFFF", hex,
                             static_cast<DWORD>(std::size(hex)), g_settings_path.c_str());
    const wchar_t* value = hex[0] == L'#' ? hex+1 : hex;
    wchar_t* stop = nullptr;
    const unsigned long parsed = wcstoul(value, &stop, 16);
    if (stop && !*stop && wcslen(value) == 6 && parsed <= 0xFFFFFFu)
        s_config.tint_rgb = static_cast<uint32_t>(parsed);
    s_config.loaded = true;
    return s_config;
}
void SaveSoon() {
    Settings().dirty = true;
    s_config.save_after = GetTickCount64() + 350;
}
void Flush() {
    if (!s_config.dirty || g_settings_path.empty() ||
        GetTickCount64() < s_config.save_after) return;
    wchar_t hex[7]{};
    swprintf_s(hex, L"%06X", s_config.tint_rgb);
    const bool saved = QueueSettingsValue(kSection, L"Visible",
        s_config.visible ? L"1" : L"0") &&
        QueueSettingsValue(kSection, L"AmbientEnabled",
        s_config.tint_enabled ? L"1" : L"0") &&
        QueueSettingsValue(kSection, L"AmbientHex", hex);
    s_config.dirty = !saved;
    if (!saved) s_config.save_after = GetTickCount64() + 3000;
}
DWORD WINAPI PickerThread(LPVOID) {
    COLORREF swatches[16]{};
    CHOOSECOLORW chooser{};
    chooser.lStructSize = sizeof(chooser);
    chooser.lpCustColors = swatches;
    const uint32_t rgb = s_picker_initial.load();
    chooser.rgbResult = RGB((rgb>>16)&255, (rgb>>8)&255, rgb&255);
    chooser.Flags = CC_ANYCOLOR | CC_FULLOPEN | CC_RGBINIT;
    if (ChooseColorW(&chooser)) {
        const auto selected = chooser.rgbResult;
        s_picker_result.store((static_cast<uint32_t>(GetRValue(selected)) << 16) |
            (static_cast<uint32_t>(GetGValue(selected)) << 8) | GetBValue(selected));
    } else s_picker_result.store(CommDlgExtendedError() ? kPickerError : kPickerCancelled);
    s_picker_open.store(false);
    return 0;
}
void PollPicker() {
    const uint32_t result = s_picker_result.exchange(kPickerEmpty);
    if (result == kPickerEmpty) return;
    if (result <= 0xFFFFFFu) SetShadowAmbientColourHex(result);
    else if (result == kPickerError) Notify(L"Shadow colour picker could not open");
}
} // namespace

bool AreShadowsVisible() { return Settings().visible; }
void SetShadowsVisible(bool value) {
    if (Settings().visible == value) return;
    s_config.visible = value;
    SaveSoon();
    Notify(value ? L"Directional shadows enabled" : L"Directional shadows disabled");
}
bool IsShadowAmbientColourEnabled() { return Settings().tint_enabled; }
void SetShadowAmbientColourEnabled(bool value) {
    if (Settings().tint_enabled == value) return;
    s_config.tint_enabled = value;
    SaveSoon();
    Notify(value ? L"Shadow ambient tint enabled" : L"Original ambient tint restored");
}
std::wstring GetShadowAmbientColourHex() {
    wchar_t hex[8]{};
    swprintf_s(hex, L"#%06X", Settings().tint_rgb);
    return hex;
}
void SetShadowAmbientColourHex(uint32_t rgb) {
    if (rgb > 0xFFFFFFu) return;
    Settings();
    s_config.tint_rgb = rgb;
    s_config.tint_enabled = true;
    SaveSoon();
    Notify(L"Shadow ambient tint set (affects ambient lighting too)");
}
void OpenShadowAmbientColourPicker() {
    if (s_picker_open.exchange(true)) {
        Notify(L"Shadow colour picker already open"); return;
    }
    PollPicker();
    s_picker_initial.store(Settings().tint_rgb);
    s_picker_result.store(kPickerEmpty);
    HANDLE thread = CreateThread(nullptr, 0, PickerThread, nullptr, 0, nullptr);
    if (thread) CloseHandle(thread);
    else { s_picker_open.store(false); Notify(L"Could not open shadow picker thread"); }
}
void ResetShadowCustomisation() {
    SetShadowsVisible(true);
    SetShadowAmbientColourEnabled(false);
}
void UpdateShadowCustomisation(MyCharacter* player) {
    PollPicker();
    Settings();
    Flush();
    const bool wants_light = !s_config.visible || s_light.changed;
    const bool wants_tint = s_config.tint_enabled || s_post.changed;
    if ((!wants_light && !wants_tint) || !player ||
        !IsReadable(player, sizeof(MyCharacter))) return;
    // These static overrides are unlike the per-frame dynamic sky material.
    // Static shadow overrides normally survive native ticks. Retain a slow
    // watchdog for unexpected engine changes/recreated components, not 20 Hz
    // polling. Edits, player changes and discovery still bypass the timer.
    struct Request { MyCharacter* player; bool visible, tint; uint32_t rgb; };
    static Request last{};
    static ULONGLONG next_verify = 0;
    const Request request{player, s_config.visible, s_config.tint_enabled, s_config.tint_rgb};
    const ULONGLONG now = GetTickCount64();
    const bool changed = request.player != last.player || request.visible != last.visible ||
        request.tint != last.tint || request.rgb != last.rgb;
    const bool missing_target = (wants_light && !s_light.component.ptr) ||
        (wants_tint && (!s_post.volume.ptr || !s_post.settings_struct.ptr ||
                       s_post.settings_offset < 0));
    const bool scanning = missing_target && s_scan_index >= 0;
    if (!changed && !scanning && now < next_verify) return;
    last = request;
    next_verify = now + 1000;
    const auto* array = Objects();
    if (!array) return;
    if (s_light.component.ptr && (!Live(array, s_light.component) ||
        !Live(array, s_light.owner) ||
        static_cast<UObjectHeader*>(s_light.component.ptr)->outer != s_light.owner.ptr)) {
        s_light = {};
        s_scan_index = 0;
    }
    if (s_post.volume.ptr && !Live(array, s_post.volume)) {
        s_post = {};
        s_scan_index = 0;
    }
    if (s_post.settings_struct.ptr && !Live(array, s_post.settings_struct)) {
        s_post = {};
        s_scan_index = 0;
    }
    if ((!s_light.component.ptr && wants_light) ||
        ((!s_post.volume.ptr || !s_post.settings_struct.ptr ||
          s_post.settings_offset < 0) && wants_tint)) {
        if (s_scan_index < 0 && (changed || now >= s_rescan_after))
            s_scan_index = 0;
        Discover(array);
    }
    if (wants_light && Live(array, s_light.component)) UpdateLight();
    if (wants_tint && Live(array, s_post.volume) &&
        Live(array, s_post.settings_struct)) UpdatePost();
}
} // namespace refunct

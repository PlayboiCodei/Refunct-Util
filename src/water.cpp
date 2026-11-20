#include "refunct/internal_api.hpp"
#include <commdlg.h>
#include <unordered_map>
#include "refunct/colour_work_budget.hpp"
#include "refunct/discovery_watermark.hpp"

namespace refunct {
namespace {
// Verified against the shipped Refunct-WindowsNoEditor.pak:
// BP_WaterSurface and WorldBase both reference the
// ocean material. WorldBase also references M_Infinite_Ocean_Linux and has
// Blueprint-created mesh components. The Linux MIC inherits the base_color
// vector parameter from M_Infinite_Ocean_Inst.
// Only tint a per-component MID, never a shared material or world geometry.
constexpr size_t kMaxWaterActors = 256;
// Discovery is a one-off/periodic operation, not part of changing a colour.
// Process larger batches but cap CPU time to avoid stuttering during loads.
constexpr int32_t kScanBatch = 2048;
constexpr ULONGLONG kScanCpuBudgetMs = 2;
constexpr size_t kApplyBatch = 6;
constexpr ULONGLONG kUpdateIntervalMs = 30;
constexpr ULONGLONG kSettingsSaveDebounceMs = 350;
// Do not steal CPU time for a background UObject scan while a user is
// actively adjusting a colour on an already-discovered water surface.
constexpr ULONGLONG kSurfaceColourEditQuietMs = 900;

struct WaterSettings {
    bool loaded = false;
    bool removed = false;           // Hide only verified ocean-rendering meshes.
    bool remove_all = false;        // Master override; preserves individual preferences.
    bool swimming_disabled = false; // Force falling instead of swimming.
    int height_offset = 0;          // Visual surface height, in Unreal units.
    bool custom = false;
    uint32_t rgb = 0x30B8FF;
    bool underwater_custom = false;
    bool tint_removed = false; // Neutral PostWater SceneColorTint (restorable).
    bool splashes_removed = false; // Hide water-only particle effects (visual only).
    bool reflections_visible = true; // Preserve the original planar-reflection visibility.
    bool reflection_geometry_visible = true; // Legacy wall/lift setting; keep INI key.
    bool reflection_buttons_visible = true;
    bool reflection_pipes_visible = true;
    bool reflection_beacons_visible = true;
    bool reflection_jump_pads_visible = true;
    bool reflection_cubes_visible = true;
    uint32_t underwater_rgb = 0x30B8FF;
    int surface_intensity_percent = 100; // 0=original, 100=selected colour, >100=HDR
    int underwater_intensity_percent = 100; // 0=original, 100=full tint, >100=boosted HDR
};
WaterSettings s_settings{};
// Each settings edit only writes the INI keys that actually changed.
enum WaterSaveField : uint32_t {
    SurfaceHex = 1 << 0, SurfaceEnabled = 1 << 1,
    UnderwaterHex = 1 << 2, UnderwaterEnabled = 1 << 3,
    UnderwaterIntensity = 1 << 4, SurfaceIntensity = 1 << 5,
    SurfaceRemoved = 1 << 6, SwimmingDisabled = 1 << 7,
    SurfaceHeight = 1 << 8, RemoveTint = 1 << 9, RemoveSplashes = 1 << 10,
    RemoveAll = 1 << 11, ReflectionsVisible = 1 << 12,
    ReflectionGeometryVisible = 1 << 14,
    ReflectionButtonsVisible = 1 << 15,
    ReflectionPipesVisible = 1 << 16,
    ReflectionBeaconsVisible = 1 << 17,
    ReflectionJumpPadsVisible = 1 << 18,
    ReflectionCubesVisible = 1 << 19
};
uint32_t s_settings_save_dirty = 0;
ULONGLONG s_settings_save_due = 0;

struct WaterTarget {
    void* actor = nullptr;
    int32_t index = -1;
    int32_t serial = 0;
    void* mesh = nullptr;
    void* original_material = nullptr;
    void* dynamic_material = nullptr;
    FNameView colour_parameter{};
    uint32_t last_rgb = 0;
    int last_intensity_percent = -1;
    LinearColor native_colour{1.0f, 1.0f, 1.0f, 1.0f};
    bool native_colour_known = false;
    ULONGLONG next_verify = 0;
    bool unsupported = false;
    // Keep ownership information until the original material is actually
    // restored. Never treat an unsuccessful SetMaterial as Game Default.
    bool cleared_on_restore = false;
    bool restore_warning_logged = false;
    bool original_visible = true;
    bool visibility_modified = false;
    Vec3 original_location{};
    bool original_location_known = false;
    bool height_modified = false;
    ULONGLONG geometry_retry_after = 0;
    ULONGLONG geometry_verify_after = 0;
    bool last_removed = false;
    int last_height_offset = 0;
};
std::vector<WaterTarget> s_targets;
MyCharacter* s_player = nullptr;
int32_t s_scan_cursor = -1;
size_t s_apply_cursor = 0;
ULONGLONG s_next_scan = 0;
DiscoveryWatermark s_discovery_watermark{};
ULONGLONG s_next_update = 0;
ULONGLONG s_surface_edit_quiet_until = 0;
bool s_restore_pending = false;
bool s_reported = false;
bool s_warning_logged = false;
bool s_applied_logged = false;
bool s_world_geometry_warning_logged = false;
size_t s_examined_meshes = 0;
size_t s_ocean_meshes = 0;
FNameView s_ocean_colour_parameter{};
bool s_ocean_parameter_ready = false;

// The shipped WorldBase.umap contains a native PlanarReflection actor with a
// NewPlanarReflectionComponent. This is NOT the DirectionalLight CastShadows
// setting, the shared ocean material or the underwater post-process volume.
struct WaterReflectionTarget {
    void* component = nullptr;
    int32_t index = -1;
    int32_t serial = 0;
    bool original_visible = true;
    bool modified = false;
    bool warning_logged = false;
    bool originally_active = false;
    bool restored_this_cycle = false;
};
// Used by the capture-only geometry update before its definition below.
bool WaterReflectionTargetValid(const WaterReflectionTarget& target,
                                const GlobalObjectArray* objects);
std::vector<WaterReflectionTarget> s_reflection_targets;
std::unordered_map<void*, bool> s_reflection_class_cache;
MyCharacter* s_reflection_player = nullptr;
int32_t s_reflection_scan_cursor = -1;
bool s_reflection_scan_complete = false;
// Kept across player changes until the requested ON state has been applied.
bool s_reflection_restore_pending = false;
ULONGLONG s_reflection_retry_after = 0;
ULONGLONG s_reflection_next_verify = 0;
bool s_reflection_apply_requested = false;
bool s_reflection_missing_logged = false;
constexpr int32_t kReflectionScanBatch = 768;
constexpr size_t kMaxReflectionTargets = 8;
// Reflected geometry is excluded from ONLY the planar capture, never from the
// main camera or collision. Retain each captured weak pointer until a verified
// restoration; never clear unrelated game-owned hidden lists.
struct CaptureHiddenWeakRef { int32_t index = -1; int32_t serial = 0; };
static_assert(sizeof(CaptureHiddenWeakRef) == 8);
enum class ReflectionKind : uint8_t { Walls = 0, Buttons = 1, Pipes = 2, Beacons = 3, JumpPads = 4, Cubes = 5 };
struct ReflectionGeometryCandidate {
    void* component = nullptr;
    int32_t index = -1;
    int32_t serial = 0;
    ReflectionKind kind = ReflectionKind::Walls;
};
uint32_t s_geometry_applied_mask = ~0u;
WaterSettings& WaterConfig();
uint32_t WaterReflectionHideMask() {
    const auto& settings = WaterConfig();
    return (settings.reflection_geometry_visible ? 0u : 1u) |
           (settings.reflection_buttons_visible ? 0u : 2u) |
           (settings.reflection_pipes_visible ? 0u : 4u) |
           (settings.reflection_beacons_visible ? 0u : 8u) |
           (settings.reflection_jump_pads_visible ? 0u : 16u) |
           (settings.reflection_cubes_visible ? 0u : 32u);
}
std::vector<ReflectionGeometryCandidate> s_geometry_candidates;
std::vector<CaptureHiddenWeakRef> s_geometry_hidden;
void* s_geometry_capture = nullptr;
int32_t s_geometry_capture_index = -1;
int32_t s_geometry_capture_serial = 0;
MyCharacter* s_geometry_player = nullptr;
int32_t s_geometry_scan_cursor = -1;
size_t s_geometry_apply_cursor = 0;
bool s_geometry_scan_complete = false;
bool s_geometry_unsupported_logged = false;
bool s_geometry_enabled_logged = false;
// A rejected native layout must not trigger a reflection walk every frame.
ULONGLONG s_geometry_retry_after = 0;
constexpr size_t kMaxGeometryCandidates = 512;
constexpr size_t kGeometryApplyBatch = 4;



// Water volumes, unlike rendered ocean meshes, determine whether Unreal
// automatically enters MOVE_Swimming. Changing the mode alone allows native
// collision/landing to put the player straight back into swimming.
struct SwimVolumeTarget {
    void* actor = nullptr;
    int32_t index = -1;
    int32_t serial = 0;
    int32_t flag_offset = -1;
    uint8_t flag_mask = 0;
};
std::vector<SwimVolumeTarget> s_swim_volumes;
MyCharacter* s_swim_player = nullptr;
int32_t s_swim_scan_cursor = -1;
ULONGLONG s_swim_next_scan = 0;
bool s_swim_resolution_warned = false;
constexpr size_t kMaxSwimVolumes = 64;

// The visible ocean is a StaticMeshComponent. Swimming is controlled by a
// separate APhysicsVolume brush. Extend a verified native brush upward rather
// than translating it, which would incorrectly remove swimming at the bottom.
// Retain original root scale, location, mobility and bounds for restoration.
struct WaterHeightVolumeTarget {
    void* actor = nullptr;
    int32_t actor_index = -1;
    int32_t actor_serial = 0;
    void* root = nullptr;
    int32_t root_index = -1;
    int32_t root_serial = 0;
    Vec3 original_location{};
    Vec3 original_scale{1.0f, 1.0f, 1.0f};
    float baseline_bottom_z = 0.0f;
    float baseline_height = 0.0f;
    uint8_t original_mobility = 0;
    bool geometry_known = false;
    bool mobility_owned = false;
    bool scale_modified = false;
    bool height_modified = false;
    ULONGLONG retry_after = 0;
};
std::vector<WaterHeightVolumeTarget> s_height_volumes;
size_t s_height_volume_apply_cursor = 0;
int32_t s_height_volume_scan_cursor = -1;
ULONGLONG s_height_volume_next_scan = 0;
ULONGLONG s_height_volume_next_update = 0;
bool s_height_volume_warning_logged = false;
bool s_height_volume_success_logged = false;
constexpr size_t kMaxHeightVolumes = 64;
constexpr size_t kHeightApplyBatch = 4;

// A cooked brush/PhysicsVolume may reject SetWorldLocation because its
// collision is static. A reversible fallback changes only the *current*
// DefaultPhysicsVolume water bit while the player is inside the newly raised
// slice of ocean. This keeps UE4 PhysSwimming's own bWaterVolume check true
// during collision/landing instead of forcing MOVE_Swimming every frame.
// It is deliberately disabled for lowered water, fly, and Remove Water.
struct RaisedWaterVirtualVolume {
    SwimVolumeTarget volume{};
    bool active = false;
    bool owns_swimming_mode = false;
};
RaisedWaterVirtualVolume s_raised_water{};
bool s_raised_water_logged = false;
bool s_raised_water_warning_logged = false;

// BP_MyCharacter in Refunct-WindowsNoEditor.pak references the native
// FixedWaterHeight field as well as IsPositionInWater/IsSwimming. Unlike the
// cooked, static WaterVolume brush, a verified character float is reversible.
// Resolve the field by reflected name AND FloatProperty type; never assume a
// hard-coded offset from the package's on-disk name table.
struct NativeFixedWaterline {
    MyCharacter* player = nullptr;
    int32_t index = -1;
    int32_t serial = 0;
    void* klass = nullptr;
    int32_t offset = -1;
    float original = 0.0f;
    float last_written = 0.0f;
    bool modified = false;
};
NativeFixedWaterline s_fixed_waterline{};
ULONGLONG s_fixed_waterline_retry_after = 0;
bool s_fixed_waterline_warned = false;
bool s_fixed_waterline_logged = false;
// Only report a change between real water and the added virtual layer once.
bool s_fixed_waterline_old_volume_logged = false;
bool s_fixed_waterline_virtual_logged = false;

// Cache the world default volume while it is verified. This permits the
// water flag to be prepared *before* entering that volume from WaterVolume_1.
// Never retain a raw pointer across UObject serial changes or level reloads.
SwimVolumeTarget s_raised_default_hint{};
int32_t s_raised_default_scan_cursor = 0;
bool s_raised_prearm_logged = false;
bool s_raised_prearm_lookup_warned = false;

// Splash suppression is visual only. Retain only live, identified emitter
// components, never blanket-disable all particles or collision events.
struct SplashTarget {
    void* component = nullptr;
    int32_t index = -1;
    int32_t serial = 0;
    bool original_visible = true;
};
std::vector<SplashTarget> s_splash_targets;
int32_t s_splash_scan_cursor = -1;
ULONGLONG s_splash_next_scan = 0;
ULONGLONG s_splash_retry_after = 0;
bool s_splash_logged = false;
constexpr size_t kMaxSplashTargets = 128;



// The shipped WorldBase map has separate PostNormal and PostWater post-process
// volumes. Tint only the PostWater Settings.SceneColorTint struct member, NOT
// the shared ocean surface material, PostNormal or global camera settings.
struct UnderwaterTintTarget {
    void* actor = nullptr;
    int32_t index = -1;
    int32_t serial = 0;
    int32_t settings_offset = -1;
    int32_t tint_offset = -1;
    // FPostProcessSettings requires its matching bOverride flag to be set.
    // UE4 stores this native BoolProperty as a packed bit (not a full bool).
    int32_t override_offset = -1;
    uint8_t override_mask = 0;
    bool original_override = false;
    LinearColor original{};
    LinearColor last_written{};
    bool applied = false;
    // A cooked PostWater brush does not cover raised water. Temporarily make
    // only this specific post-process volume unbound while the camera is in
    // the new layer; preserve its original bitfields for exact restoration.
    int32_t unbound_offset = -1;
    uint8_t unbound_mask = 0;
    int32_t enabled_offset = -1;
    uint8_t enabled_mask = 0;
    bool original_unbound = false;
    bool original_enabled = false;
    bool raised_effect_owned = false;
};
UnderwaterTintTarget s_underwater_target{};
void* s_underwater_settings_struct = nullptr;
MyCharacter* s_underwater_player = nullptr;
int32_t s_underwater_scan_cursor = -1;
ULONGLONG s_underwater_next_scan = 0;
ULONGLONG s_underwater_next_apply = 0;
bool s_underwater_restore_pending = false;
bool s_underwater_logged = false;
bool s_underwater_resolution_warned = false;
bool s_raised_postprocess_warning_logged = false;
bool s_raised_postprocess_logged = false;


constexpr uint32_t kNoPickerResult = 0xFFFFFFFFu;
constexpr uint32_t kPickerCancelled = 0x01000000u;
std::atomic<uint32_t> s_picker_result{kNoPickerResult};
std::atomic<uint32_t> s_picker_initial{0};
std::atomic<uint8_t> s_picker_target{0}; // 0=surface, 1=underwater view
std::atomic<bool> s_picker_open{false};

std::wstring WaterHex(uint32_t rgb) {
    wchar_t out[8]{};
    swprintf_s(out, L"#%06X", rgb & 0xFFFFFFu);
    return out;
}
uint32_t WaterParseHex(const wchar_t* text, uint32_t fallback) {
    if (!text) return fallback;
    if (*text == L'#') ++text;
    if (wcslen(text) != 6) return fallback;
    uint32_t result = 0;
    for (int i = 0; i < 6; ++i) {
        const wchar_t c = text[i];
        const int digit = c >= L'0' && c <= L'9' ? c - L'0' :
            c >= L'A' && c <= L'F' ? c - L'A' + 10 :
            c >= L'a' && c <= L'f' ? c - L'a' + 10 : -1;
        if (digit < 0) return fallback;
        result = (result << 4) | static_cast<uint32_t>(digit);
    }
    return result;
}
WaterSettings& WaterConfig() {
    if (s_settings.loaded || g_settings_path.empty()) return s_settings;
    wchar_t colour[32]{};
    GetPrivateProfileStringW(L"WaterColours", L"Hex", L"#30B8FF",
        colour, static_cast<DWORD>(std::size(colour)), g_settings_path.c_str());
    s_settings.rgb = WaterParseHex(colour, s_settings.rgb);
    s_settings.custom = GetPrivateProfileIntW(L"WaterColours", L"CustomEnabled",
        0, g_settings_path.c_str()) != 0;
    s_settings.surface_intensity_percent = (std::max)(0,
        static_cast<int>(GetPrivateProfileIntW(L"WaterColours",
            L"SurfaceIntensityPercent", 100, g_settings_path.c_str())));
    wchar_t underwater_hex[32]{};
    GetPrivateProfileStringW(L"WaterColours", L"UnderwaterHex", L"#30B8FF",
        underwater_hex, static_cast<DWORD>(std::size(underwater_hex)),
        g_settings_path.c_str());
    s_settings.underwater_rgb = WaterParseHex(underwater_hex,
                                              s_settings.underwater_rgb);
    s_settings.underwater_custom = GetPrivateProfileIntW(
        L"WaterColours", L"UnderwaterCustomEnabled", 0,
        g_settings_path.c_str()) != 0;
    // Preserve intensities above 100% across launches. Zero is the only
    // lower bound; no arbitrary high-end clamp is applied.
    s_settings.underwater_intensity_percent = (std::max)(0,
        static_cast<int>(GetPrivateProfileIntW(L"WaterColours",
            L"UnderwaterIntensityPercent", 100, g_settings_path.c_str())));
    s_settings.tint_removed = GetPrivateProfileIntW(L"WaterColours", L"RemoveUnderwaterTint", 0,
        g_settings_path.c_str()) != 0;
    s_settings.splashes_removed = GetPrivateProfileIntW(L"WaterColours", L"RemoveWaterSplashes", 0,
        g_settings_path.c_str()) != 0;
    s_settings.reflections_visible = GetPrivateProfileIntW(
        L"WaterColours", L"ReflectionsVisible", 1, g_settings_path.c_str()) != 0;
    s_settings.reflection_geometry_visible = GetPrivateProfileIntW(
        L"WaterColours", L"ReflectionGeometryVisible", 1, g_settings_path.c_str()) != 0;
    s_settings.reflection_buttons_visible = GetPrivateProfileIntW(
        L"WaterColours", L"ReflectionButtonsVisible", 1, g_settings_path.c_str()) != 0;
    s_settings.reflection_pipes_visible = GetPrivateProfileIntW(
        L"WaterColours", L"ReflectionPipesVisible", 1, g_settings_path.c_str()) != 0;
    s_settings.reflection_beacons_visible = GetPrivateProfileIntW(
        L"WaterColours", L"ReflectionBeaconsVisible", 1, g_settings_path.c_str()) != 0;
    s_settings.reflection_jump_pads_visible = GetPrivateProfileIntW(
        L"WaterColours", L"ReflectionJumpPadsVisible", 1, g_settings_path.c_str()) != 0;
    s_settings.reflection_cubes_visible = GetPrivateProfileIntW(
        L"WaterColours", L"ReflectionCubesVisible", 1, g_settings_path.c_str()) != 0;
    s_settings.remove_all = GetPrivateProfileIntW(L"WaterGameplay", L"RemoveAll", 0,
        g_settings_path.c_str()) != 0;
    s_settings.removed = GetPrivateProfileIntW(L"WaterGameplay", L"RemoveSurface", 0,
        g_settings_path.c_str()) != 0;
    s_settings.swimming_disabled = GetPrivateProfileIntW(L"WaterGameplay", L"DisableSwimming", 0,
        g_settings_path.c_str()) != 0;
    s_settings.height_offset = (std::clamp)(static_cast<int>(
        GetPrivateProfileIntW(L"WaterGameplay", L"HeightOffset", 0,
                              g_settings_path.c_str())), -100000, 100000);
    s_settings.loaded = true;
    return s_settings;
}
// Effective state is separate from saved individual options. A master override
// must not destroy the user's selected tint, splash, swimming or surface modes.
bool WaterSurfaceHidden() { const auto& s = WaterConfig(); return s.remove_all || s.removed; }
bool WaterNoSwimming() { const auto& s = WaterConfig(); return s.remove_all || s.swimming_disabled; }
bool WaterNoTint() { const auto& s = WaterConfig(); return s.remove_all || s.tint_removed; }
bool WaterNoSplashes() { const auto& s = WaterConfig(); return s.remove_all || s.splashes_removed; }
bool WaterSaveConfig() {
    if (g_settings_path.empty()) return false;
    const auto& setting = WaterConfig();
    uint32_t failed = 0;
    const auto write = [&](uint32_t field, const wchar_t* key,
                           const std::wstring& value) {
        if ((s_settings_save_dirty & field) &&
            !QueueSettingsValue(L"WaterColours", key,
                                       value.c_str()))
            failed |= field;
    };
    write(SurfaceHex, L"Hex", WaterHex(setting.rgb));
    write(SurfaceEnabled, L"CustomEnabled", setting.custom ? L"1" : L"0");
    write(SurfaceIntensity, L"SurfaceIntensityPercent",
          std::to_wstring(setting.surface_intensity_percent));
    write(UnderwaterHex, L"UnderwaterHex", WaterHex(setting.underwater_rgb));
    write(UnderwaterEnabled, L"UnderwaterCustomEnabled",
          setting.underwater_custom ? L"1" : L"0");
    write(UnderwaterIntensity, L"UnderwaterIntensityPercent",
          std::to_wstring(setting.underwater_intensity_percent));
    write(RemoveTint, L"RemoveUnderwaterTint", setting.tint_removed ? L"1" : L"0");
    write(RemoveSplashes, L"RemoveWaterSplashes", setting.splashes_removed ? L"1" : L"0");
    write(ReflectionsVisible, L"ReflectionsVisible", setting.reflections_visible ? L"1" : L"0");
    write(ReflectionGeometryVisible, L"ReflectionGeometryVisible",
          setting.reflection_geometry_visible ? L"1" : L"0");
    write(ReflectionButtonsVisible, L"ReflectionButtonsVisible",
          setting.reflection_buttons_visible ? L"1" : L"0");
    write(ReflectionPipesVisible, L"ReflectionPipesVisible",
          setting.reflection_pipes_visible ? L"1" : L"0");
    write(ReflectionBeaconsVisible, L"ReflectionBeaconsVisible",
          setting.reflection_beacons_visible ? L"1" : L"0");
    write(ReflectionJumpPadsVisible, L"ReflectionJumpPadsVisible",
          setting.reflection_jump_pads_visible ? L"1" : L"0");
    write(ReflectionCubesVisible, L"ReflectionCubesVisible",
          setting.reflection_cubes_visible ? L"1" : L"0");
    const auto write_gameplay = [&](uint32_t field, const wchar_t* key,
                                    const std::wstring& value) {
        if ((s_settings_save_dirty & field) &&
            !QueueSettingsValue(L"WaterGameplay", key,
                                        value.c_str()))
            failed |= field;
    };
    write_gameplay(RemoveAll, L"RemoveAll", setting.remove_all ? L"1" : L"0");
    write_gameplay(SurfaceRemoved, L"RemoveSurface", setting.removed ? L"1" : L"0");
    write_gameplay(SwimmingDisabled, L"DisableSwimming",
                   setting.swimming_disabled ? L"1" : L"0");
    write_gameplay(SurfaceHeight, L"HeightOffset", std::to_wstring(setting.height_offset));
    s_settings_save_dirty = failed;
    if (failed) Log(L"Water: could not save WaterColours or WaterGameplay settings");
    return failed == 0;
}
// Queue only the keys that changed; the render update never waits for disk IO.
void QueueWaterConfigSave(uint32_t changed) {
    s_settings_save_dirty |= changed;
    s_settings_save_due = GetTickCount64() + kSettingsSaveDebounceMs;
}
void FlushWaterConfigIfDue() {
    if (!s_settings_save_dirty || GetTickCount64() < s_settings_save_due) return;
    if (!WaterSaveConfig()) s_settings_save_due = GetTickCount64() + 3000;
}
void WaterSettingsChanged(const std::wstring& message, uint16_t changed_fields) {
    // Cosmetic colour changes are tracked separately from World gameplay changes.
    // A Game Default request must restore existing MIDs, not start another
    // expensive discovery pass. Discovery resumes only for Custom colours.
    // Reuse the verified ocean mesh and MID across palette changes. A full
    // GUObjectArray rescan can take seconds, and is not needed for a new RGB.
    const bool surface_active = WaterConfig().custom &&
        WaterConfig().surface_intensity_percent > 0;
    if (!surface_active) {
        s_scan_cursor = -1;
    } else if (s_targets.empty() && s_scan_cursor < 0) {
        s_scan_cursor = 0;
        s_next_scan = 0;
        s_reported = false;
        s_examined_meshes = 0;
        s_ocean_meshes = 0;
    }
    s_next_update = 0; // Existing MIDs update on the next game tick.
    if (surface_active && !s_targets.empty()) {
        // Deferring only background discovery leaves an already-cached MID
        // free to update immediately. Pause/resume any in-flight scan too.
        s_surface_edit_quiet_until = GetTickCount64() + kSurfaceColourEditQuietMs;
    } else if (!surface_active) {
        // The next Custom request creates a new material and may log once.
        s_applied_logged = false;
    }
    // Do not re-enable a synchronous file Log() on every RGB adjustment.
    QueueWaterConfigSave(changed_fields);
    Notify(message + L" (visual only)");
}
void UnderwaterSettingsChanged(const std::wstring& message, uint16_t changed_fields) {
    s_underwater_next_apply = 0;
    s_underwater_next_scan = 0;
    if (!s_underwater_target.actor) s_underwater_scan_cursor = 0;
    s_underwater_logged = false;
    QueueWaterConfigSave(changed_fields);
    Notify(message + L" (visual only)");
}
DWORD WINAPI WaterPickerThread(LPVOID) {
    COLORREF palette[16]{};
    CHOOSECOLORW choice{};
    choice.lStructSize = sizeof(choice);
    choice.lpCustColors = palette;
    const uint32_t rgb = s_picker_initial.load(std::memory_order_relaxed);
    choice.rgbResult = RGB((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
    choice.Flags = CC_FULLOPEN | CC_ANYCOLOR | CC_RGBINIT;
    if (ChooseColorW(&choice)) {
        const uint32_t selected = (static_cast<uint32_t>(GetRValue(choice.rgbResult)) << 16) |
            (static_cast<uint32_t>(GetGValue(choice.rgbResult)) << 8) |
            static_cast<uint32_t>(GetBValue(choice.rgbResult));
        s_picker_result.store(selected, std::memory_order_release);
    } else {
        s_picker_result.store(kPickerCancelled, std::memory_order_release);
    }
    s_picker_open.store(false, std::memory_order_release);
    return 0;
}
std::wstring WaterObjectName(void* object) {
    return IsReadable(object, sizeof(UObjectHeader))
        ? UnrealName(static_cast<const UObjectHeader*>(object)->name) : L"";
}
bool WaterIsA(void* object, const wchar_t* class_name) {
    if (!IsReadable(object, sizeof(UObjectHeader))) return false;
    void* cls = static_cast<const UObjectHeader*>(object)->class_ptr;
    for (int i = 0; i < 12 && IsReadable(cls, sizeof(UStructHeader)); ++i) {
        if (WaterObjectName(cls) == class_name) return true;
        cls = static_cast<const UStructHeader*>(cls)->super_struct;
    }
    return false;
}
const GlobalObjectArray* WaterObjects() {
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
// Refunct's ocean can be a Blueprint-created mesh, a mesh owned by
// BP_WaterSurface, or a mesh spawned by WorldBase. The old discovery required
// a very narrow component name and owner, which can miss the rendered water.
// Instead inspect live StaticMeshComponents, but only colour components whose
// *currently assigned material* is one of the packaged ocean materials.
// Reject class defaults and archetypes to avoid mutating editor/template data.
bool WaterIsLevelWaterComponent(void* component) {
    if (!IsReadable(component, sizeof(UObjectHeader))) return false;
    const auto* header = static_cast<const UObjectHeader*>(component);
    if ((static_cast<uint32_t>(header->flags) & 0x30u) != 0) return false;
    // UE4 classes outlive their components across level restarts. Cache only
    // the native class hierarchy result, never a component instance pointer.
    static std::unordered_map<void*, bool> mesh_class_cache;
    bool is_mesh = false;
    const auto it = mesh_class_cache.find(header->class_ptr);
    if (it != mesh_class_cache.end()) is_mesh = it->second;
    else {
        is_mesh = WaterIsA(component, L"StaticMeshComponent");
        if (mesh_class_cache.size() < 4096)
            mesh_class_cache.emplace(header->class_ptr, is_mesh);
    }
    if (!is_mesh) return false;
    // A component owned by a live AActor (including LevelScriptActor and
    // BP_WaterSurface_C) is an instance, not a Blueprint class template.
    void* owner = header->outer;
    for (int depth = 0; depth < 6 && IsReadable(owner, sizeof(UObjectHeader)); ++depth) {
        const auto* parent = static_cast<const UObjectHeader*>(owner);
        if ((static_cast<uint32_t>(parent->flags) & 0x30u) != 0) return false;
        if (WaterIsA(owner, L"Actor")) return true;
        owner = parent->outer;
    }
    return false;
}
bool WaterIsTargetValid(const WaterTarget& target, const GlobalObjectArray* objects) {
    return objects && target.index >= 0 && target.index < objects->num_elements &&
        objects->objects[target.index].object == target.actor &&
        objects->objects[target.index].serial_number == target.serial &&
        WaterIsLevelWaterComponent(target.actor);
}
int32_t WaterParam(void* function, const wchar_t* name, const wchar_t* type) {
    if (!IsReadable(function, sizeof(UStructHeader))) return -1;
    void* field = static_cast<const UStructHeader*>(function)->children;
    for (int i = 0; i < 48 && IsReadable(field, sizeof(UFieldHeader)); ++i) {
        const auto* property = static_cast<const UFieldHeader*>(field);
        if (UnrealName(property->object.name) == name &&
            WaterObjectName(property->object.class_ptr) == type) {
            const int32_t offset = FindClassPropertyOffset(function, name);
            const int bytes = wcscmp(type, L"NameProperty") == 0 ? 8 :
                wcscmp(type, L"StructProperty") == 0 ? 16 :
                wcscmp(type, L"StrProperty") == 0 ? 12 : 4;
            return offset >= 0 && offset + bytes <= 96 ? offset : -1;
        }
        field = property->next;
    }
    return -1;
}
// Unreal's UFunction metadata is shared by all instances of a class and is
// stable for this pinned executable. Resolving it uses costly FName-to-string
// allocations; cache verified argument offsets per concrete UObject class.
// Thread-local avoids a data race if a menu callback runs on another thread.
struct WaterCallCache {
    void* owner_class = nullptr;
    void* function = nullptr;
    int32_t first = -1;
    int32_t second = -1;
};
bool WaterCallMatches(const WaterCallCache& cached, void* object) {
    return IsReadable(object, sizeof(UObjectHeader)) &&
        static_cast<const UObjectHeader*>(object)->class_ptr == cached.owner_class &&
        cached.first >= 0 && cached.second >= 0 &&
        IsReadable(cached.function, sizeof(UFieldHeader));
}
void* WaterGetMaterial(void* mesh) {
    if (!mesh || !g_process_event) return nullptr;
    thread_local WaterCallCache cached{};
    if (!WaterCallMatches(cached, mesh)) {
        cached = {};
        void* fn = FindReflectedFunction(mesh, L"GetMaterial");
        const int32_t index = WaterParam(fn, L"ElementIndex", L"IntProperty");
        const int32_t result = WaterParam(fn, L"ReturnValue", L"ObjectProperty");
        if (index < 0 || result < 0) return nullptr;
        cached = {static_cast<const UObjectHeader*>(mesh)->class_ptr, fn, index, result};
    }
    alignas(16) std::array<uint8_t, 96> args{}; // ElementIndex = 0
    g_process_event(mesh, cached.function, args.data());
    void* material = nullptr;
    std::memcpy(&material, args.data() + cached.second, sizeof(material));
    return IsReadable(material, sizeof(UObjectHeader)) ? material : nullptr;
}
bool WaterSetMaterial(void* mesh, void* material) {
    if (!mesh || !IsReadable(material, sizeof(UObjectHeader)) || !g_process_event)
        return false;
    void* fn = FindReflectedFunction(mesh, L"SetMaterial");
    const int32_t index = WaterParam(fn, L"ElementIndex", L"IntProperty");
    const int32_t value = WaterParam(fn, L"Material", L"ObjectProperty");
    if (index < 0 || value < 0) return false;
    alignas(16) std::array<uint8_t, 96> args{};
    std::memcpy(args.data() + value, &material, sizeof(material));
    g_process_event(mesh, fn, args.data());
    return WaterGetMaterial(mesh) == material;
}
// The packaged material instance owns the FName "base_color". The alternate
// M_Infinite_Ocean_Linux instance inherits it, so follow UE's reflected Parent
// property rather than requiring the *displayed* material to have a local
// vector override. All material names below were checked against the PAK.
bool WaterIsOceanMaterial(void* material) {
    for (int depth = 0; depth < 6 && IsReadable(material, sizeof(UObjectHeader)); ++depth) {
        const std::wstring name = WaterObjectName(material);
        if (name == L"M_Infinite_Ocean_Inst" ||
            name == L"M_Infinite_Ocean_Linux" ||
            name == L"M_Infinite_Ocean") return true;
        if (!WaterIsA(material, L"MaterialInstance")) return false;
        void* parent = ReadReflectedObject(material, L"Parent");
        if (!parent || parent == material) return false;
        material = parent;
    }
    return false;
}
// In the game's legacy UE4 material format, FVectorParameterValue starts
// with an FName, followed by an FLinearColor and an FGuid (40-byte entries).
// The ocean instance has a base_color override, but other loaded MIDs may
// contain more than one vector override. Do not assume array count == 1.
bool WaterReadColourName(void* instance, FNameView& result) {
    if (!WaterIsA(instance, L"MaterialInstance")) return false;
    void* cls = static_cast<const UObjectHeader*>(instance)->class_ptr;
    const int32_t offset = FindClassPropertyOffset(cls, L"VectorParameterValues");
    if (offset < 0 || offset > 0x800 ||
        !IsReadable(static_cast<const uint8_t*>(instance) + offset,
                    sizeof(UObjectPointerArray))) return false;
    UObjectPointerArray values{};
    std::memcpy(&values, static_cast<const uint8_t*>(instance) + offset, sizeof(values));
    if (values.count < 1 || values.count > 32 ||
        values.capacity < values.count || values.capacity > 512 || !values.data)
        return false;

    // Refunct's cooked asset names use ParameterName (the legacy UE4 layout),
    // not the newer FMaterialParameterInfo layout. Do not probe guessed
    // strides against live memory: a bad FName index can crash the game.
    constexpr size_t kEntryStrides[] = {40};
    for (const size_t stride : kEntryStrides) {
        for (int32_t i = 0; i < values.count; ++i) {
            const auto* address = reinterpret_cast<const uint8_t*>(values.data) +
                                  static_cast<size_t>(i) * stride;
            if (!IsReadable(address, sizeof(FNameView))) break;
            FNameView candidate{};
            std::memcpy(&candidate, address, sizeof(candidate));
            // Reject arbitrary vector/float bytes before passing the value
            // to UE4's FName resolver (which assumes a valid name index).
            if (candidate.index == 0 || candidate.index >= 65536 ||
                candidate.number > 255) continue;
            if (UnrealName(candidate) == L"base_color") {
                result = candidate;
                return true;
            }
        }
    }
    return false;
}

// A loaded material instance (including one not currently assigned to the
// water component) or a cooked material-expression UObject can provide the
// real runtime FName. Acquire the name, not the material, and never edit these
// evidence objects. This runs in the existing bounded discovery pass.
void WaterObserveLoadedParameter(void* object) {
    if (s_ocean_parameter_ready || !IsReadable(object, sizeof(UObjectHeader))) return;
    const auto* header = static_cast<const UObjectHeader*>(object);
    static std::unordered_map<void*, uint8_t> classes;
    uint8_t kind = 0;
    if (const auto it = classes.find(header->class_ptr); it != classes.end()) {
        kind = it->second;
    } else {
        const std::wstring klass = WaterObjectName(header->class_ptr);
        kind = (klass == L"MaterialInstanceConstant" ||
                klass == L"MaterialInstanceDynamic") ? 1 :
               klass == L"MaterialExpressionVectorParameter" ? 2 : 0;
        if (classes.size() < 4096) classes.emplace(header->class_ptr, kind);
    }
    FNameView parameter{};
    if (kind == 1) {
        if (!WaterReadColourName(object, parameter)) return;
    } else if (kind == 2) {
        const int32_t offset = FindClassPropertyOffset(header->class_ptr,
                                                       L"ParameterName");
        if (offset < 0 || offset > 0x800 ||
            !IsReadable(static_cast<const uint8_t*>(object) + offset,
                        sizeof(parameter))) return;
        std::memcpy(&parameter, static_cast<const uint8_t*>(object) + offset,
                    sizeof(parameter));
        if (parameter.index == 0 || parameter.index >= 65536 ||
            parameter.number > 255 || UnrealName(parameter) != L"base_color") return;
    } else return;
    s_ocean_colour_parameter = parameter;
    s_ocean_parameter_ready = true;
    Log(kind == 1
        ? L"Water colours: resolved base_color from a loaded material instance"
        : L"Water colours: resolved base_color from a loaded vector expression");
}

// The packaged .pak contains the identifier base_color, but its package name
// index is not the engine's runtime FName index. Obtain a real FName from a
// loaded ocean MIC or from the game's KismetStringLibrary.
void WaterObserveNameLibrary(void* object) {
    if (s_ocean_parameter_ready || !g_process_event ||
        !IsReadable(object, sizeof(UObjectHeader))) return;
    const auto* header = static_cast<const UObjectHeader*>(object);
    if (UnrealName(header->name) != L"Default__KismetStringLibrary" ||
        WaterObjectName(header->class_ptr) != L"KismetStringLibrary") return;
    void* fn = FindReflectedFunction(object, L"Conv_StringToName");
    const int32_t input = WaterParam(fn, L"InString", L"StrProperty");
    const int32_t output = WaterParam(fn, L"ReturnValue", L"NameProperty");
    if (input < 0 || output < 0) return;
    wchar_t name[] = L"base_color";
    const FStringView text{name, static_cast<int32_t>(std::size(name)),
                           static_cast<int32_t>(std::size(name))};
    alignas(16) std::array<uint8_t, 96> args{};
    std::memcpy(args.data() + input, &text, sizeof(text));
    g_process_event(object, fn, args.data());
    FNameView actual{};
    std::memcpy(&actual, args.data() + output, sizeof(actual));
    if (UnrealName(actual) != L"base_color") return;
    s_ocean_colour_parameter = actual;
    s_ocean_parameter_ready = true;
    Log(L"Water colours: resolved base_color via KismetStringLibrary");
}
bool WaterBaseColourParameter(void* material, FNameView& result) {
    if (!WaterIsOceanMaterial(material)) return false;
    if (s_ocean_parameter_ready) {
        result = s_ocean_colour_parameter;
        return true;
    }
    for (int depth = 0; depth < 6 && IsReadable(material, sizeof(UObjectHeader)); ++depth) {
        if (WaterObjectName(material) == L"M_Infinite_Ocean_Inst" &&
            WaterReadColourName(material, result)) {
            s_ocean_colour_parameter = result;
            s_ocean_parameter_ready = true;
            return true;
        }
        if (!WaterIsA(material, L"MaterialInstance")) break;
        void* parent = ReadReflectedObject(material, L"Parent");
        if (!parent || parent == material) break;
        material = parent;
    }
    return false;
}
// Read the actual base_color FLinearColor on the original ocean MIC.
// The pinned legacy game build uses 40-byte FVectorParameterValue entries:
// FName (8), FLinearColor (16), FGuid (16). Other fields are never edited.
// A child MIC may inherit the parameter, so walk the material parent chain.
bool WaterReadNativeBaseColour(void* source, FNameView parameter,
                               LinearColor& colour) {
    if (!WaterIsOceanMaterial(source)) return false;
    for (int depth = 0; depth < 6 && IsReadable(source, sizeof(UObjectHeader)); ++depth) {
        if (WaterIsA(source, L"MaterialInstance")) {
            void* cls = static_cast<const UObjectHeader*>(source)->class_ptr;
            const int32_t offset = FindClassPropertyOffset(
                cls, L"VectorParameterValues");
            if (offset >= 0 && offset <= 0x800 &&
                IsReadable(static_cast<const uint8_t*>(source) + offset,
                           sizeof(UObjectPointerArray))) {
                UObjectPointerArray values{};
                std::memcpy(&values, static_cast<const uint8_t*>(source) + offset,
                            sizeof(values));
                if (values.count >= 0 && values.count <= 32 &&
                    values.capacity >= values.count && values.capacity <= 512 &&
                    values.data) {
                    for (int32_t i = 0; i < values.count; ++i) {
                        const auto* entry = reinterpret_cast<const uint8_t*>(values.data) +
                                            static_cast<size_t>(i) * 40;
                        if (!IsReadable(entry, sizeof(FNameView) + sizeof(LinearColor)))
                            break;
                        FNameView name{};
                        std::memcpy(&name, entry, sizeof(name));
                        if (name.index != parameter.index ||
                            name.number != parameter.number) continue;
                        LinearColor candidate{};
                        std::memcpy(&candidate, entry + sizeof(FNameView),
                                    sizeof(candidate));
                        if (!std::isfinite(candidate.r) ||
                            !std::isfinite(candidate.g) ||
                            !std::isfinite(candidate.b) ||
                            candidate.r < 0.0f || candidate.g < 0.0f ||
                            candidate.b < 0.0f || candidate.r > 256.0f ||
                            candidate.g > 256.0f || candidate.b > 256.0f) return false;
                        colour = candidate;
                        return true;
                    }
                }
            }
        }
        void* parent = ReadReflectedObject(source, L"Parent");
        if (!parent || parent == source) break;
        source = parent;
    }
    return false;
}
// Match the existing surface colour conversion at 100%. For percentages below
// 100, interpolate from the original ocean parameter in linear colour space.
// Above 100, apply the same unbounded HDR extrapolation as underwater tint.
LinearColor SurfaceBlendTint(uint32_t rgb, const LinearColor& original,
                             int percent) {
    const auto linear = [](uint32_t channel) {
        const float x = static_cast<float>(channel) / 255.0f;
        return x <= 0.04045f ? x / 12.92f :
            std::pow((x + 0.055f) / 1.055f, 2.4f);
    };
    const LinearColor selected{linear((rgb >> 16) & 255),
                               linear((rgb >> 8) & 255),
                               linear(rgb & 255), 1.0f};
    const double strength = static_cast<double>((std::max)(0, percent)) / 100.0;
    const auto mix = [strength](float base, float target) -> float {
        const double blended = static_cast<double>(base) +
            (static_cast<double>(target) - base) * strength;
        if (strength <= 1.0) return static_cast<float>(blended);
        // The HDR path follows underwater intensity's boosted extrapolation.
        // Double intermediates keep INT_MAX% finite without a preset cap.
        return static_cast<float>((std::max)(0.0, blended) * strength);
    };
    return {mix(original.r, selected.r), mix(original.g, selected.g),
            mix(original.b, selected.b), 1.0f};
}
void* WaterCreateMID(void* mesh, void* original) {
    if (!mesh || !original || !g_process_event) return nullptr;
    struct Method { const wchar_t* name; const wchar_t* parent; bool named; };
    constexpr Method methods[] = {
        {L"CreateDynamicMaterialInstance", L"SourceMaterial", true},
        {L"CreateAndSetMaterialInstanceDynamicFromMaterial", L"Parent", false},
        {L"CreateAndSetMaterialInstanceDynamic", nullptr, false}
    };
    for (const Method& method : methods) {
        void* fn = FindReflectedFunction(mesh, method.name);
        const int32_t index = WaterParam(fn, L"ElementIndex", L"IntProperty");
        const int32_t returned = WaterParam(fn, L"ReturnValue", L"ObjectProperty");
        const int32_t parent = method.parent ? WaterParam(fn, method.parent, L"ObjectProperty") : -1;
        const int32_t name = method.named ? WaterParam(fn, L"OptionalName", L"NameProperty") : -1;
        if (index < 0 || returned < 0 || (method.parent && parent < 0) ||
            (method.named && name < 0)) continue;
        if (!method.parent && WaterGetMaterial(mesh) != original && !WaterSetMaterial(mesh, original))
            break;
        alignas(16) std::array<uint8_t, 96> args{};
        if (parent >= 0) std::memcpy(args.data() + parent, &original, sizeof(original));
        g_process_event(mesh, fn, args.data());
        void* mid = nullptr;
        std::memcpy(&mid, args.data() + returned, sizeof(mid));
        if (IsReadable(mid, sizeof(UObjectHeader)) && WaterGetMaterial(mesh) == mid &&
            WaterObjectName(static_cast<const UObjectHeader*>(mid)->class_ptr)
                .find(L"MaterialInstanceDynamic") != std::wstring::npos)
            return mid;
    }
    if (WaterGetMaterial(mesh) != original) WaterSetMaterial(mesh, original);
    return nullptr;
}
bool WaterTint(void* mid, FNameView parameter, const LinearColor& colour) {
    if (!g_process_event) return false;
    thread_local WaterCallCache cached{};
    if (!WaterCallMatches(cached, mid)) {
        cached = {};
        void* fn = FindReflectedFunction(mid, L"SetVectorParameterValue");
        if (!fn) fn = FindReflectedFunction(mid, L"K2_SetVectorParameterValue");
        const int32_t name = WaterParam(fn, L"ParameterName", L"NameProperty");
        const int32_t value = WaterParam(fn, L"Value", L"StructProperty");
        if (name < 0 || value < 0) return false;
        cached = {static_cast<const UObjectHeader*>(mid)->class_ptr, fn, name, value};
    }
    alignas(16) std::array<uint8_t, 96> args{};
    std::memcpy(args.data() + cached.first, &parameter, sizeof(parameter));
    std::memcpy(args.data() + cached.second, &colour, sizeof(colour));
    g_process_event(mid, cached.function, args.data());
    return true;
}
// A dedicated MID only contains our surface-colour override. If restoring its
// original material fails, clearing that override safely makes it inherit the
// ocean's native base_color while we keep retrying SetMaterial.
bool WaterClearOwnMID(void* mid) {
    if (!g_process_event || !WaterIsA(mid, L"MaterialInstanceDynamic")) return false;
    void* fn = FindReflectedFunction(mid, L"ClearParameterValues");
    if (!fn) return false; // Not exposed by every cooked UE4 build.
    alignas(16) std::array<uint8_t, 16> args{};
    g_process_event(mid, fn, args.data());
    return true;
}
void WaterApplyColour(WaterTarget& target, bool enabled, uint32_t rgb,
                      int intensity_percent = 100) {
    const ULONGLONG now = GetTickCount64();
    if (enabled && target.dynamic_material && target.original_material &&
        target.last_rgb == rgb &&
        target.last_intensity_percent == intensity_percent &&
        !target.cleared_on_restore && now < target.next_verify) return;
    // Both call sites already verify the GUObjectArray index, serial and live
    // component type through WaterIsTargetValid(). Do not repeat the costly
    // actor-owner/class reflection on each RGB change.
    target.mesh = target.actor;
    void* displayed = WaterGetMaterial(target.mesh);
    if (!displayed) return;

    const bool our_mid_displayed = target.dynamic_material &&
                                   displayed == target.dynamic_material;
    if (!enabled) {
        if (!target.dynamic_material) return;
        if (!our_mid_displayed) {
            // The game replaced our MID. Never overwrite the newer material.
            // Our override is no longer displayed, so release this entry.
            target.dynamic_material = nullptr;
            target.original_material = nullptr;
            target.cleared_on_restore = false;
            target.restore_warning_logged = false;
            target.unsupported = false;
            return;
        }
        if (now < target.next_verify) return;

        void* original = target.original_material;
        if (!IsReadable(original, sizeof(UObjectHeader)) ||
            original == displayed || !WaterIsOceanMaterial(original)) {
            // Recover from a lost/stale cached pointer using the known parent
            // of *our own* MID, never using an arbitrary world's material.
            void* parent = ReadReflectedObject(displayed, L"Parent");
            if (parent != displayed && WaterIsOceanMaterial(parent)) {
                original = parent;
                target.original_material = original;
            } else {
                original = nullptr;
            }
        }
        if (original && WaterSetMaterial(target.mesh, original)) {
            target.dynamic_material = nullptr;
            target.original_material = nullptr;
            target.cleared_on_restore = false;
            target.restore_warning_logged = false;
            target.unsupported = false;
            Log(L"Water colours: native surface material restored (Game Default)");
            return;
        }
        // Do not discard target.original_material or target.dynamic_material:
        // if we did, the tinted MID could remain permanently on the mesh.
        if (!target.cleared_on_restore && WaterClearOwnMID(displayed)) {
            target.cleared_on_restore = true;
            Log(L"Water colours: reset surface MID overrides; retrying native material");
        }
        target.next_verify = now + 1000;
        if (!target.restore_warning_logged) {
            target.restore_warning_logged = true;
            Log(L"Water colours: native surface restore not yet confirmed; retaining original for retry");
        }
        return;
    }

    const bool ours = our_mid_displayed && target.original_material &&
                      IsReadable(target.original_material, sizeof(UObjectHeader));
    if (ours && target.last_rgb == rgb &&
        target.last_intensity_percent == intensity_percent &&
        !target.cleared_on_restore) {
        target.next_verify = now + 750;
        return;
    }
    // A startup material swap may happen after our first attempt. Retry
    // occasionally instead of disabling this water surface forever.
    if (target.unsupported && !ours && now < target.next_verify) return;
    target.unsupported = false;
    void* source = ours ? target.original_material : displayed;
    FNameView parameter{};
    if (ours) parameter = target.colour_parameter;
    else if (!WaterBaseColourParameter(source, parameter)) {
        target.unsupported = true;
        target.next_verify = now + 2000;
        if (!s_warning_logged) {
            s_warning_logged = true;
            Log((L"Water colours: ocean base_color not found (displayed: " +
                 WaterObjectName(source) + L")").c_str());
        }
        return;
    }
    // Reuse the original colour cached on the tracked MID. Only read the
    // native parameter array when creating/adopting a MID, not every edit.
    LinearColor native_colour = target.native_colour;
    bool native_known = ours && target.native_colour_known;
    if (!ours) native_known = WaterReadNativeBaseColour(source, parameter, native_colour);
    // If an older cooked material cannot expose its native vector, the
    // 0% setting still restores the original material exactly. Neutral white
    // is the safe interpolation fallback for partial strength.
    if (!native_known) native_colour = {1.0f, 1.0f, 1.0f, 1.0f};
    const LinearColor tint = SurfaceBlendTint(rgb, native_colour,
                                              intensity_percent);
    void* mid = ours ? target.dynamic_material : WaterCreateMID(target.mesh, source);
    if (!mid || !WaterTint(mid, parameter, tint)) {
        if (mid && !ours) WaterSetMaterial(target.mesh, source);
        target.unsupported = true;
        target.next_verify = now + 2000;
        if (!s_warning_logged) {
            s_warning_logged = true;
            Log(L"Water colours: dynamic material creation or tint failed");
        }
        return;
    }
    target.original_material = source;
    target.dynamic_material = mid;
    target.colour_parameter = parameter;
    target.last_rgb = rgb;
    target.last_intensity_percent = intensity_percent;
    target.native_colour = native_colour;
    target.native_colour_known = native_known;
    target.next_verify = now + 750;
    target.unsupported = false;
    target.cleared_on_restore = false;
    target.restore_warning_logged = false;
    if (!s_applied_logged) {
        s_applied_logged = true;
        Log((L"Water colours: applied base_color to " + WaterObjectName(target.mesh) +
             L" (source material: " + WaterObjectName(source) + L")").c_str());
    }
}
// Reflected calls on verified, live ocean mesh components. Never write
// visibility or transform fields by guessed offsets.
bool WaterGetVisible(void* mesh, bool& visible) {
    if (!g_process_event) return false;
    void* fn = FindReflectedFunction(mesh, L"IsVisible");
    const int32_t result = WaterParam(fn, L"ReturnValue", L"BoolProperty");
    if (result < 0) return false;
    alignas(16) std::array<uint8_t, 128> args{};
    g_process_event(mesh, fn, args.data());
    visible = args[result] != 0;
    return true;
}
bool WaterSetVisible(void* mesh, bool visible) {
    if (!g_process_event) return false;
    void* fn = FindReflectedFunction(mesh, L"SetVisibility");
    const int32_t value = WaterParam(fn, L"bNewVisibility", L"BoolProperty");
    const int32_t propagate = WaterParam(fn, L"bPropagateToChildren", L"BoolProperty");
    if (value < 0 || propagate < 0) return false;
    alignas(16) std::array<uint8_t, 128> args{};
    args[value] = visible ? 1 : 0;
    // Only the identified ocean component, never unrelated child meshes.
    args[propagate] = 0;
    g_process_event(mesh, fn, args.data());
    bool actual = false;
    return WaterGetVisible(mesh, actual) && actual == visible;
}
// SetVisibility is insufficient for some UE4 planar capture render states:
// after being hidden, bHiddenInGame or an inactive component can still keep
// its scene proxy from submitting captures. On explicit restoration, request a
// render-state refresh via the reflected SceneComponent API, without changing
// native offsets, materials, or global rendering flags.
bool WaterReadComponentActive(void* component, bool& active) {
    if (!g_process_event) return false;
    void* fn = FindReflectedFunction(component, L"IsActive");
    const int32_t result = WaterParam(fn, L"ReturnValue", L"BoolProperty");
    if (result < 0) return false;
    alignas(16) std::array<uint8_t,128> args{};
    g_process_event(component, fn, args.data());
    active = args[result] != 0;
    return true;
}
void WaterWakePlanarReflection(void* component) {
    if (!g_process_event) return;
    // Explicitly re-enable the capture after an OFF -> ON transition. The
    // initial IsActive state is not reliable if discovery happens after a
    // previous OFF action; it must not prevent recovery.
    void* show = FindReflectedFunction(component, L"SetHiddenInGame");
    const int32_t hidden = WaterParam(show, L"bNewHidden", L"BoolProperty");
    const int32_t children = WaterParam(show, L"bPropagateToChildren", L"BoolProperty");
    if (hidden >= 0 && children >= 0) {
        alignas(16) std::array<uint8_t,128> args{};
        args[hidden] = 0;
        args[children] = 0;
        g_process_event(component, show, args.data());
        Log(L"Water reflections: restore SetHiddenInGame(false) dispatched");
    } else {
        Log(L"Water reflections: SetHiddenInGame not reflected; visibility-only restore");
    }
    bool active = false;
    if (!WaterReadComponentActive(component, active) || active) return;
    void* fn = FindReflectedFunction(component, L"Activate");
    const int32_t reset = WaterParam(fn, L"bReset", L"BoolProperty");
    if (reset < 0) return;
    alignas(16) std::array<uint8_t,128> args{};
    args[reset] = 1;
    g_process_event(component, fn, args.data());
    bool now_active = false;
    if (WaterReadComponentActive(component, now_active) && now_active)
        Log(L"Water reflections: inactive planar capture reactivated");
}
struct WaterCaptureArrayHeader {
    const uint8_t* data = nullptr;
    int32_t count = 0;
    int32_t capacity = 0;
};


// Capture-only wall/lift reflection removal. The shipped BP_IslandChunk_C
// BaseMesh is the concrete wall/tile panel, and BP_Lift/BP_Mover LiftMesh
// covers moving platforms. Never include the ocean material, the actor's
// collision, player, lights or the scene capture itself.
//
// HideComponent uses UE's own allocator to append TWeakObjectPtr entries.
// Reversal is allowed only when the reflected HiddenComponents array was empty
// before our writes and still contains exactly the weak refs we inserted.
// We modify only the array's Count (on the game thread) to unhide them; the
// engine retains ownership of Data/Capacity and frees it normally.
// Discover the rendered components, rather than relying exclusively on a
// single Blueprint object property. Components may be created by Blueprint
// construction scripts and need not be exposed as Mover/Mesh/Beacon.
// Walk only UObject ownership, stopping at the first actor. Do not follow
// arbitrary references into a different actor's meshes.
bool WaterReflectionLiveObject(const GlobalObjectArray* objects, void* ptr) {
    if (!objects || !IsReadable(ptr, sizeof(UObjectHeader))) return false;
    const auto* hdr = static_cast<const UObjectHeader*>(ptr);
    return hdr->index >= 0 && hdr->index < objects->num_elements &&
        objects->objects[hdr->index].object == ptr &&
        (static_cast<uint32_t>(hdr->flags) & 0x30u) == 0 &&
        WaterObjectName(ptr).rfind(L"Default__", 0) != 0;
}

bool WaterReflectionBeamName(void* object) {
    auto name = WaterObjectName(object);
    for (auto& ch : name) ch = static_cast<wchar_t>(towlower(ch));
    return name.find(L"beacon") != std::wstring::npos ||
           name.find(L"beam") != std::wstring::npos;
}

bool WaterReflectionVisualComponent(void* component) {
    // Shape/collision components are not rendered in the shipping game.
    return WaterIsA(component, L"MeshComponent") ||
           WaterIsA(component, L"StaticMeshComponent") ||
           WaterIsA(component, L"ParticleSystemComponent");
}

void* WaterReflectionVisualOwner(const GlobalObjectArray* objects, void* component) {
    if (!WaterReflectionLiveObject(objects, component)) return nullptr;
    void* outer = static_cast<const UObjectHeader*>(component)->outer;
    for (int depth = 0; depth < 8 && WaterReflectionLiveObject(objects, outer); ++depth) {
        const auto* hdr = static_cast<const UObjectHeader*>(outer);
        const auto type = WaterObjectName(hdr->class_ptr);
        if (type == L"BP_Jumppad_C" || type == L"BP_PowerCore_C" ||
            type == L"BP_Button_C") return outer;
        if (WaterIsA(outer, L"Actor")) break;
        // Only component subobjects can sit between a visual and its owner.
        if (!WaterIsA(outer, L"SceneComponent")) break;
        outer = hdr->outer;
    }
    return nullptr;
}

bool WaterReflectionIsBeaconVisual(const GlobalObjectArray* objects,
                                   void* component, void* actor) {
    void* beacon = ReadReflectedObject(actor, L"Beacon");
    if (component == beacon || WaterReflectionBeamName(component)) return true;
    // A Beacon can be a SceneComponent root rather than the actual primitive.
    // Follow reflected attachment parents only within the same button actor.
    void* parent = component;
    for (int depth = 0; depth < 8; ++depth) {
        parent = ReadReflectedObject(parent, L"AttachParent");
        if (!parent || WaterReflectionVisualOwner(objects, parent) != actor) break;
        if (parent == beacon || WaterReflectionBeamName(parent)) return true;
    }
    // Particle components sometimes have generic instance names; the asset
    // can still identify the beacon without excluding unrelated button FX.
    if (WaterIsA(component, L"ParticleSystemComponent")) {
        void* asset = ReadReflectedObject(component, L"Template");
        if (asset && WaterReflectionBeamName(asset)) return true;
    }
    return false;
}

bool WaterGeometryCandidateValid(const ReflectionGeometryCandidate& candidate,
                                 const GlobalObjectArray* objects) {
    return objects && candidate.component && candidate.index >= 0 &&
        candidate.index < objects->num_elements &&
        objects->objects[candidate.index].object == candidate.component &&
        (candidate.serial == 0 ||
         objects->objects[candidate.index].serial_number == candidate.serial) &&
        IsReadable(candidate.component, sizeof(UObjectHeader)) &&
        (static_cast<uint32_t>(static_cast<UObjectHeader*>(candidate.component)->flags) & 0x30u) == 0;
}

bool WaterReadCaptureHiddenList(void* component, int32_t& offset,
                                WaterCaptureArrayHeader& out) {
    offset = -1;
    if (!IsReadable(component, sizeof(UObjectHeader))) return false;
    void* cls = static_cast<UObjectHeader*>(component)->class_ptr;
    bool reflected_array = false;
    for (int depth = 0; depth < 16 && IsReadable(cls, sizeof(UStructHeader)); ++depth) {
        const auto* type = static_cast<const UStructHeader*>(cls);
        void* field = type->children;
        for (int i = 0; i < 4096 && IsReadable(field, sizeof(UFieldHeader)); ++i) {
            const auto* prop = static_cast<const UFieldHeader*>(field);
            if (WaterObjectName(field) == L"HiddenComponents" &&
                WaterObjectName(prop->object.class_ptr) == L"ArrayProperty") {
                reflected_array = true;
                break;
            }
            field = prop->next;
        }
        if (reflected_array) break;
        cls = type->super_struct;
    }
    if (!reflected_array) return false;
    offset = FindClassPropertyOffset(static_cast<UObjectHeader*>(component)->class_ptr,
                                     L"HiddenComponents");
    if (offset < 0 || offset > 0x4000 || !IsReadable(static_cast<uint8_t*>(component) + offset,
                                                   sizeof(WaterCaptureArrayHeader))) return false;
    std::memcpy(&out, static_cast<uint8_t*>(component) + offset, sizeof(out));
    return out.count >= 0 && out.count <= 1024 &&
        out.capacity >= out.count && out.capacity <= 4096 &&
        (!out.count || (out.data &&
            IsReadable(out.data, size_t(out.count) * sizeof(CaptureHiddenWeakRef))));
}

bool WaterGeometryHiddenListMatches(const WaterCaptureArrayHeader& header) {
    if (header.count != static_cast<int32_t>(s_geometry_hidden.size())) return false;
    for (int32_t i = 0; i < header.count; ++i) {
        CaptureHiddenWeakRef actual{};
        std::memcpy(&actual, header.data + size_t(i) * sizeof(actual), sizeof(actual));
        if (actual.index != s_geometry_hidden[size_t(i)].index ||
            actual.serial != s_geometry_hidden[size_t(i)].serial) return false;
    }
    return true;
}

bool WaterGeometryCaptureStillLive(const GlobalObjectArray* objects) {
    return objects && s_geometry_capture && s_geometry_capture_index >= 0 &&
        s_geometry_capture_index < objects->num_elements &&
        objects->objects[s_geometry_capture_index].object == s_geometry_capture &&
        objects->objects[s_geometry_capture_index].serial_number == s_geometry_capture_serial;
}

bool WaterRestoreGeometryReflections(const GlobalObjectArray* objects) {
    if (s_geometry_hidden.empty()) return true;
    if (!WaterGeometryCaptureStillLive(objects)) {
        // The old level has gone away; never dereference its components.
        s_geometry_hidden.clear();
        s_geometry_capture = nullptr;
        return true;
    }
    WaterCaptureArrayHeader list{};
    int32_t offset = -1;
    if (!WaterReadCaptureHiddenList(s_geometry_capture, offset, list) ||
        !WaterGeometryHiddenListMatches(list)) return false;
    auto* address = static_cast<uint8_t*>(s_geometry_capture) + offset + sizeof(void*);
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(address, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const DWORD protection = mbi.Protect & 0xffu;
    if (protection != PAGE_READWRITE && protection != PAGE_WRITECOPY &&
        protection != PAGE_EXECUTE_READWRITE && protection != PAGE_EXECUTE_WRITECOPY) return false;
    const int32_t zero = 0;
    std::memcpy(address, &zero, sizeof(zero));
    // No memory freed here: UE owns the array buffer and may reuse it.
    s_geometry_hidden.clear();
    if (WaterConfig().reflections_visible) {
        WaterSetVisible(s_geometry_capture, false);
        WaterSetVisible(s_geometry_capture, true);
        WaterWakePlanarReflection(s_geometry_capture);
    }
    Log(L"Water reflected geometry: restored original capture hidden-list (empty)");
    return true;
}

void WaterUpdateGeometryReflections(MyCharacter* player,
                                    const GlobalObjectArray* objects) {
    if (!player || !objects) return;
    const uint32_t hide_mask = WaterReflectionHideMask();
    const ULONGLONG now = GetTickCount64();
    // Compare the requested state even when a previous restore was blocked.
    // A new player, capture serial, or menu change retries immediately.
    struct RetryContext {
        MyCharacter* player = nullptr;
        uint32_t mask = ~0u;
        void* capture = nullptr;
        int32_t index = -1, serial = 0;
        bool visible = true;
    };
    static RetryContext previous{};
    const auto* target = s_reflection_targets.empty() ? nullptr : &s_reflection_targets.front();
    const RetryContext current_context{player, hide_mask,
        target ? target->component : nullptr, target ? target->index : -1,
        target ? target->serial : 0, WaterConfig().reflections_visible};
    if (previous.player != current_context.player || previous.mask != current_context.mask ||
        previous.capture != current_context.capture || previous.index != current_context.index ||
        previous.serial != current_context.serial || previous.visible != current_context.visible) {
        s_geometry_retry_after = 0;
        previous = current_context;
    }
    if (now < s_geometry_retry_after) return;
    const auto defer_retry = [&] { s_geometry_retry_after = now + 1000; };
    if (s_geometry_player != player) {
        s_geometry_player = player;
        s_geometry_candidates.clear();
        s_geometry_scan_cursor = hide_mask ? 0 : -1;
        s_geometry_apply_cursor = 0;
        s_geometry_scan_complete = false;
        s_geometry_unsupported_logged = false;
        s_geometry_enabled_logged = false;
    }
    // A change to any reflection category requires rebuilding the hidden list.
    // Never keep stale captures of excluded meshes from another category.
    if (s_geometry_applied_mask != hide_mask) {
        if (!WaterRestoreGeometryReflections(objects)) {
            if (!s_geometry_unsupported_logged) {
                s_geometry_unsupported_logged = true;
                Log(L"Water object reflections: restore blocked by changed capture list; reload level");
            }
            defer_retry();
            return;
        }
        s_geometry_applied_mask = hide_mask;
        s_geometry_candidates.clear();
        s_geometry_apply_cursor = 0;
        s_geometry_scan_cursor = hide_mask ? 0 : -1;
        s_geometry_scan_complete = false;
        s_geometry_unsupported_logged = false;
        s_geometry_enabled_logged = false;
    }
    if (!hide_mask) return;
    if (!WaterConfig().reflections_visible || s_reflection_targets.empty()) return;
    auto& capture = s_reflection_targets.front();
    if (!WaterReflectionTargetValid(capture, objects)) return;
    if (s_geometry_capture != capture.component ||
        !WaterGeometryCaptureStillLive(objects)) {
        // An older capture may still be alive after a player change. Avoid
        // abandoning exclusions we own unless we can restore them first.
        if (!s_geometry_hidden.empty() && WaterGeometryCaptureStillLive(objects) &&
            !WaterRestoreGeometryReflections(objects)) {
            defer_retry();
            return;
        }
        // New level/capture; never carry weak IDs into another object's list.
        s_geometry_hidden.clear();
        s_geometry_capture = capture.component;
        s_geometry_capture_index = capture.index;
        s_geometry_capture_serial = capture.serial;
        s_geometry_scan_cursor = 0;
        s_geometry_scan_complete = false;
        s_geometry_candidates.clear();
        s_geometry_apply_cursor = 0;
        s_geometry_unsupported_logged = false;
        s_geometry_enabled_logged = false;
    }
    if (s_geometry_scan_complete && s_geometry_enabled_logged &&
        s_geometry_apply_cursor >= s_geometry_candidates.size()) return;
    int32_t offset = -1;
    WaterCaptureArrayHeader current{};
    if (!WaterReadCaptureHiddenList(s_geometry_capture, offset, current) ||
        (s_geometry_hidden.empty() && current.count != 0) ||
        (!s_geometry_hidden.empty() && !WaterGeometryHiddenListMatches(current))) {
        if (!s_geometry_unsupported_logged) {
            s_geometry_unsupported_logged = true;
            Log(L"Water reflected geometry: HiddenComponents layout or ownership not verified; no writes");
        }
        defer_retry();
        return;
    }
    void* hide = FindReflectedFunction(s_geometry_capture, L"HideComponent");
    const int32_t input = WaterParam(hide, L"InComponent", L"ObjectProperty");
    if (input < 0 || !g_process_event) {
        if (!s_geometry_unsupported_logged) {
            s_geometry_unsupported_logged = true;
            Log(L"Water reflected geometry: HideComponent API unavailable; no writes");
        }
        defer_retry();
        return;
    }
    if (s_geometry_scan_cursor < 0 && !s_geometry_scan_complete)
        s_geometry_scan_cursor = 0;
    if (s_geometry_scan_cursor >= 0) {
        ColourWorkBudget budget(450);
        int inspected = 0;
        while (s_geometry_scan_cursor < objects->num_elements &&
               inspected++ < kReflectionScanBatch && !budget.expired()) {
            const auto& item = objects->objects[s_geometry_scan_cursor++];
            if (!item.object || !IsReadable(item.object, sizeof(UObjectHeader))) continue;
            const auto* actor = static_cast<UObjectHeader*>(item.object);
            if ((static_cast<uint32_t>(actor->flags) & 0x30u) != 0) continue;
            const auto type = WaterObjectName(actor->class_ptr);
            // Match only known gameplay actor types and their concrete, live
            // primitive components. Never hide a whole actor from the main world.
            if (WaterObjectName(item.object).find(L"Default__") == 0) continue;
            const auto collect = [&](void* component, ReflectionKind kind) {
                if (!(hide_mask & (1u << static_cast<uint8_t>(kind))) ||
                    !component || !IsReadable(component, sizeof(UObjectHeader)) ||
                    !WaterIsA(component, L"PrimitiveComponent")) return;
                const auto* header = static_cast<const UObjectHeader*>(component);
                if ((static_cast<uint32_t>(header->flags) & 0x30u) != 0) return;
                const int32_t idx = header->index;
                if (idx < 0 || idx >= objects->num_elements ||
                    objects->objects[idx].object != component) return;
                if (std::any_of(s_geometry_candidates.begin(), s_geometry_candidates.end(),
                    [&](const ReflectionGeometryCandidate& x) { return x.component == component; })) return;
                s_geometry_candidates.push_back({component, idx,
                    objects->objects[idx].serial_number, kind});
            };
            if (type == L"BP_IslandChunk_C")
                collect(ReadReflectedObject(item.object, L"BaseMesh"), ReflectionKind::Walls);
            else if (type == L"BP_Lift_C" || type == L"BP_Mover_C")
                collect(ReadReflectedObject(item.object, L"LiftMesh"), ReflectionKind::Walls);
            else if (type == L"BP_Button_C") {
                collect(ReadReflectedObject(item.object, L"Button"), ReflectionKind::Buttons);
                collect(ReadReflectedObject(item.object, L"Button base"), ReflectionKind::Buttons);
                void* beacon = ReadReflectedObject(item.object, L"Beacon");
                if (WaterReflectionVisualOwner(objects, beacon) == item.object)
                    collect(beacon, ReflectionKind::Beacons);
            }
            else if (type == L"BP_Jumppad_C" && (hide_mask & 16u)) {
                // Same actor/property used by native jump-pad colour customisation.
                void* mesh = ReadReflectedObject(item.object, L"Mover");
                if (WaterIsA(mesh, L"StaticMeshComponent") &&
                    static_cast<const UObjectHeader*>(mesh)->outer == item.object)
                    collect(mesh, ReflectionKind::JumpPads);
            } else if (type == L"BP_PowerCore_C" && (hide_mask & 32u)) {
                // The cube system resolves this exact reflected visual mesh.
                void* mesh = ReadReflectedObject(item.object, L"Mesh");
                if (WaterIsA(mesh, L"StaticMeshComponent") &&
                    static_cast<const UObjectHeader*>(mesh)->outer == item.object)
                    collect(mesh, ReflectionKind::Cubes);
            }
            // Refunct pipes can be Blueprint-added components without a
            // named property. Inspect the current object directly (one bounded
            // GUObjectArray pass, never an O(N^2) actor/component search).
            if ((hide_mask & 4u) && WaterIsA(item.object, L"StaticMeshComponent")) {
                void* owner = actor->outer;
                if (IsReadable(owner, sizeof(UObjectHeader)) &&
                    WaterObjectName(static_cast<UObjectHeader*>(owner)->class_ptr) ==
                        L"BP_TravelPipe_C")
                    collect(item.object, ReflectionKind::Pipes);
            }
            // Supplement the explicit property path with all live, owned
            // visual primitives. Keep button meshes separate from beacon FX.
            if (hide_mask & (8u | 16u | 32u)) {
                void* owner = WaterReflectionVisualOwner(objects, item.object);
                if (owner && WaterReflectionVisualComponent(item.object)) {
                    const auto owner_type = WaterObjectName(
                        static_cast<const UObjectHeader*>(owner)->class_ptr);
                    if (owner_type == L"BP_Jumppad_C")
                        collect(item.object, ReflectionKind::JumpPads);
                    else if (owner_type == L"BP_PowerCore_C")
                        collect(item.object, ReflectionKind::Cubes);
                    else if ((hide_mask & 8u) && owner_type == L"BP_Button_C" &&
                             item.object != ReadReflectedObject(owner, L"Button") &&
                             item.object != ReadReflectedObject(owner, L"Button base") &&
                             WaterReflectionIsBeaconVisual(objects, item.object, owner))
                        collect(item.object, ReflectionKind::Beacons);
                }
            }
            if (s_geometry_candidates.size() >= kMaxGeometryCandidates) break;
        }
        if (s_geometry_scan_cursor < objects->num_elements &&
            s_geometry_candidates.size() < kMaxGeometryCandidates) return;
        s_geometry_scan_cursor = -1;
        s_geometry_scan_complete = true;
        std::array<size_t, 6> category_counts{};
        for (const auto& candidate : s_geometry_candidates)
            ++category_counts[static_cast<size_t>(candidate.kind)];
        Log((L"Water reflected geometry: candidates walls/platforms=" +
             std::to_wstring(category_counts[0]) + L", buttons=" +
             std::to_wstring(category_counts[1]) + L", pipes=" +
             std::to_wstring(category_counts[2]) + L", beacon beams=" +
             std::to_wstring(category_counts[3]) + L", jump pads=" +
             std::to_wstring(category_counts[4]) + L", cubes=" +
             std::to_wstring(category_counts[5])).c_str());
        for (size_t i = 0; i < category_counts.size(); ++i) {
            if ((hide_mask & (1u << i)) && category_counts[i] == 0) {
                const wchar_t* labels[] = {L"walls/platforms", L"buttons", L"pipes",
                    L"beacon beams", L"jump pads", L"cubes"};
                Log((std::wstring(L"Water reflected geometry: no live visual components for ") +
                     labels[i]).c_str());
            }
        }
    }
    // Apply only a few exclusions per frame to keep frame pacing stable.
    ColourWorkBudget apply_budget(450);
    size_t applied = 0;
    while (s_geometry_apply_cursor < s_geometry_candidates.size() &&
           applied++ < kGeometryApplyBatch && !apply_budget.expired()) {
        const auto& candidate = s_geometry_candidates[s_geometry_apply_cursor++];
        if (!WaterGeometryCandidateValid(candidate, objects)) continue;
        // A zero serial means UE has not allocated a weak reference yet.
        // Capture the current live serial immediately before HideComponent.
        const auto before_serial = objects->objects[candidate.index].serial_number;
        // Player changes need not hide components twice when the capture is
        // still the same. The weak pointer (index, serial) is stable for a
        // live component in this loaded level.
        if (std::find_if(s_geometry_hidden.begin(), s_geometry_hidden.end(),
            [&](const CaptureHiddenWeakRef& x) {
                return x.index == candidate.index && x.serial == before_serial;
            }) != s_geometry_hidden.end()) continue;
        WaterCaptureArrayHeader before{};
        if (!WaterReadCaptureHiddenList(s_geometry_capture, offset, before) ||
            !WaterGeometryHiddenListMatches(before)) {
            s_geometry_unsupported_logged = true;
            s_geometry_apply_cursor = s_geometry_candidates.size();
            Log(L"Water reflected geometry: hidden list changed before exclusion; stopped");
            break;
        }
        alignas(16) std::array<uint8_t,128> args{};
        std::memcpy(args.data() + input, &candidate.component, sizeof(void*));
        g_process_event(s_geometry_capture, hide, args.data());
        WaterCaptureArrayHeader after{};
        if (!WaterReadCaptureHiddenList(s_geometry_capture, offset, after) ||
            after.count != before.count + 1 || !after.data ||
            !IsReadable(after.data + size_t(before.count) * sizeof(CaptureHiddenWeakRef),
                        sizeof(CaptureHiddenWeakRef))) {
            if (!s_geometry_unsupported_logged) {
                s_geometry_unsupported_logged = true;
                Log(L"Water reflected geometry: HideComponent did not produce expected list; stopped");
            }
            // Do not perform additional writes when the engine's list behaves
            // differently than expected; never guess how to roll it back.
            s_geometry_apply_cursor = s_geometry_candidates.size();
            break;
        }
        // Make sure the engine preserved every previously tracked entry.
        WaterCaptureArrayHeader prefix = after;
        prefix.count = before.count;
        if (!WaterGeometryHiddenListMatches(prefix)) {
            s_geometry_apply_cursor = s_geometry_candidates.size();
            s_geometry_unsupported_logged = true;
            Log(L"Water reflected geometry: existing capture entries changed; stopped");
            break;
        }
        CaptureHiddenWeakRef last{};
        std::memcpy(&last, after.data + size_t(before.count) * sizeof(last), sizeof(last));
        // HideComponent can allocate the first weak-reference serial. Validate
        // the appended entry against the live registry AFTER the call, not
        // against the previously unassigned (zero) serial.
        const auto& live = objects->objects[candidate.index];
        if (last.index != candidate.index || last.serial <= 0 ||
            live.object != candidate.component || live.serial_number != last.serial ||
            (before_serial != 0 && before_serial != last.serial)) {
            s_geometry_apply_cursor = s_geometry_candidates.size();
            if (!s_geometry_unsupported_logged) {
                s_geometry_unsupported_logged = true;
                Log(L"Water reflected geometry: weak pointer layout mismatch; stopped");
            }
            break;
        }
        s_geometry_hidden.push_back(last);
    }
    if (s_geometry_scan_complete && s_geometry_apply_cursor == s_geometry_candidates.size() &&
        !s_geometry_enabled_logged) {
        s_geometry_enabled_logged = true;
        // Refresh the capture once, after the batched exclusions have landed.
        // Never force it ON when All water reflections is intentionally OFF.
        if (s_geometry_unsupported_logged) return;
        if (!s_geometry_hidden.empty() && WaterConfig().reflections_visible) {
            WaterSetVisible(s_geometry_capture, false);
            WaterSetVisible(s_geometry_capture, true);
            WaterWakePlanarReflection(s_geometry_capture);
        }
        std::array<size_t, 6> excluded_counts{};
        for (const auto& candidate : s_geometry_candidates) {
            if (std::any_of(s_geometry_hidden.begin(), s_geometry_hidden.end(),
                [&](const CaptureHiddenWeakRef& ref) { return ref.index == candidate.index; }))
                ++excluded_counts[static_cast<size_t>(candidate.kind)];
        }
        Log((L"Water reflected geometry: applied beacon beams=" +
             std::to_wstring(excluded_counts[3]) + L", jump pads=" +
             std::to_wstring(excluded_counts[4]) + L", cubes=" +
             std::to_wstring(excluded_counts[5])).c_str());
        Log((L"Water reflected geometry: capture excluded " +
            std::to_wstring(s_geometry_hidden.size()) +
            L" selected object/beam components (world meshes untouched)").c_str());
    }
}

// Apply only to live, map-owned native PlanarReflection components. No native
// offset writes, render hooks, material edits or process-wide engine CVars.
bool WaterReflectionTargetValid(const WaterReflectionTarget& target,
                                const GlobalObjectArray* objects) {
    if (!objects || !target.component || target.index < 0 ||
        target.index >= objects->num_elements) return false;
    const auto& item = objects->objects[target.index];
    if (item.object != target.component || item.serial_number != target.serial ||
        !IsReadable(item.object, sizeof(UObjectHeader))) return false;
    return (static_cast<uint32_t>(
        static_cast<const UObjectHeader*>(item.object)->flags) & 0x30u) == 0;
}

bool WaterIsLivePlanarReflection(void* candidate) {
    if (!IsReadable(candidate, sizeof(UObjectHeader))) return false;
    const auto* component = static_cast<const UObjectHeader*>(candidate);
    if ((static_cast<uint32_t>(component->flags) & 0x30u) != 0) return false;
    // Only cache the actual class (not instances) to avoid thousands of
    // repeated FName conversions during the initial bounded scan.
    bool planar = false;
    const auto found = s_reflection_class_cache.find(component->class_ptr);
    if (found != s_reflection_class_cache.end()) planar = found->second;
    else {
        planar = WaterIsA(candidate, L"PlanarReflectionComponent");
        if (s_reflection_class_cache.size() < 4096)
            s_reflection_class_cache.emplace(component->class_ptr, planar);
    }
    if (!planar) return false;
    // Reject CDOs, editor templates, and orphan components. Require an
    // actual APlanarReflection owner in the shipped level, not a name match.
    void* owner = component->outer;
    for (int depth = 0; depth < 8 && IsReadable(owner, sizeof(UObjectHeader)); ++depth) {
        const auto* hdr = static_cast<const UObjectHeader*>(owner);
        if ((static_cast<uint32_t>(hdr->flags) & 0x30u) != 0) return false;
        if (WaterIsA(owner, L"PlanarReflection"))
            return WaterObjectName(owner).find(L"Default__") != 0;
        if (WaterIsA(owner, L"Actor")) return false;
        owner = hdr->outer;
    }
    return false;
}

void WaterUpdateReflections(MyCharacter* player) {
    const bool wanted = WaterConfig().reflections_visible;
    const bool geometry_wanted = WaterReflectionHideMask() == 0;
    if (s_reflection_player != player) {
        s_reflection_player = player;
        s_reflection_targets.clear();
        s_reflection_class_cache.clear();
        s_reflection_scan_cursor = -1;
        s_reflection_scan_complete = false;
        s_reflection_retry_after = 0;
        s_reflection_missing_logged = false;
    }
    if (!player || (wanted && geometry_wanted && s_reflection_targets.empty() &&
                    !s_reflection_restore_pending)) return;
    const ULONGLONG now = GetTickCount64();
    // On stable captured components, call Unreal's visibility UFunctions at
    // most once a second. An explicit toggle bypasses this.
    if (!s_reflection_apply_requested && s_reflection_scan_cursor < 0) {
        if (!s_reflection_targets.empty() && now < s_reflection_next_verify) return;
        if (s_reflection_targets.empty() && s_reflection_scan_complete &&
            now < s_reflection_retry_after) return;
    }
    // No registry read is required when neither discovery nor verification is due.
    const auto* objects = WaterObjects();
    if (!objects) return;
    s_reflection_apply_requested = false;
    s_reflection_next_verify = now + 1000;
    // Cached refs are checked with GUObjectArray serials before every write.
    // No scanning or UObject calls occur when the setting is ON and no target
    // has ever been changed.
    for (auto it = s_reflection_targets.begin(); it != s_reflection_targets.end();) {
        if (!WaterReflectionTargetValid(*it, objects)) {
            it = s_reflection_targets.erase(it);
            s_reflection_scan_complete = false;
            continue;
        }
        bool current = false;
        if (!WaterGetVisible(it->component, current)) {
            ++it;
            continue;
        }
        if (wanted && (it->modified || (!geometry_wanted && !current) ||
                       (s_reflection_restore_pending && !it->restored_this_cycle))) {
            // "ON" must explicitly enable the verified capture. Restoring the
            // first observed value is wrong when the component was discovered
            // after an earlier OFF and its cached "original" is already OFF.
            if (current || WaterSetVisible(it->component, true)) {
                WaterWakePlanarReflection(it->component);
                it->restored_this_cycle = true;
                it->modified = false;
                it->warning_logged = false;
                Log((L"Water reflections: " + WaterObjectName(it->component) +
                     L" restored ON; visibility=ON; capture wake attempted").c_str());
            } else if (!it->warning_logged) {
                it->warning_logged = true;
                Log(L"Water reflections: restore SetVisibility(true) failed");
            }
        } else if (!wanted) {
            it->restored_this_cycle = false;
            if (current) {
                if (WaterSetVisible(it->component, false)) {
                    it->modified = true;
                    it->warning_logged = false;
                    Log((L"Water reflections: " + WaterObjectName(it->component) +
                        L" disabled OFF (SetVisibility verified)").c_str());
                } else if (!it->warning_logged) {
                    it->warning_logged = true;
                    Log(L"Water reflections: SetVisibility(false) failed; component unchanged");
                }
            }
        }
        ++it;
    }
    // Finish an ON request only after every cached capture is verified restored.
    // A failed setter remains eligible for the next bounded retry.
    if (wanted && s_reflection_restore_pending && s_reflection_scan_complete &&
        s_reflection_scan_cursor < 0 && !s_reflection_targets.empty() &&
        std::all_of(s_reflection_targets.begin(), s_reflection_targets.end(),
                    [](const WaterReflectionTarget& t) { return t.restored_this_cycle; })) {
        s_reflection_restore_pending = false;
    }
    if ((!wanted || !geometry_wanted || s_reflection_restore_pending) &&
        s_reflection_targets.empty() && s_reflection_scan_complete &&
        now >= s_reflection_retry_after) {
        // Retrying *only when missing*, not rediscovering found pillars/actors.
        s_reflection_scan_complete = false;
    }
    if (s_reflection_scan_cursor < 0 &&
        ((!wanted || !geometry_wanted || s_reflection_restore_pending) && !s_reflection_scan_complete))
        s_reflection_scan_cursor = 0;
    if (s_reflection_scan_cursor < 0) return;

    // Part of the same shared per-frame cosmetic CPU budget as all colour
    // modules, protecting frame pacing during one-time actor discovery.
    ColourWorkBudget budget(450);
    int32_t examined = 0;
    while (s_reflection_scan_cursor < objects->num_elements &&
           examined++ < kReflectionScanBatch && !budget.expired()) {
        const int32_t idx = s_reflection_scan_cursor++;
        const auto& item = objects->objects[idx];
        if (!item.object || !WaterIsLivePlanarReflection(item.object)) continue;
        if (std::any_of(s_reflection_targets.begin(), s_reflection_targets.end(),
            [&](const WaterReflectionTarget& t) {return t.component == item.object;})) continue;
        bool current = true;
        if (!WaterGetVisible(item.object, current)) {
            Log((L"Water reflections: found " + WaterObjectName(item.object) +
                L" but IsVisible is unavailable").c_str());
            continue;
        }
        bool active = false;
        const bool active_known = WaterReadComponentActive(item.object, active);
        s_reflection_targets.push_back({item.object, idx, item.serial_number,
                                        current, false, false, active_known && active});
        Log((L"Water reflections: found " + WaterObjectName(item.object) +
            L" (PlanarReflectionComponent, initial visibility " +
            (current ? L"ON" : L"OFF") + L")").c_str());
        if (!wanted && current) {
            if (WaterSetVisible(item.object, false)) {
                s_reflection_targets.back().modified = true;
                Log(L"Water reflections: captured component disabled OFF (verified)");
            } else {
                s_reflection_targets.back().warning_logged = true;
                Log(L"Water reflections: initial SetVisibility OFF failed");
            }
        } else if (wanted && (s_reflection_restore_pending || !current)) {
            // A saved object-exclusion choice can require discovery at launch.
            // Ensure ON really enables the capture even if initially hidden.
            // Discovery may first occur while the capture is already OFF.
            // Never save that state and use it as the requested ON state.
            if (current || WaterSetVisible(item.object, true)) {
                WaterWakePlanarReflection(item.object);
                s_reflection_targets.back().restored_this_cycle = true;
                Log(L"Water reflections: discovered component restored ON");
            } else {
                s_reflection_targets.back().warning_logged = true;
                Log(L"Water reflections: discovered component restore ON failed");
            }
        }
        if (s_reflection_targets.size() >= kMaxReflectionTargets) break;
    }
    if (s_reflection_scan_cursor < objects->num_elements &&
        s_reflection_targets.size() < kMaxReflectionTargets) return;
    s_reflection_scan_cursor = -1;
    s_reflection_scan_complete = true;
    s_reflection_retry_after = now + 15000;
    if (s_reflection_targets.empty() && !s_reflection_missing_logged) {
        Log((L"Water reflections: scan complete; active PlanarReflection components=" +
             std::to_wstring(s_reflection_targets.size()) +
             (wanted ? L"; requested ON" : L"; requested OFF") +
             L".").c_str());
    }
    s_reflection_missing_logged = s_reflection_missing_logged || s_reflection_targets.empty();
    if (wanted && !s_reflection_targets.empty() &&
        std::all_of(s_reflection_targets.begin(), s_reflection_targets.end(),
        [](const WaterReflectionTarget& t) { return t.restored_this_cycle; }))
        s_reflection_restore_pending = false;
}

// Only identify effect components by their own type and their reflected
// particle-system asset. In particular, do not touch bubbles/beacons,
// unrelated particles, templates, actors, or all water-volume components.
bool WaterSplashName(const std::wstring& source) {
    std::wstring name = source;
    for (wchar_t& ch : name) ch = static_cast<wchar_t>(towlower(ch));
    return name.find(L"splash") != std::wstring::npos ||
           name.find(L"ripple") != std::wstring::npos ||
           name.find(L"water") != std::wstring::npos;
}
bool WaterSplashComponent(void* component) {
    if (!IsReadable(component, sizeof(UObjectHeader))) return false;
    const auto* hdr = static_cast<const UObjectHeader*>(component);
    if ((static_cast<uint32_t>(hdr->flags) & 0x30u) != 0) return false;
    // The class hierarchy is stable across map reloads. Cache per class to
    // avoid expensive FName conversion for every unrelated UObject on scans.
    static std::unordered_map<void*, uint8_t> component_class_cache;
    uint8_t kind = 0;
    if (auto it = component_class_cache.find(hdr->class_ptr);
        it != component_class_cache.end()) kind = it->second;
    else {
        kind = WaterIsA(component, L"ParticleSystemComponent") ? 1 :
               WaterIsA(component, L"NiagaraComponent") ? 2 : 0;
        if (component_class_cache.size() < 4096)
            component_class_cache.emplace(hdr->class_ptr, kind);
    }
    if (!kind) return false;
    const bool cascade = kind == 1;
    // Avoid class defaults / prefabs: the component must belong to a live actor.
    bool live = false;
    void* owner = hdr->outer;
    for (int depth = 0; depth < 8 && IsReadable(owner, sizeof(UObjectHeader)); ++depth) {
        const auto* o = static_cast<const UObjectHeader*>(owner);
        if ((static_cast<uint32_t>(o->flags) & 0x30u) != 0) return false;
        if (WaterIsA(owner, L"Actor")) { live = true; break; }
        owner = o->outer;
    }
    if (!live) return false;
    // Prefer the named asset to the generic ParticleSystemComponent_123 name.
    const wchar_t* property_name = cascade ? L"Template" : L"Asset";
    static std::unordered_map<void*, int32_t> asset_offset_cache;
    int32_t offset = -1;
    if (const auto it = asset_offset_cache.find(hdr->class_ptr);
        it != asset_offset_cache.end()) offset = it->second;
    else {
        void* klass = hdr->class_ptr;
        for (int depth = 0; depth < 12 && IsReadable(klass, sizeof(UStructHeader)); ++depth) {
            auto* field = static_cast<const UStructHeader*>(klass)->children;
            for (int examined = 0; examined < 1024 && IsReadable(field, sizeof(UFieldHeader)); ++examined) {
                const auto* prop = static_cast<const UFieldHeader*>(field);
                if (UnrealName(prop->object.name) == property_name &&
                    WaterObjectName(prop->object.class_ptr) == L"ObjectProperty") {
                    offset = FindClassPropertyOffset(hdr->class_ptr, property_name);
                    break;
                }
                field = prop->next;
            }
            if (offset >= 0) break;
            klass = static_cast<const UStructHeader*>(klass)->super_struct;
        }
        if (asset_offset_cache.size() < 4096)
            asset_offset_cache.emplace(hdr->class_ptr, offset);
    }
    if (offset >= 0 && offset < 0x2000) {
        auto* address = static_cast<const uint8_t*>(component) + offset;
        if (IsReadable(address, sizeof(void*))) {
            void* asset = nullptr;
            std::memcpy(&asset, address, sizeof(asset));
            if (IsReadable(asset, sizeof(UObjectHeader))) {
                const bool correct_type = cascade ? WaterIsA(asset, L"ParticleSystem") :
                                                   WaterIsA(asset, L"NiagaraSystem");
                if (correct_type) return WaterSplashName(WaterObjectName(asset));
            }
        }
    }
    // Some game-created components have no reflected asset pointer. Accept
    // only explicitly water-named components in that case.
    return WaterSplashName(WaterObjectName(component));
}
bool WaterSplashTargetValid(const SplashTarget& target,
                            const GlobalObjectArray* objects) {
    return objects && target.index >= 0 && target.index < objects->num_elements &&
        objects->objects[target.index].object == target.component &&
        objects->objects[target.index].serial_number == target.serial &&
        IsReadable(target.component, sizeof(UObjectHeader));
}
void WaterRestoreSplashTargets(const GlobalObjectArray* objects) {
    for (auto& target : s_splash_targets) {
        if (!WaterSplashTargetValid(target, objects)) { target.component = nullptr; continue; }
        bool visible = false;
        // If the game already changed visibility, do not overwrite it.
        if (WaterGetVisible(target.component, visible) && !visible &&
            WaterSetVisible(target.component, target.original_visible))
            target.component = nullptr;
        else if (visible) target.component = nullptr;
    }
    s_splash_targets.erase(std::remove_if(s_splash_targets.begin(),
        s_splash_targets.end(), [](const SplashTarget& t) { return !t.component; }),
        s_splash_targets.end());
}
void WaterUpdateSplashEffects() {
    const auto* objects = WaterObjects();
    if (!objects) return;
    if (!WaterNoSplashes()) {
        if (!s_splash_targets.empty()) WaterRestoreSplashTargets(objects);
        s_splash_scan_cursor = -1;
        return;
    }
    const ULONGLONG now = GetTickCount64();
    if (now < s_splash_retry_after) return;
    for (auto& target : s_splash_targets) {
        if (!WaterSplashTargetValid(target, objects)) continue;
        bool visible = false;
        if (WaterGetVisible(target.component, visible) && visible &&
            !WaterSetVisible(target.component, false)) {
            s_splash_retry_after = GetTickCount64() + 1000;
            return;
        }
    }
    if (s_splash_scan_cursor < 0 && now >= s_splash_next_scan)
        s_splash_scan_cursor = 0;
    if (s_splash_scan_cursor < 0) return;
    const ULONGLONG begin_time = now;
    int processed = 0;
    while (s_splash_scan_cursor < objects->num_elements && processed++ < kScanBatch &&
           s_splash_targets.size() < kMaxSplashTargets) {
        if ((processed & 127) == 0 && GetTickCount64() - begin_time >= kScanCpuBudgetMs)
            break;
        const int32_t index = s_splash_scan_cursor++;
        const auto& item = objects->objects[index];
        if (!IsReadable(item.object, sizeof(UObjectHeader))) continue;
        const auto* hdr = static_cast<const UObjectHeader*>(item.object);
        if ((static_cast<uint32_t>(hdr->flags) & 0x30u) != 0) continue;
        // Do not repeat reflection/visibility calls for components already hidden.
        bool tracked = false;
        for (const auto& t : s_splash_targets)
            if (t.component == item.object && t.serial == item.serial_number) {
                tracked = true; break;
            }
        if (tracked || !WaterSplashComponent(item.object)) continue;
        bool visible = false;
        if (!WaterGetVisible(item.object, visible) || !visible) continue;
        // Record BEFORE the native setter, so a setter that succeeds but
        // fails verification still has its original state available to restore.
        s_splash_targets.push_back({item.object, index, item.serial_number, true});
        if (!WaterSetVisible(item.object, false)) {
            Log(L"Water splashes: particle visibility write unverified; will retry");
            s_splash_retry_after = GetTickCount64() + 1000;
        } else if (!s_splash_logged) {
            s_splash_logged = true;
            Log((L"Water splashes: hiding water particle system " +
                 WaterObjectName(item.object)).c_str());
        }
    }
    if (s_splash_scan_cursor >= objects->num_elements ||
        s_splash_targets.size() >= kMaxSplashTargets) {
        s_splash_scan_cursor = -1;
        s_splash_next_scan = GetTickCount64() + 120;
        // Remove entries freed by the engine; the serial check prevents
        // restoring visibility on an object that reused the same address.
        s_splash_targets.erase(std::remove_if(s_splash_targets.begin(),
            s_splash_targets.end(), [objects](const SplashTarget& t) {
                return !WaterSplashTargetValid(t, objects);
            }), s_splash_targets.end());
    }
}
bool WaterGetLocation(void* mesh, Vec3& location) {
    if (!g_process_event) return false;
    void* fn = FindReflectedFunction(mesh, L"K2_GetComponentLocation");
    if (!fn) fn = FindReflectedFunction(mesh, L"GetComponentLocation");
    const int32_t result = WaterParam(fn, L"ReturnValue", L"StructProperty");
    if (result < 0) return false;
    alignas(16) std::array<uint8_t, 128> args{};
    g_process_event(mesh, fn, args.data());
    std::memcpy(&location, args.data() + result, sizeof(location));
    return std::isfinite(location.x) && std::isfinite(location.y) &&
           std::isfinite(location.z);
}
bool WaterSetLocation(void* mesh, const Vec3& location) {
    if (!g_process_event || !std::isfinite(location.x) ||
        !std::isfinite(location.y) || !std::isfinite(location.z)) return false;
    void* fn = FindReflectedFunction(mesh, L"K2_SetWorldLocation");
    const int32_t value = WaterParam(fn, L"NewLocation", L"StructProperty");
    const int32_t teleport = WaterParam(fn, L"bTeleport", L"BoolProperty");
    if (value < 0) return false;
    // K2_SetWorldLocation has a SweepHitResult output, so use a large
    // correctly aligned parameter frame rather than just a FVector buffer.
    alignas(16) std::array<uint8_t, 256> args{};
    std::memcpy(args.data() + value, &location, sizeof(location));
    if (teleport >= 0) args[teleport] = 1;
    g_process_event(mesh, fn, args.data());
    Vec3 actual{};
    return WaterGetLocation(mesh, actual) &&
           std::fabs(actual.x - location.x) < 1.0f &&
           std::fabs(actual.y - location.y) < 1.0f &&
           std::fabs(actual.z - location.z) < 1.0f;
}
void WaterGeometryUnsupported(const wchar_t* operation) {
    if (s_world_geometry_warning_logged) return;
    s_world_geometry_warning_logged = true;
    Log((std::wstring(L"Water gameplay: reflected ") + operation +
         L" failed on an ocean mesh; setting will retry").c_str());
    Notify(L"Water setting could not be applied; see RefunctUtility.log");
}
void WaterApplyWorldGeometry(WaterTarget& target, bool remove, int height) {
    const ULONGLONG now = GetTickCount64();
    if (now < target.geometry_retry_after) return;
    if (target.last_removed == remove && target.last_height_offset == height &&
        now < target.geometry_verify_after) return;
    // Visibility and position are independent: restoring one must not
    // overwrite the other's native state.
    if (remove) {
        if (!target.visibility_modified) {
            bool visible = false;
            if (!WaterGetVisible(target.actor, visible)) {
                WaterGeometryUnsupported(L"IsVisible");
                target.geometry_retry_after = now + 1000;
                return;
            }
            target.original_visible = visible;
            if (visible) {
                // The native setter may succeed even when its read-back fails.
                // Remember the original state before the call so a later OFF
                // request always knows to restore visibility.
                target.visibility_modified = true;
                if (!WaterSetVisible(target.actor, false)) {
                    WaterGeometryUnsupported(L"SetVisibility");
                    target.geometry_retry_after = now + 1000;
                    return;
                }
            }
        } else {
            bool visible = false;
            if (WaterGetVisible(target.actor, visible) && visible &&
                !WaterSetVisible(target.actor, false)) {
                WaterGeometryUnsupported(L"SetVisibility");
                target.geometry_retry_after = now + 1000;
                return;
            }
        }
    } else if (target.visibility_modified) {
        if (!WaterSetVisible(target.actor, target.original_visible)) {
            WaterGeometryUnsupported(L"SetVisibility restoration");
            target.geometry_retry_after = now + 1000;
            return;
        }
        target.visibility_modified = false;
    }
    if (height != 0) {
        Vec3 now_location{};
        if (!WaterGetLocation(target.actor, now_location)) {
            WaterGeometryUnsupported(L"GetComponentLocation");
            target.geometry_retry_after = now + 1000;
            return;
        }
        if (!target.original_location_known) {
            target.original_location = now_location;
            target.original_location_known = true;
        }
        Vec3 desired = target.original_location;
        desired.z += static_cast<float>(height);
        if (!std::isfinite(desired.z)) return;
        // Mark pending restoration *before* the setter: it might move the
        // component but fail verification, and must never lose the baseline.
        target.height_modified = true;
        if (std::fabs(now_location.z - desired.z) > 0.5f &&
            !WaterSetLocation(target.actor, desired)) {
            WaterGeometryUnsupported(L"SetWorldLocation");
            target.geometry_retry_after = now + 1000;
            return;
        }
    } else if (target.height_modified) {
        if (!WaterSetLocation(target.actor, target.original_location)) {
            WaterGeometryUnsupported(L"SetWorldLocation restoration");
            target.geometry_retry_after = now + 1000;
            return;
        }
        target.height_modified = false;
        target.original_location_known = false;
    }
    target.last_removed = remove;
    target.last_height_offset = height;
    target.geometry_verify_after = now + 750;
}
void WaterDiscover(const GlobalObjectArray* objects) {
    const int32_t begin = s_scan_cursor;
    const int32_t end = (std::min)(objects->num_elements, begin + kScanBatch);
    ColourWorkBudget budget(700);
    for (; s_scan_cursor < end; ++s_scan_cursor) {
        if (budget.expired()) break;
        const auto& item = objects->objects[s_scan_cursor];
        if (!item.object || (static_cast<uint32_t>(item.flags) & 0x30000000u) ||
            !IsReadable(item.object, sizeof(UObjectHeader))) continue;
        // Scan for genuine runtime FNames alongside mesh discovery. Class
        // names are cached, so this does not rename every UObject each tick.
        if (!s_ocean_parameter_ready) {
            WaterObserveLoadedParameter(item.object);
            // Restrict this reflected library call to the one expected CDO.
            if ((static_cast<uint32_t>(item.object->flags) & 0x10u) != 0 &&
                WaterObjectName(item.object) == L"Default__KismetStringLibrary")
                WaterObserveNameLibrary(item.object);
        }
        if (!WaterIsLevelWaterComponent(item.object)) continue;
        ++s_examined_meshes;
        // Check the live assigned shader, not the component/actor name.
        void* material = WaterGetMaterial(item.object);
        if (!material || !WaterIsOceanMaterial(material)) continue;
        ++s_ocean_meshes;
        if (!s_ocean_parameter_ready) {
            FNameView parameter{};
            WaterBaseColourParameter(material, parameter);
        }
        const bool found = std::any_of(s_targets.begin(), s_targets.end(),
            [&](const WaterTarget& t) { return t.actor == item.object &&
                                             t.serial == item.serial_number; });
        if (found) continue;
        if (s_targets.size() >= kMaxWaterActors) break;
        WaterTarget target{};
        target.actor = item.object;
        target.index = s_scan_cursor;
        target.serial = item.serial_number;
        target.mesh = item.object;
        s_targets.push_back(target);
    }
    if (s_scan_cursor >= objects->num_elements || s_targets.size() >= kMaxWaterActors) {
        s_scan_cursor = -1;
        s_discovery_watermark.Complete(objects->objects, objects->num_elements, GetTickCount64(),
            s_targets.empty() || WaterSurfaceHidden() || WaterConfig().height_offset != 0 ? 5000 : 47000, s_next_scan);
        if (!s_reported) {
            s_reported = true;
            Log((L"Water colours: scanned " + std::to_wstring(s_examined_meshes) +
                L" live StaticMeshComponents; " + std::to_wstring(s_ocean_meshes) +
                L" ocean-material matches; " + std::to_wstring(s_targets.size()) +
                L" tracked meshes; base_color resolved=" +
                (s_ocean_parameter_ready ? L"yes" : L"no")).c_str());
        }
    }
}
// Reflection-based lookup avoids hard-coding FPostProcessSettings offsets.
// UScriptStruct "PostProcessSettings" is a loaded UObject; its child UProperty
// fields describe the same native member offsets used inside volume.Settings.
int32_t UnderwaterMemberOffset(void* type, const wchar_t* name,
                               const wchar_t* property_class) {
    for (int depth = 0; depth < 12 && IsReadable(type, sizeof(UStructHeader)); ++depth) {
        const auto* structure = static_cast<const UStructHeader*>(type);
        int examined = 0;
        for (void* field = structure->children;
             examined++ < 4096 && IsReadable(field, sizeof(UFieldHeader)); ) {
            const auto* property = static_cast<const UFieldHeader*>(field);
            if (UnrealName(property->object.name) == name) {
                if (WaterObjectName(property->object.class_ptr) != property_class)
                    return -1;
                return FindClassPropertyOffset(type, name);
            }
            field = property->next;
        }
        type = structure->super_struct;
    }
    return -1;
}
// The bool descriptor's offset inside UBoolProperty is NOT the same as
// UProperty::Offset_Internal. Refunct's earlier +0x58 assumption is known to
// reject valid fields. Resolve the descriptor from reflected neighbouring
// bOverride properties, not from an unverified hard-coded x86 layout.
struct UnderwaterBoolField {
    const uint8_t* descriptor = nullptr;
    int32_t property_offset = -1;
    bool name_found = false;
    bool type_correct = false;
};
UnderwaterBoolField UnderwaterFindBoolField(void* type, const wchar_t* name) {
    UnderwaterBoolField result{};
    for (int depth = 0; depth < 12 && IsReadable(type, sizeof(UStructHeader)); ++depth) {
        const auto* structure = static_cast<const UStructHeader*>(type);
        int inspected = 0;
        for (void* field = structure->children;
             inspected++ < 4096 && IsReadable(field, sizeof(UFieldHeader)); ) {
            const auto* entry = static_cast<const UFieldHeader*>(field);
            if (UnrealName(entry->object.name) == name) {
                result.name_found = true;
                result.type_correct = WaterObjectName(entry->object.class_ptr) ==
                                      L"BoolProperty";
                if (result.type_correct && IsReadable(field, 0x40)) {
                    result.descriptor = static_cast<const uint8_t*>(field);
                    result.property_offset = FindClassPropertyOffset(type, name);
                }
                return result;
            }
            field = entry->next;
        }
        type = structure->super_struct;
    }
    return result;
}
struct UnderwaterBoolBits {
    int32_t address_offset = -1;
    uint8_t mask = 0;
    uint8_t field_size = 0;
};
bool UnderwaterReadBoolBits(const UnderwaterBoolField& field, size_t descriptor_offset,
                            UnderwaterBoolBits& out) {
    if (!field.descriptor || field.property_offset < 0 ||
        field.property_offset > 0x1800 - 8 ||
        !IsReadable(field.descriptor, descriptor_offset + 4)) return false;
    const uint8_t* d = field.descriptor + descriptor_offset;
    const uint8_t field_size = d[0];
    const uint8_t byte_offset = d[1];
    const uint8_t byte_mask = d[2];
    const uint8_t field_mask = d[3];
    // These are single-bit UPROPERTY override fields, not native bools.
    if ((field_size != 1 && field_size != 2 && field_size != 4 &&
         field_size != 8) || byte_offset >= field_size ||
        !byte_mask || (byte_mask & static_cast<uint8_t>(byte_mask - 1)) ||
        (field_mask != byte_mask && field_mask != 0xFF)) return false;
    const int32_t address_offset = field.property_offset + byte_offset;
    if (address_offset < 0 || address_offset >= 0x1800) return false;
    out = {address_offset, byte_mask, field_size};
    return true;
}
// All three fields are consecutive bitfields in UE4's FPostProcessSettings.
// Require an independently reflected neighbour to corroborate the descriptor
// layout and byte-mask ordering before writing into native game memory.
bool UnderwaterAdjacentBoolBits(const UnderwaterBoolBits& left,
                                const UnderwaterBoolBits& right) {
    if (left.field_size != right.field_size ||
        right.address_offset < left.address_offset ||
        right.address_offset > left.address_offset + 1) return false;
    if (right.address_offset == left.address_offset) {
        return left.mask != 0x80 && right.mask == (left.mask << 1);
    }
    return left.mask == 0x80 && right.mask == 1;
}
bool UnderwaterOverrideMember(void* type, int32_t& offset, uint8_t& mask) {
    offset = -1;
    mask = 0;
    const auto before = UnderwaterFindBoolField(type, L"bOverride_FilmWhiteClip");
    const auto target = UnderwaterFindBoolField(type, L"bOverride_SceneColorTint");
    const auto after = UnderwaterFindBoolField(type, L"bOverride_SceneFringeIntensity");
    if (!target.name_found || !target.type_correct || !target.descriptor ||
        target.property_offset < 0) {
        if (!s_underwater_resolution_warned) {
            Log((L"Underwater colours: SceneColorTint override property " +
                 std::wstring(!target.name_found ? L"not reflected" :
                     (!target.type_correct ? L"has unexpected type" :
                                              L"has invalid metadata"))).c_str());
        }
        return false;
    }
    // Older UE4 x86 builds differ in the size/alignment of UProperty.
    // A candidate is accepted only if adjacent named bools confirm the same
    // layout. Most 32-bit builds use 0x50; a previous +0x58 assumption failed.
    constexpr size_t kDescriptorOffsets[] = {0x50, 0x54, 0x58, 0x5C, 0x60, 0x64};
    int valid_candidates = 0;
    UnderwaterBoolBits chosen{};
    size_t chosen_descriptor = 0;
    for (const size_t descriptor_offset : kDescriptorOffsets) {
        UnderwaterBoolBits bits{};
        if (!UnderwaterReadBoolBits(target, descriptor_offset, bits)) continue;
        UnderwaterBoolBits previous{};
        UnderwaterBoolBits following{};
        const bool prev_valid = before.name_found && before.type_correct &&
            UnderwaterReadBoolBits(before, descriptor_offset, previous) &&
            UnderwaterAdjacentBoolBits(previous, bits);
        const bool next_valid = after.name_found && after.type_correct &&
            UnderwaterReadBoolBits(after, descriptor_offset, following) &&
            UnderwaterAdjacentBoolBits(bits, following);
        if (!prev_valid && !next_valid) continue;
        ++valid_candidates;
        chosen = bits;
        chosen_descriptor = descriptor_offset;
    }
    if (valid_candidates != 1) {
        if (!s_underwater_resolution_warned) {
            std::wstring message = L"Underwater colours: override descriptor unresolved (candidates=" +
                std::to_wstring(valid_candidates) + L", reflected property offset=" +
                std::to_wstring(target.property_offset) + L", neighbours=" +
                std::wstring(before.name_found ? L"previous" : L"no-previous") + L"/" +
                std::wstring(after.name_found ? L"next" : L"no-next") + L")";
            Log(message.c_str());
            // Bounded raw metadata diagnostics help identify any future UE4
            // ABI variant, without writing to arbitrary bytes.
            for (const size_t descriptor_offset : kDescriptorOffsets) {
                if (!IsReadable(target.descriptor, descriptor_offset + 4)) continue;
                const uint8_t* d = target.descriptor + descriptor_offset;
                Log((L"Underwater colours: BoolProperty +" +
                    std::to_wstring(descriptor_offset) + L" = [" +
                    std::to_wstring(d[0]) + L"," + std::to_wstring(d[1]) + L"," +
                    std::to_wstring(d[2]) + L"," + std::to_wstring(d[3]) +
                    L"]").c_str());
            }
        }
        return false;
    }
    offset = chosen.address_offset;
    mask = chosen.mask;
    if (!s_underwater_logged) {
        Log((L"Underwater colours: validated BoolProperty layout +" +
             std::to_wstring(chosen_descriptor) + L", field offset=" +
             std::to_wstring(offset) + L", mask=" +
             std::to_wstring(mask)).c_str());
    }
    return true;
}
bool UnderwaterTargetValid(const GlobalObjectArray* objects) {
    const auto& target = s_underwater_target;
    return objects && target.actor && target.index >= 0 &&
        target.index < objects->num_elements &&
        objects->objects[target.index].object == target.actor &&
        objects->objects[target.index].serial_number == target.serial &&
        WaterIsA(target.actor, L"PostProcessVolume") &&
        (static_cast<uint32_t>(static_cast<UObjectHeader*>(target.actor)->flags)
            & 0x30u) == 0;
}
void UnderwaterRegisterVolume(void* actor, const GlobalObjectArray* objects) {
    if (s_underwater_target.actor || !objects ||
        !IsReadable(actor, sizeof(UObjectHeader)) ||
        !WaterIsA(actor, L"PostProcessVolume")) return;
    const auto* header = static_cast<const UObjectHeader*>(actor);
    const int32_t index = header->index;
    if (index < 0 || index >= objects->num_elements ||
        objects->objects[index].object != actor ||
        (static_cast<uint32_t>(header->flags) & 0x30u) != 0) return;
    s_underwater_target.actor = actor;
    s_underwater_target.index = index;
    s_underwater_target.serial = objects->objects[index].serial_number;
    s_underwater_target.settings_offset = -1;
    s_underwater_target.tint_offset = -1;
    s_underwater_target.override_offset = -1;
    s_underwater_target.override_mask = 0;
    s_underwater_next_apply = 0;
    s_underwater_logged = false;
    Log((L"Underwater colours: found PostWater volume " +
         WaterObjectName(actor)).c_str());
}
void UnderwaterDiscover(const GlobalObjectArray* objects) {
    if (!objects || s_underwater_scan_cursor < 0) return;
    const int32_t begin = s_underwater_scan_cursor;
    const int32_t end = (std::min)(objects->num_elements, begin + kScanBatch);
    ColourWorkBudget budget(700);
    // Cache class classifications; avoid resolving thousands of UObject names
    // on each scan. Class pointers are persistent across level reloads.
    static std::unordered_map<void*, uint8_t> kinds;
    for (; s_underwater_scan_cursor < end; ++s_underwater_scan_cursor) {
        if (budget.expired()) break;
        const auto& item = objects->objects[s_underwater_scan_cursor];
        if (!IsReadable(item.object, sizeof(UObjectHeader))) continue;
        const auto* header = static_cast<const UObjectHeader*>(item.object);
        if ((static_cast<uint32_t>(header->flags) & 0x30u) != 0 &&
            WaterObjectName(header->class_ptr) != L"ScriptStruct") continue;
        uint8_t kind = 0;
        const auto found = kinds.find(header->class_ptr);
        if (found != kinds.end()) kind = found->second;
        else {
            const auto name = WaterObjectName(header->class_ptr);
            kind = name == L"ScriptStruct" ? 1 :
                   name == L"WorldBase_C" ? 2 :
                   name == L"PostProcessVolume" ? 3 : 0;
            if (kinds.size() < 4096) kinds.emplace(header->class_ptr, kind);
        }
        if (kind == 1 && WaterObjectName(item.object) == L"PostProcessSettings") {
            if (!s_underwater_settings_struct) s_underwater_logged = false;
            s_underwater_settings_struct = item.object;
        }
        if (s_underwater_target.actor) continue;
        if (kind == 2) {
            // WorldBase exposes an explicit reference to the underwater volume.
            UnderwaterRegisterVolume(ReadReflectedObject(item.object, L"PostWater"),
                                     objects);
        } else if (kind == 3 && WaterObjectName(item.object) == L"PostWater") {
            UnderwaterRegisterVolume(item.object, objects);
        }
        // No need to scan the rest of the object array once both references
        // have been resolved. Rebind invalidates the volume cache as before.
        if (s_underwater_target.actor && s_underwater_settings_struct) {
            s_underwater_scan_cursor = -1;
            break;
        }
    }
    if (s_underwater_scan_cursor >= objects->num_elements) {
        s_underwater_scan_cursor = -1;
        s_underwater_next_scan = GetTickCount64() + 5000;
        if (!s_underwater_logged && (!s_underwater_target.actor ||
                                     !s_underwater_settings_struct)) {
            s_underwater_logged = true;
            Log((L"Underwater colours: discovery volume=" +
                 std::wstring(s_underwater_target.actor ? L"yes" : L"no") +
                 L", PostProcessSettings struct=" +
                 (s_underwater_settings_struct ? L"yes" : L"no")).c_str());
        }
    }
}
bool UnderwaterResolveTint() {
    auto& target = s_underwater_target;
    if (!target.actor || !s_underwater_settings_struct) return false;
    if (target.settings_offset >= 0 && target.tint_offset >= 0 &&
        target.override_offset >= 0 && target.override_mask) return true;
    const auto* obj = static_cast<const UObjectHeader*>(target.actor);
    const int32_t settings = UnderwaterMemberOffset(
        obj->class_ptr, L"Settings", L"StructProperty");
    const int32_t tint = UnderwaterMemberOffset(
        s_underwater_settings_struct, L"SceneColorTint", L"StructProperty");
    int32_t override_offset = -1;
    uint8_t override_mask = 0;
    const bool has_override = UnderwaterOverrideMember(
        s_underwater_settings_struct, override_offset, override_mask);
    // A reflected tint without its matching override flag is not usable.
    // Fail closed rather than writing an ignored or unrelated field.
    if (settings < 0 || settings > 0x2000 || tint < 0 || tint > 0x1800 ||
        settings + tint > 0x3000 || !has_override ||
        override_offset < 0 || override_offset > 0x1800 ||
        settings + override_offset > 0x3000) {
        if (!s_underwater_resolution_warned) {
            s_underwater_resolution_warned = true;
            Log((L"Underwater colours: reflected offsets unavailable (Settings=" +
                 std::to_wstring(settings) + L", SceneColorTint=" +
                 std::to_wstring(tint) + L", bOverride_SceneColorTint=" +
                 std::to_wstring(override_offset) + L", mask=" +
                 std::to_wstring(override_mask) + L")").c_str());
        }
        return false;
    }
    s_underwater_resolution_warned = false;
    target.settings_offset = settings;
    target.tint_offset = tint;
    target.override_offset = override_offset;
    target.override_mask = override_mask;
    Log((L"Underwater colours: PostWater Settings=" + std::to_wstring(settings) +
         L", SceneColorTint=" + std::to_wstring(tint) +
         L", bOverride_SceneColorTint=" + std::to_wstring(override_offset) +
         L", mask=" + std::to_wstring(override_mask)).c_str());
    return true;
}
// Verify both reflected struct fields are writable, and avoid writing into
// freed, guard, read-only, or uncommitted memory during a level transition.
uint8_t* UnderwaterMemberAddress(int32_t offset, size_t length) {
    if (!s_underwater_target.actor || s_underwater_target.settings_offset < 0 ||
        offset < 0 || offset > 0x1800 || length == 0 || length > 16) return nullptr;
    auto* address = static_cast<uint8_t*>(s_underwater_target.actor) +
        s_underwater_target.settings_offset + offset;
    if (!IsReadable(address, length)) return nullptr;
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(address, &memory, sizeof(memory)) != sizeof(memory) ||
        memory.State != MEM_COMMIT ||
        (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
        reinterpret_cast<uintptr_t>(address) + length >
            reinterpret_cast<uintptr_t>(memory.BaseAddress) + memory.RegionSize)
        return nullptr;
    const DWORD access = memory.Protect & 0xFF;
    if (access != PAGE_READWRITE && access != PAGE_EXECUTE_READWRITE &&
        access != PAGE_WRITECOPY && access != PAGE_EXECUTE_WRITECOPY)
        return nullptr;
    return address;
}
uint8_t* UnderwaterTintAddress() {
    return UnderwaterMemberAddress(s_underwater_target.tint_offset,
                                   sizeof(LinearColor));
}
uint8_t* UnderwaterOverrideAddress() {
    return UnderwaterMemberAddress(s_underwater_target.override_offset, 1);
}
void UnderwaterSetOverride(uint8_t* address, bool enabled) {
    if (!address || !s_underwater_target.override_mask) return;
    const uint8_t mask = s_underwater_target.override_mask;
    *address = static_cast<uint8_t>((*address & static_cast<uint8_t>(~mask)) |
                 (enabled ? mask : 0));
}
bool UnderwaterColoursEqual(const LinearColor& a, const LinearColor& b) {
    return std::fabs(a.r - b.r) < 0.00001f &&
           std::fabs(a.g - b.g) < 0.00001f &&
           std::fabs(a.b - b.b) < 0.00001f &&
           std::fabs(a.a - b.a) < 0.00001f;
}
// Blend in linear RGB, matching UE4's FLinearColor post-process settings.
// When the native override is disabled, the neutral SceneColorTint is white;
// its stored value is ignored until the flag is enabled.
LinearColor UnderwaterBlendTint(const LinearColor& original,
                               bool original_override,
                               const LinearColor& selected, int percent) {
    // Use double for the HDR calculation so even an intensity near INT_MAX
    // cannot overflow intermediate multiplication before conversion to float.
    const double hdr_strength = static_cast<double>((std::max)(0, percent)) / 100.0;
    const float strength = static_cast<float>(hdr_strength);
    const LinearColor base = original_override ? original :
        LinearColor{1.0f, 1.0f, 1.0f, original.a};
    if (strength <= 1.0f) {
        // Preserve the exact existing look for 0–100%.
        return {base.r + (selected.r - base.r) * strength,
                base.g + (selected.g - base.g) * strength,
                base.b + (selected.b - base.b) * strength,
                original.a};
    }
    // HDR strength is intentionally uncapped: preserve the direction of
    // the selected colour and never introduce a fixed 500%/5.0 ceiling.
    // Even at INT_MAX%, double intermediates remain finite for the verified
    // post-process input range, and FLinearColor can represent their result.
    const auto boosted = [hdr_strength](float baseline, float target) -> float {
        const double extrapolated = static_cast<double>(baseline) +
            (static_cast<double>(target) - static_cast<double>(baseline)) * hdr_strength;
        const double hdr = (std::max)(0.0, extrapolated) * hdr_strength;
        return static_cast<float>(hdr);
    };
    return {boosted(base.r, selected.r), boosted(base.g, selected.g),
            boosted(base.b, selected.b), original.a};
}
LinearColor UnderwaterRGB(uint32_t rgb, float alpha) {
    const auto channel = [](uint32_t byte) {
        const float c = static_cast<float>(byte) / 255.0f;
        return c <= 0.04045f ? c / 12.92f :
            std::pow((c + 0.055f) / 1.055f, 2.4f);
    };
    return {channel((rgb >> 16) & 255), channel((rgb >> 8) & 255),
            channel(rgb & 255), alpha};
}
void UnderwaterApplyTint(const GlobalObjectArray* objects) {
    auto& target = s_underwater_target;
    if (!UnderwaterTargetValid(objects)) return;
    if (!UnderwaterResolveTint()) {
        // Reflection metadata is effectively static per game build. Retry
        // conservatively rather than traversing every UProperty each tick.
        s_underwater_next_apply = GetTickCount64() + 3000;
        return;
    }
    auto* location = UnderwaterTintAddress();
    auto* override_location = UnderwaterOverrideAddress();
    if (!location || !override_location) return;
    LinearColor current{};
    std::memcpy(&current, location, sizeof(current));
    if (!std::isfinite(current.r) || !std::isfinite(current.g) ||
        !std::isfinite(current.b) || !std::isfinite(current.a)) return;
    // Reject implausible *foreign* memory values, but don't block the high
    // HDR tint that this module wrote itself. Otherwise >100% could get
    // stuck and Game Default would no longer be able to restore it.
    const bool our_last_tint = target.applied &&
        UnderwaterColoursEqual(current, target.last_written);
    if (!our_last_tint &&
        (std::fabs(current.r) > 100.0f ||
         std::fabs(current.g) > 100.0f ||
         std::fabs(current.b) > 100.0f)) return;
    const bool override_enabled = (*override_location & target.override_mask) != 0;
    const auto& config = WaterConfig();
    const bool custom_effective = config.underwater_custom &&
        config.underwater_intensity_percent > 0;
    const bool tint_effective = WaterNoTint() || custom_effective;
    if (!tint_effective) {
        if (target.applied) {
            // Restore only the tint we last wrote. Leave newer game-originated
            // tint changes alone, but always restore the bit we enabled.
            if (UnderwaterColoursEqual(current, target.last_written))
                std::memcpy(location, &target.original, sizeof(target.original));
            if (override_enabled)
                UnderwaterSetOverride(override_location, target.original_override);
        }
        target.applied = false;
        s_underwater_restore_pending = false;
        return;
    }
    // Preserve both original values before enabling our own override. If the
    // game changes the tint or disables the override, track its latest state.
    if (!target.applied || !UnderwaterColoursEqual(current, target.last_written) ||
        !override_enabled) {
        target.original = current;
        target.original_override = override_enabled;
    }
    // Removal has priority over a saved custom colour, without erasing that
    // colour. White SceneColorTint is neutral in linear RGB.
    const LinearColor selected = WaterNoTint()
        ? LinearColor{1.0f, 1.0f, 1.0f, target.original.a}
        : UnderwaterRGB(config.underwater_rgb, target.original.a);
    const LinearColor new_colour = WaterNoTint() ? selected : UnderwaterBlendTint(
        target.original, target.original_override, selected,
        config.underwater_intensity_percent);
    if (!UnderwaterColoursEqual(current, new_colour))
        std::memcpy(location, &new_colour, sizeof(new_colour));
    if (!override_enabled) UnderwaterSetOverride(override_location, true);
    target.last_written = new_colour;
    target.applied = true;
    if (!s_underwater_logged) {
        s_underwater_logged = true;
        Log((L"Underwater colours: SceneColorTint and bOverride_SceneColorTint "
             L"enabled on PostWater (original override=" +
             std::wstring(target.original_override ? L"on" : L"off") + L")").c_str());
    }
}
// Restore original colour and its matching override flag before dropping an
// actor reference when a practice/restart transition recreates the player.
void UnderwaterRestoreRaisedEffect(const GlobalObjectArray* objects);
void UnderwaterRestoreBeforeRebind() {
    UnderwaterRestoreRaisedEffect(WaterObjects());
    if (!s_underwater_target.applied ||
        !UnderwaterTargetValid(WaterObjects())) return;
    uint8_t* address = UnderwaterTintAddress();
    uint8_t* override_location = UnderwaterOverrideAddress();
    if (!address || !override_location) return;
    LinearColor current{};
    std::memcpy(&current, address, sizeof(current));
    if (UnderwaterColoursEqual(current, s_underwater_target.last_written))
        std::memcpy(address, &s_underwater_target.original,
                    sizeof(s_underwater_target.original));
    if ((*override_location & s_underwater_target.override_mask) != 0)
        UnderwaterSetOverride(override_location,
                              s_underwater_target.original_override);
}
// Forward declarations: these are defined with the native water-height code.
bool RaisedWaterOriginalSurface(const GlobalObjectArray* objects, float& surface_z);
void UnderwaterUpdateRaisedEffect(MyCharacter* player,
                                 const GlobalObjectArray* objects);

void UpdateUnderwaterColours(MyCharacter* player) {
    const auto& config = WaterConfig();
    const bool custom_effective = config.underwater_custom &&
        config.underwater_intensity_percent > 0;
    const bool tint_effective = WaterNoTint() || custom_effective;
    const bool raised_effect_needed = config.height_offset > 0 &&
        !WaterNoSwimming() && !WaterNoTint();
    if (!tint_effective && !raised_effect_needed &&
        !s_underwater_restore_pending && !s_underwater_target.applied &&
        !s_underwater_target.raised_effect_owned) return;
    if (s_underwater_player != player) {
        UnderwaterRestoreBeforeRebind();
        s_underwater_player = player;
        s_underwater_target = {};
        s_underwater_scan_cursor = 0;
        s_underwater_next_scan = 0;
        s_underwater_next_apply = 0;
        s_underwater_logged = false;
        s_underwater_resolution_warned = false;
        s_raised_postprocess_warning_logged = false;
        s_raised_postprocess_logged = false;
    }
    const ULONGLONG now = GetTickCount64();
    if (now < s_underwater_next_apply) return;
    // Discover aggressively across frames, but keep a bounded scan budget.
    // The normal 150ms verification rate is retained after discovery.
    s_underwater_next_apply = now + (s_underwater_scan_cursor >= 0 ? 16 :
                                     (raised_effect_needed ? 30 : 150));
    const auto* objects = WaterObjects();
    if (!objects) return;
    if (s_underwater_target.actor && !UnderwaterTargetValid(objects)) {
        s_underwater_target = {};
        s_underwater_scan_cursor = 0;
        s_underwater_next_scan = 0;
        s_underwater_logged = false;
    }
    if (s_underwater_scan_cursor < 0 && now >= s_underwater_next_scan &&
        (tint_effective || raised_effect_needed || s_underwater_restore_pending) &&
        (!s_underwater_target.actor || !s_underwater_settings_struct))
        s_underwater_scan_cursor = 0;
    if (s_underwater_scan_cursor >= 0) UnderwaterDiscover(objects);
    if (s_underwater_target.actor && s_underwater_settings_struct)
        UnderwaterApplyTint(objects);
    UnderwaterUpdateRaisedEffect(player, objects);
    if (!tint_effective && !raised_effect_needed &&
        !s_underwater_target.applied &&
        !s_underwater_target.raised_effect_owned) {
        s_underwater_restore_pending = false;
        s_underwater_target = {};
        s_underwater_scan_cursor = -1;
    }
}

// Only the reflected APhysicsVolume::bWaterVolume bit is changed. The
// adjacent bPhysicsOnContact property independently confirms the native
// UBoolProperty descriptor layout before either byte is writable.
bool SwimResolveWaterFlag(void* actor, int32_t& offset, uint8_t& mask) {
    offset = -1;
    mask = 0;
    if (!IsReadable(actor, sizeof(UObjectHeader))) return false;
    void* klass = static_cast<UObjectHeader*>(actor)->class_ptr;
    const auto water = UnderwaterFindBoolField(klass, L"bWaterVolume");
    const auto contact = UnderwaterFindBoolField(klass, L"bPhysicsOnContact");
    if (!water.type_correct || !contact.type_correct) return false;
    constexpr size_t kDescriptors[] = {0x50, 0x54, 0x58, 0x5C, 0x60, 0x64};
    UnderwaterBoolBits resolved{};
    int matches = 0;
    for (const auto descriptor : kDescriptors) {
        UnderwaterBoolBits w{}, c{};
        if (!UnderwaterReadBoolBits(water, descriptor, w) ||
            !UnderwaterReadBoolBits(contact, descriptor, c) ||
            !(UnderwaterAdjacentBoolBits(w, c) ||
              UnderwaterAdjacentBoolBits(c, w))) continue;
        resolved = w;
        ++matches;
    }
    if (matches != 1 || resolved.address_offset < 0 ||
        resolved.address_offset > 0x1000) return false;
    offset = resolved.address_offset;
    mask = resolved.mask;
    return true;
}
bool SwimVolumeValid(const SwimVolumeTarget& target,
                     const GlobalObjectArray* objects) {
    if (!objects || !target.actor || target.index < 0 ||
        target.index >= objects->num_elements ||
        objects->objects[target.index].object != target.actor ||
        objects->objects[target.index].serial_number != target.serial ||
        !IsReadable(target.actor, sizeof(UObjectHeader))) return false;
    const auto* object = static_cast<const UObjectHeader*>(target.actor);
    return (static_cast<uint32_t>(object->flags) & 0x30u) == 0 &&
           object->index == target.index && WaterIsA(target.actor, L"PhysicsVolume");
}
uint8_t* SwimFlagAddress(const SwimVolumeTarget& target,
                         const GlobalObjectArray* objects) {
    if (!SwimVolumeValid(target, objects) || target.flag_offset < 0 ||
        target.flag_offset > 0x1000 || !target.flag_mask) return nullptr;
    auto* address = static_cast<uint8_t*>(target.actor) + target.flag_offset;
    return IsWritable(address, 1) ? address : nullptr;
}
// Restore only bits the utility actually cleared, and never touch a UObject
// after its global-object index/serial has changed during a map reload.
void SwimRestoreVolumes() {
    const auto* objects = WaterObjects();
    // Object-array access may be briefly unavailable during OpenLevel. Keep
    // the restore request; retry when native objects are visible again.
    if (!objects) return;
    for (auto it = s_swim_volumes.begin(); it != s_swim_volumes.end();) {
        if (!SwimVolumeValid(*it, objects)) {
            it = s_swim_volumes.erase(it);
            continue;
        }
        uint8_t* address = SwimFlagAddress(*it, objects);
        if (!address) { ++it; continue; } // Retry a temporary write failure.
        *address = static_cast<uint8_t>(*address | it->flag_mask);
        if ((*address & it->flag_mask) == 0) { ++it; continue; }
        it = s_swim_volumes.erase(it);
    }
}
// The target must be a live PhysicsVolume and the water bit must be validated
// against a second reflected property. Never patch an arbitrary actor field.
bool SwimDisableVolume(void* actor, const GlobalObjectArray* objects) {
    if (!actor || !objects || !IsReadable(actor, sizeof(UObjectHeader))) return false;
    const auto* object = static_cast<const UObjectHeader*>(actor);
    const int index = object->index;
    if (index < 0 || index >= objects->num_elements ||
        objects->objects[index].object != actor ||
        (static_cast<uint32_t>(object->flags) & 0x30u) != 0 ||
        !WaterIsA(actor, L"PhysicsVolume")) return false;
    const auto active = std::find_if(s_swim_volumes.begin(), s_swim_volumes.end(),
        [actor](const SwimVolumeTarget& item) { return item.actor == actor; });
    if (active != s_swim_volumes.end()) {
        uint8_t* byte = SwimFlagAddress(*active, objects);
        if (!byte) return false;
        *byte = static_cast<uint8_t>(*byte & ~active->flag_mask);
        return (*byte & active->flag_mask) == 0;
    }
    int offset = -1;
    uint8_t mask = 0;
    if (!SwimResolveWaterFlag(actor, offset, mask)) {
        if (!s_swim_resolution_warned) {
            s_swim_resolution_warned = true;
            Log(L"Water gameplay: cannot validate bWaterVolume BoolProperty; swimming override skipped");
            Notify(L"Water physics override unavailable; see RefunctUtility.log");
        }
        return false;
    }
    SwimVolumeTarget target{actor, index,
                            objects->objects[index].serial_number, offset, mask};
    uint8_t* byte = SwimFlagAddress(target, objects);
    if (!byte) return false;
    if ((*byte & mask) == 0) return true; // A non-water volume: leave it alone.
    if (s_swim_volumes.size() >= kMaxSwimVolumes) return false;
    *byte = static_cast<uint8_t>(*byte & ~mask);
    if ((*byte & mask) != 0) return false;
    const bool first_volume = s_swim_volumes.empty();
    s_swim_volumes.push_back(target);
    if (first_volume)
        Log(L"Water gameplay: native PhysicsVolume water flag disabled (collision-safe override)");
    return true;
}
// The currently active volume can be discovered immediately on entering
// water, without waiting for the bounded global-object scan to finish.
// A serial-checked native handle is mandatory before reading a reflected
// component or calling a UE function. PhysicsVolume roots are usually
// BrushComponents, whose SceneComponent location is movable through UE4's
// K2_SetWorldLocation when the cooked actor permits runtime movement.
bool WaterHeightVolumeValid(const WaterHeightVolumeTarget& target,
                            const GlobalObjectArray* objects) {
    if (!objects || target.actor_index < 0 || target.root_index < 0 ||
        target.actor_index >= objects->num_elements ||
        target.root_index >= objects->num_elements ||
        objects->objects[target.actor_index].object != target.actor ||
        objects->objects[target.actor_index].serial_number != target.actor_serial ||
        objects->objects[target.root_index].object != target.root ||
        objects->objects[target.root_index].serial_number != target.root_serial ||
        !IsReadable(target.actor, sizeof(UObjectHeader)) ||
        !IsReadable(target.root, sizeof(UObjectHeader)) ||
        !WaterIsA(target.actor, L"PhysicsVolume") ||
        !WaterIsA(target.root, L"SceneComponent")) return false;
    const auto* actor = static_cast<const UObjectHeader*>(target.actor);
    const auto* root = static_cast<const UObjectHeader*>(target.root);
    return actor->index == target.actor_index && root->index == target.root_index &&
        (static_cast<uint32_t>(actor->flags) & 0x30u) == 0 &&
        (static_cast<uint32_t>(root->flags) & 0x30u) == 0 &&
        ReadReflectedObject(target.actor, L"RootComponent") == target.root;
}
// A water bit cleared by our existing no-swim feature is still an originally
// native water volume. Do not lose it when master Remove Water is toggled on.
bool WaterHeightIsNativeWater(void* actor, const GlobalObjectArray* objects) {
    // The raised-water fallback temporarily marks DefaultPhysicsVolume as
    // water. That must never make the *default atmosphere* eligible for brush
    // relocation during a later discovery pass.
    if (WaterIsA(actor, L"DefaultPhysicsVolume")) return false;
    for (const auto& entry : s_swim_volumes)
        if (entry.actor == actor && SwimVolumeValid(entry, objects)) return true;
    int32_t offset = -1;
    uint8_t mask = 0;
    if (!SwimResolveWaterFlag(actor, offset, mask) || offset < 0) return false;
    const auto* address = static_cast<const uint8_t*>(actor) + offset;
    return IsReadable(address, 1) && (*address & mask) != 0;
}
// Native collision expansion: keep the brush's bottom at its original world
// height while extending its *top* to match the raised rendered ocean.
// Static UE4 brush components reject SetWorldLocation; the Blueprint-callable
// SetMobility API re-registers the component as Movable. All property layouts
// and native return values must verify before we modify a cooked brush.
bool WaterHeightReadMobility(void* root, uint8_t& mobility) {
    if (!IsReadable(root, sizeof(UObjectHeader))) return false;
    void* klass = static_cast<const UObjectHeader*>(root)->class_ptr;
    for (int depth = 0; depth < 16 && IsReadable(klass, sizeof(UStructHeader)); ++depth) {
        const auto* cls = static_cast<const UStructHeader*>(klass);
        void* field = cls->children;
        for (int count = 0; count < 4096 &&
             IsReadable(field, sizeof(UFieldHeader)); ++count) {
            const auto* f = static_cast<const UFieldHeader*>(field);
            if (UnrealName(f->object.name) == L"Mobility") {
                const std::wstring type = WaterObjectName(f->object.class_ptr);
                if (type != L"ByteProperty" && type != L"EnumProperty") return false;
                const int32_t offset = FindClassPropertyOffset(klass, L"Mobility");
                if (offset < 0 || offset > 0x1000) return false;
                const auto* ptr = static_cast<const uint8_t*>(root) + offset;
                if (!IsReadable(ptr, 1) || *ptr > 2) return false;
                mobility = *ptr;
                return true;
            }
            field = f->next;
        }
        klass = cls->super_struct;
    }
    return false;
}
bool WaterHeightSetMobility(void* root, uint8_t mobility) {
    if (!g_process_event || mobility > 2 ||
        !IsReadable(root, sizeof(UObjectHeader))) return false;
    void* fn = FindReflectedFunction(root, L"SetMobility");
    int32_t offset = WaterParam(fn, L"NewMobility", L"ByteProperty");
    if (offset < 0) offset = WaterParam(fn, L"NewMobility", L"EnumProperty");
    if (offset < 0 || offset >= 128) return false;
    alignas(16) std::array<uint8_t, 128> args{};
    args[offset] = mobility; // EComponentMobility: Static=0, Stationary=1, Movable=2.
    g_process_event(root, fn, args.data());
    uint8_t actual = 0xFF;
    return WaterHeightReadMobility(root, actual) && actual == mobility;
}
bool WaterHeightGetScale(void* root, Vec3& scale) {
    if (!g_process_event) return false;
    void* fn = FindReflectedFunction(root, L"K2_GetComponentScale");
    if (!fn) fn = FindReflectedFunction(root, L"GetComponentScale");
    int32_t result = WaterParam(fn, L"ReturnValue", L"StructProperty");
    if (result < 0) return false;
    alignas(16) std::array<uint8_t, 128> args{};
    g_process_event(root, fn, args.data());
    std::memcpy(&scale, args.data() + result, sizeof(scale));
    return std::isfinite(scale.x) && std::isfinite(scale.y) &&
           std::isfinite(scale.z) && std::fabs(scale.z) > 0.001f;
}
bool WaterHeightSetScale(void* root, const Vec3& scale) {
    if (!g_process_event || !std::isfinite(scale.z)) return false;
    void* fn = FindReflectedFunction(root, L"SetWorldScale3D");
    int32_t arg = WaterParam(fn, L"NewScale", L"StructProperty");
    if (arg < 0) arg = WaterParam(fn, L"NewScale3D", L"StructProperty");
    if (arg < 0) return false;
    alignas(16) std::array<uint8_t, 128> args{};
    std::memcpy(args.data() + arg, &scale, sizeof(scale));
    g_process_event(root, fn, args.data());
    Vec3 actual{};
    return WaterHeightGetScale(root, actual) &&
        std::fabs(actual.x - scale.x) < 0.01f &&
        std::fabs(actual.y - scale.y) < 0.01f &&
        std::fabs(actual.z - scale.z) < 0.01f;
}
bool WaterHeightGetBounds(void* actor, float& bottom, float& height) {
    if (!g_process_event) return false;
    void* fn = FindReflectedFunction(actor, L"GetActorBounds");
    const int32_t origin_offset = WaterParam(fn, L"Origin", L"StructProperty");
    const int32_t extent_offset = WaterParam(fn, L"BoxExtent", L"StructProperty");
    if (origin_offset < 0 || extent_offset < 0) return false;
    alignas(16) std::array<uint8_t, 256> args{};
    g_process_event(actor, fn, args.data());
    Vec3 origin{}, extent{};
    std::memcpy(&origin, args.data() + origin_offset, sizeof(origin));
    std::memcpy(&extent, args.data() + extent_offset, sizeof(extent));
    height = extent.z * 2.0f;
    bottom = origin.z - extent.z;
    return std::isfinite(bottom) && std::isfinite(height) &&
           height > 50.0f && height < 200000.0f;
}
// On failure we retain original transforms/mobility for a later reset retry.
// Never restore the original mobility while the brush is still displaced.
bool WaterHeightRestoreNative(WaterHeightVolumeTarget& target) {
    Vec3 now{};
    if (!WaterGetLocation(target.root, now)) return false;
    if (std::fabs(now.z - target.original_location.z) > 0.5f ||
        std::fabs(now.x - target.original_location.x) > 0.5f ||
        std::fabs(now.y - target.original_location.y) > 0.5f) {
        if (!WaterSetLocation(target.root, target.original_location)) return false;
    }
    if (target.scale_modified) {
        // Only clear ownership after *both* native transformations verify.
        if (!WaterHeightSetScale(target.root, target.original_scale)) return false;
        target.scale_modified = false;
    }
    if (target.mobility_owned) {
        if (!WaterHeightSetMobility(target.root, target.original_mobility)) return false;
        target.mobility_owned = false;
    }
    target.height_modified = false;
    return true;
}
void WaterHeightApplyVolume(WaterHeightVolumeTarget& target,
                            const GlobalObjectArray* objects, int height) {
    if (!WaterHeightVolumeValid(target, objects) ||
        GetTickCount64() < target.retry_after) return;
    // Restore the original brush, including its original mobility.
    if (height == 0) {
        if (target.height_modified || target.scale_modified || target.mobility_owned) {
            if (!WaterHeightRestoreNative(target))
                target.retry_after = GetTickCount64() + 2000;
        }
        return;
    }
    if (!target.geometry_known || target.baseline_height < 50.0f) return;
    const float factor = 1.0f + static_cast<float>(height) / target.baseline_height;
    if (!std::isfinite(factor) || factor < 0.05f || factor > 20.0f) return;
    // Scaling around the root pivot changes the bottom of the brush unless
    // we translate the pivot by (bottom - pivot) * (1 - factor).
    const Vec3 desired_scale{target.original_scale.x, target.original_scale.y,
                              target.original_scale.z * factor};
    Vec3 desired = target.original_location;
    desired.z += (target.baseline_bottom_z - target.original_location.z) * (1.0f - factor);
    if (!std::isfinite(desired.z)) return;
    uint8_t mobility = 0xFF;
    if (!WaterHeightReadMobility(target.root, mobility)) return;
    if (mobility == 2 && target.height_modified) {
        Vec3 actual_location{}, actual_scale{};
        if (WaterGetLocation(target.root, actual_location) &&
            WaterHeightGetScale(target.root, actual_scale) &&
            std::fabs(actual_location.z - desired.z) < 0.5f &&
            std::fabs(actual_scale.z - desired_scale.z) < 0.01f) {
            // SetWaterHeightOffset resets the global success flag. Reaffirm
            // verified geometry even if this value was already applied.
            s_height_volume_success_logged = true;
            return; // No native re-registration or physics mutation each tick.
        }
    }
    if (mobility != 2) {
        // SetMobility re-registers the collision component using UE's normal
        // engine path. A raw UObject Mobility byte write would be unsafe.
        // A reflected setter can change the underlying native value yet
        // fail readback. Track ownership before invoking it for safe retry.
        target.mobility_owned = true;
        if (!WaterHeightSetMobility(target.root, 2)) {
            target.retry_after = GetTickCount64() + 2000;
            WaterHeightRestoreNative(target);
            if (!s_height_volume_warning_logged) {
                s_height_volume_warning_logged = true;
                Log(L"Water height: native brush SetMobility(Movable) unavailable or rejected; keeping original collision");
            }
            return;
        }
        target.mobility_owned = true;
    }
    // Save recovery ownership *before* writing native geometry. A setter may
    // modify the world and still return a failed verification result.
    target.height_modified = true;
    target.scale_modified = true;
    const bool scale_ok = WaterHeightSetScale(target.root, desired_scale);
    const bool location_ok = scale_ok && WaterSetLocation(target.root, desired);
    float actual_bottom = 0.0f, actual_height = 0.0f;
    const bool bounds_ok = location_ok &&
        WaterHeightGetBounds(target.actor, actual_bottom, actual_height) &&
        std::fabs(actual_bottom - target.baseline_bottom_z) < 10.0f &&
        std::fabs(actual_height - (target.baseline_height +
                                  static_cast<float>(height))) < 10.0f;
    if (!scale_ok || !location_ok || !bounds_ok) {
        target.retry_after = GetTickCount64() + 2000;
        WaterHeightRestoreNative(target); // Best effort; preserve ownership for retry.
        if (!s_height_volume_warning_logged) {
            s_height_volume_warning_logged = true;
            Log(L"Water height: native water brush expansion or collision bounds verification failed; restoring original geometry");
            Notify(L"Water physics height unavailable; see RefunctUtility.log");
        }
        return;
    }
    if (!s_height_volume_success_logged) {
        s_height_volume_success_logged = true;
        Log((L"Water height: native water brush expanded upward by " +
            std::to_wstring(height) + L" uu with movable collision; check native volume overlap in-game").c_str());
    }
}

void WaterHeightDiscoverAndApply(const GlobalObjectArray* objects, int height) {
    const ULONGLONG started = GetTickCount64();
    size_t examined = 0;
    size_t applied = 0;
    // Resolve the class hierarchy once per UE class, not once per UObject.
    static std::unordered_map<void*, bool> physics_class_cache;
    while (s_height_volume_scan_cursor < objects->num_elements &&
           examined++ < kScanBatch &&
           GetTickCount64() - started < kScanCpuBudgetMs) {
        const int32_t index = s_height_volume_scan_cursor++;
        const auto& item = objects->objects[index];
        if (!item.object || !IsReadable(item.object, sizeof(UObjectHeader))) continue;
        const auto* obj = item.object;
        if ((static_cast<uint32_t>(obj->flags) & 0x30u) != 0 ||
            obj->index != index) continue;
        const auto cached = physics_class_cache.find(obj->class_ptr);
        bool is_physics = false;
        if (cached != physics_class_cache.end()) is_physics = cached->second;
        else {
            is_physics = WaterIsA(item.object, L"PhysicsVolume");
            if (physics_class_cache.size() < 4096)
                physics_class_cache.emplace(obj->class_ptr, is_physics);
        }
        if (!is_physics) continue;
        if (std::any_of(s_height_volumes.begin(), s_height_volumes.end(),
            [&](const WaterHeightVolumeTarget& t) { return t.actor == item.object; })) continue;
        if (!WaterHeightIsNativeWater(item.object, objects)) continue;
        void* root = ReadReflectedObject(item.object, L"RootComponent");
        if (!IsReadable(root, sizeof(UObjectHeader)) ||
            !WaterIsA(root, L"BrushComponent")) continue;
        const auto* root_obj = static_cast<const UObjectHeader*>(root);
        const int32_t root_index = root_obj->index;
        if (root_index < 0 || root_index >= objects->num_elements ||
            objects->objects[root_index].object != root ||
            (static_cast<uint32_t>(root_obj->flags) & 0x30u) != 0) continue;
        Vec3 original{};
        if (!WaterGetLocation(root, original)) continue;
        if (s_height_volumes.size() >= kMaxHeightVolumes) break;
        WaterHeightVolumeTarget target{};
        target.actor = item.object;
        target.actor_index = index;
        target.actor_serial = item.serial_number;
        target.root = root;
        target.root_index = root_index;
        target.root_serial = objects->objects[root_index].serial_number;
        target.original_location = original;
        // Cannot safely infer scale or collision bounds from the brush pivot.
        target.geometry_known = WaterHeightGetScale(root, target.original_scale) &&
            WaterHeightGetBounds(item.object, target.baseline_bottom_z,
                                 target.baseline_height) &&
            WaterHeightReadMobility(root, target.original_mobility);
        if (!target.geometry_known) {
            if (!s_height_volume_warning_logged) {
                s_height_volume_warning_logged = true;
                Log(L"Water height: native brush mobility, scale or collision bounds could not be verified");
            }
            continue;
        }
        s_height_volumes.push_back(target);
        if (applied++ < kHeightApplyBatch)
            WaterHeightApplyVolume(s_height_volumes.back(), objects, height);
    }
    if (s_height_volume_scan_cursor >= objects->num_elements ||
        s_height_volumes.size() >= kMaxHeightVolumes) {
        s_height_volume_scan_cursor = -1;
        s_height_volume_next_scan = GetTickCount64() + 5000;
        if (s_height_volumes.empty() && !s_height_volume_warning_logged) {
            s_height_volume_warning_logged = true;
            Log(L"Water height: could not discover a movable native water PhysicsVolume; only ocean mesh height changed");
        }
    }
}
void UpdateWaterPhysicsHeight() {
    const int height = WaterConfig().height_offset;
    if (height == 0 && s_height_volumes.empty()) return;
    if (g_restart_requested || g_full_level_reload_requested) return;
    const ULONGLONG tick = GetTickCount64();
    if (tick < s_height_volume_next_update) return;
    s_height_volume_next_update = tick + 80; // Avoid expensive reflection every frame.
    const auto* objects = WaterObjects();
    if (!objects) return;
    // Stale actor/brush handles must never be reused after OpenLevel.
    s_height_volumes.erase(std::remove_if(s_height_volumes.begin(), s_height_volumes.end(),
        [objects](const WaterHeightVolumeTarget& t) {
            return !WaterHeightVolumeValid(t, objects);
        }), s_height_volumes.end());
    const size_t target_count = s_height_volumes.size();
    for (size_t attempted = 0;
         attempted < (std::min)(kHeightApplyBatch, target_count); ++attempted) {
        if (s_height_volume_apply_cursor >= target_count)
            s_height_volume_apply_cursor = 0;
        WaterHeightApplyVolume(s_height_volumes[s_height_volume_apply_cursor++],
                               objects, height);
    }
    if (height == 0) {
        s_height_volumes.erase(std::remove_if(s_height_volumes.begin(),
            s_height_volumes.end(), [](const WaterHeightVolumeTarget& t) {
                return !t.height_modified && !t.scale_modified && !t.mobility_owned;
            }), s_height_volumes.end());
        // An unsuccessful restoration stays tracked and is retried.
        if (s_height_volumes.empty()) {
            s_height_volume_scan_cursor = -1;
            s_height_volume_apply_cursor = 0;
        }
        return;
    }
    const ULONGLONG now = GetTickCount64();
    if (s_height_volume_scan_cursor < 0 && now >= s_height_volume_next_scan)
        s_height_volume_scan_cursor = 0;
    if (s_height_volume_scan_cursor >= 0)
        WaterHeightDiscoverAndApply(objects, height);
}
void* SwimCurrentPhysicsVolume(MyCharacter* player) {
    if (!player || !g_process_event) return nullptr;
    for (void* object : {static_cast<void*>(player->movement),
                         static_cast<void*>(player->root_component)}) {
        if (!IsReadable(object, sizeof(UObjectHeader))) continue;
        void* fn = FindReflectedFunction(object, L"GetPhysicsVolume");
        const int result = WaterParam(fn, L"ReturnValue", L"ObjectProperty");
        if (result < 0) continue;
        alignas(16) std::array<uint8_t, 128> args{};
        g_process_event(object, fn, args.data());
        void* volume = nullptr;
        std::memcpy(&volume, args.data() + result, sizeof(volume));
        if (volume) return volume;
    }
    return nullptr;
}
// Uses the engine's reflected setter, preserving its movement-mode callbacks.
// A byte ABI write is intentionally avoided. The validated function parameter
// is the same one already used by Remove Water Gravity / Swimming.
bool RaisedWaterSetMovementMode(MyCharacter* player, uint8_t mode) {
    if (!player || !g_process_event ||
        !IsReadable(player->movement, sizeof(MovementComponent))) return false;
    void* fn = FindReflectedFunction(player->movement, L"SetMovementMode");
    const int32_t offset = WaterParam(fn, L"NewMovementMode", L"ByteProperty");
    if (offset < 0 || offset >= 128) return false;
    alignas(16) std::array<uint8_t, 128> args{};
    args[offset] = mode;
    g_process_event(player->movement, fn, args.data());
    return player->movement->movement_mode == mode;
}

void RaisedWaterRestoreVirtualVolume(MyCharacter* player,
                                     const GlobalObjectArray* objects,
                                     bool preserve_swimming = false) {
    if (!s_raised_water.active) return;
    const auto old = s_raised_water;
    // Do not erase ownership when the volume is live but its memory is only
    // temporarily unwritable. It must be restored on a subsequent game tick.
    if (SwimVolumeValid(old.volume, objects)) {
        uint8_t* byte = SwimFlagAddress(old.volume, objects);
        if (!byte) return;
        *byte = static_cast<uint8_t>(*byte & ~old.volume.flag_mask);
        if ((*byte & old.volume.flag_mask) != 0) return;
    }
    s_raised_water = {};
    // Only undo swimming we introduced, and only if the character still uses
    // this now-restored default volume. Never interfere with native water.
    if (!preserve_swimming && old.owns_swimming_mode && player &&
        IsReadable(player, sizeof(MyCharacter)) &&
        IsReadable(player->movement, sizeof(MovementComponent)) &&
        player->movement->movement_mode == 4 &&
        SwimCurrentPhysicsVolume(player) == old.volume.actor)
        RaisedWaterSetMovementMode(player, 3); // MOVE_Falling
}

// Return the baseline Z of the *verified ocean mesh*. We cannot infer an
// accurate waterline from the static brush's origin (often its centre).
bool RaisedWaterOriginalSurface(const GlobalObjectArray* objects, float& surface_z) {
    for (const auto& target : s_targets) {
        if (!WaterIsTargetValid(target, objects) ||
            !target.original_location_known || !target.height_modified) continue;
        surface_z = target.original_location.z;
        return std::isfinite(surface_z);
    }
    return false;
}

// A game-level waterline is independent of UE's brush transform. Try this
// first so the Refunct-specific underwater tests and the native movement
// fallback agree on the desired surface. The pak only proves the name exists:
// runtime metadata must independently prove the field is a live float.
// A native water-volume switch is safe only if the destination volume is
// already water before CharacterMovement handles PhysicsVolumeChanged.
// Find that destination by previously verified UObject identity or by a
// bounded scan of actors in the *same ULevel* as the local character.
// Fail closed if the current world's default volume cannot be identified.
void* RaisedWaterDefaultVolume(MyCharacter* player,
                               const GlobalObjectArray* objects,
                               bool currently_in_native_water) {
    if (!player || !objects || !objects->objects || objects->num_elements < 1)
        return nullptr;
    if (SwimVolumeValid(s_raised_default_hint, objects) &&
        WaterIsA(s_raised_default_hint.actor, L"DefaultPhysicsVolume"))
        return s_raised_default_hint.actor;
    s_raised_default_hint = {};
    if (!currently_in_native_water) return nullptr;
    if (!IsReadable(player, sizeof(UObjectHeader))) return nullptr;
    const void* player_outer = static_cast<const UObjectHeader*>(static_cast<void*>(player))->outer;
    if (!player_outer) return nullptr;
    const ULONGLONG started = GetTickCount64();
    int processed = 0;
    while (s_raised_default_scan_cursor < objects->num_elements &&
           processed < 2048 && GetTickCount64() - started < 2) {
        const int32_t index = s_raised_default_scan_cursor++;
        ++processed;
        const auto& item = objects->objects[index];
        if (!IsReadable(item.object, sizeof(UObjectHeader))) continue;
        const auto* actor = item.object;
        if (actor->index != index || (static_cast<uint32_t>(actor->flags) & 0x30u) ||
            actor->outer != player_outer) continue;
        // Class check is expensive; only examine objects with a matching name.
        if (WaterObjectName(item.object).find(L"DefaultPhysicsVolume") != 0 ||
            !WaterIsA(item.object, L"DefaultPhysicsVolume")) continue;
        int32_t offset = -1;
        uint8_t mask = 0;
        if (!SwimResolveWaterFlag(item.object, offset, mask)) continue;
        s_raised_default_hint = {item.object, index, item.serial_number, offset, mask};
        return item.object;
    }
    if (s_raised_default_scan_cursor >= objects->num_elements) {
        s_raised_default_scan_cursor = 0;
        if (!s_raised_prearm_lookup_warned) {
            s_raised_prearm_lookup_warned = true;
            Log(L"Water height: native-exit prearm unavailable; no same-level DefaultPhysicsVolume verified");
        }
    }
    return nullptr;
}

int32_t FindNativeFixedWaterlineOffset(MyCharacter* player) {
    if (!IsReadable(player, sizeof(UObjectHeader))) return -1;
    void* klass = static_cast<const UObjectHeader*>(static_cast<void*>(player))->class_ptr;
    for (int depth = 0; depth < 16 && IsReadable(klass, sizeof(UStructHeader)); ++depth) {
        void* field = static_cast<const UStructHeader*>(klass)->children;
        for (int count = 0; count < 4096 && IsReadable(field, sizeof(UFieldHeader)); ++count) {
            const auto* header = static_cast<const UFieldHeader*>(field);
            if (WaterObjectName(field) == L"FixedWaterHeight") {
                if (WaterObjectName(header->object.class_ptr) != L"FloatProperty" ||
                    !IsReadable(field, 0x40)) return -1;
                int32_t offset = -1;
                std::memcpy(&offset, static_cast<const uint8_t*>(field) + 0x3C,
                            sizeof(offset));
                // A property may belong to the native or Blueprint subclass.
                return offset >= 0x20 && offset <= 0x4000 - sizeof(float)
                    ? offset : -1;
            }
            field = header->next;
        }
        klass = static_cast<const UStructHeader*>(klass)->super_struct;
    }
    return -1;
}
bool FixedWaterlineIsLive(MyCharacter* player,
                          const GlobalObjectArray* objects) {
    const auto& target = s_fixed_waterline;
    if (!objects || !player || target.player != player ||
        target.index < 0 || target.index >= objects->num_elements ||
        objects->objects[target.index].object != static_cast<void*>(player) ||
        objects->objects[target.index].serial_number != target.serial ||
        !IsReadable(player, sizeof(MyCharacter))) return false;
    const auto* object = static_cast<const UObjectHeader*>(static_cast<void*>(player));
    return (static_cast<uint32_t>(object->flags) & 0x30u) == 0 &&
           object->index == target.index && object->class_ptr == target.klass;
}
void UpdateNativeFixedWaterline(MyCharacter* player) {
    const int height = WaterConfig().height_offset;
    const bool desired_enabled = height != 0 && !WaterNoSwimming() &&
                                 !g_restart_requested &&
                                 !g_full_level_reload_requested;
    // No override to apply or restore: do not validate the global registry.
    if (!s_fixed_waterline.player && !desired_enabled) return;
    const auto* objects = WaterObjects();
    if (!objects || !player || !IsReadable(player, sizeof(MyCharacter))) {
        // On a world reload the previous address cannot be restored safely.
        s_fixed_waterline = {};
        return;
    }
    if (s_fixed_waterline.player && !FixedWaterlineIsLive(player, objects))
        s_fixed_waterline = {};

    // A stale cached actor may have been discarded by the lifetime check.
    if (!s_fixed_waterline.player && !desired_enabled) return;
    if (!s_fixed_waterline.player) {
        const ULONGLONG now = GetTickCount64();
        if (now < s_fixed_waterline_retry_after) return;
        s_fixed_waterline_retry_after = now + 5000;
        const auto* object = static_cast<const UObjectHeader*>(static_cast<void*>(player));
        if ((static_cast<uint32_t>(object->flags) & 0x30u) != 0 ||
            object->index < 0 || object->index >= objects->num_elements ||
            objects->objects[object->index].object != static_cast<void*>(player)) return;
        const int32_t property_offset = FindNativeFixedWaterlineOffset(player);
        if (property_offset < 0) {
            if (!s_fixed_waterline_warned) {
                s_fixed_waterline_warned = true;
                Log(L"Water height: BP_MyCharacter FixedWaterHeight FloatProperty unavailable; no native waterline write attempted");
            }
            return;
        }
        auto* address = reinterpret_cast<float*>(
            reinterpret_cast<uint8_t*>(player) + property_offset);
        if (!IsReadable(address, sizeof(float)) ||
            !std::isfinite(*address) || std::fabs(*address) > 100000.0f)
            return;
        // Refunct's saved fixed waterline should agree approximately with
        // the known original ocean surface, not an unrelated float.
        float surface_z = 0.0f;
        if (!RaisedWaterOriginalSurface(objects, surface_z)) return;
        if (std::fabs(*address - surface_z) > 500.0f) {
            if (!s_fixed_waterline_warned) {
                s_fixed_waterline_warned = true;
                Log(L"Water height: FixedWaterHeight did not match the baseline ocean surface; refusing unverified override");
            }
            return;
        }
        s_fixed_waterline = {player, object->index,
            objects->objects[object->index].serial_number,
            object->class_ptr, property_offset, *address, *address, false};
        Log((L"Water height: verified Refunct FixedWaterHeight float; baseline=" +
             std::to_wstring(*address) + L" oceanSurface=" +
             std::to_wstring(surface_z)).c_str());
    }

    auto* value = reinterpret_cast<float*>(
        reinterpret_cast<uint8_t*>(player) + s_fixed_waterline.offset);
    if (!IsReadable(value, sizeof(float))) return;
    if (!desired_enabled) {
        // Don't overwrite a game-side change that no longer matches our value.
        if (s_fixed_waterline.modified &&
            std::fabs(*value - s_fixed_waterline.last_written) < 0.5f &&
            IsWritable(value, sizeof(float))) {
            *value = s_fixed_waterline.original;
            if (std::fabs(*value - s_fixed_waterline.original) >= 0.5f) return;
        }
        s_fixed_waterline = {};
        s_fixed_waterline_logged = false;
        s_fixed_waterline_retry_after = 0;
        return;
    }
    // IMPORTANT: Refunct's water-volume exit event still fires at the OLD
    // brush boundary. If FixedWaterHeight is raised during this event, its
    // Blueprint water-exit code can snap the character straight to that
    // raised height (observed as a teleport out of the original water).
    // Keep Refunct's original waterline while its real WaterVolume owns the
    // player. Only enable the elevated character waterline in the added
    // virtual layer after the native volume has actually been exited.
    const void* active_volume = SwimCurrentPhysicsVolume(player);
    const bool inside_native_water = height > 0 && active_volume &&
        WaterIsA(const_cast<void*>(active_volume), L"PhysicsVolume") &&
        !WaterIsA(const_cast<void*>(active_volume), L"DefaultPhysicsVolume");
    const float wanted = s_fixed_waterline.original +
        static_cast<float>(inside_native_water ? 0 : height);
    if (inside_native_water && !s_fixed_waterline_old_volume_logged) {
        s_fixed_waterline_old_volume_logged = true;
        Log(L"Water height: native water volume still active; using original FixedWaterHeight until the old boundary is exited");
    }
    if (!inside_native_water && height > 0 &&
        !s_fixed_waterline_virtual_logged) {
        s_fixed_waterline_virtual_logged = true;
        Log(L"Water height: entered raised layer; switching FixedWaterHeight to raised surface");
    }
    if (!std::isfinite(wanted) || std::fabs(wanted) > 150000.0f) return;
    if (std::fabs(*value - wanted) < 0.5f) return;
    if (!IsWritable(value, sizeof(float))) return;
    // Preserve the original before the first write; if verification fails,
    // retaining ownership allows a later reset to restore it.
    s_fixed_waterline.modified = true;
    s_fixed_waterline.last_written = wanted;
    *value = wanted;
    if (std::fabs(*value - wanted) < 0.5f && !s_fixed_waterline_logged) {
        s_fixed_waterline_logged = true;
        Log((L"Water height: native FixedWaterHeight set to " +
             std::to_wstring(wanted) + L" (Refunct waterline override; runtime swimming unverified)").c_str());
    }
}


void UpdateRaisedWaterFallback(MyCharacter* player) {
    const int offset = WaterConfig().height_offset;
    // The inactive fallback owns no volume to restore. Do not inspect Unreal
    // objects when the native brush works or no raised layer is requested.
    if (!s_raised_water.active && (offset <= 0 || WaterNoSwimming() ||
        g_flying || g_noclip || g_restart_requested || g_full_level_reload_requested ||
        !s_height_volume_warning_logged || s_height_volume_success_logged)) return;
    const auto* objects = WaterObjects();
    if (!objects) return;
    // FixedWaterHeight controls Refunct's waterline, but UE4 PhysSwimming
    // still needs a water volume. Keep the verified default-volume water bit
    // while inside the raised layer; prevent the legacy fallback from forcing
    // movement modes over and over near the new surface.
    const bool native_waterline_owned = player && s_fixed_waterline.modified &&
        FixedWaterlineIsLive(player, objects);
    // Do not conflict with the proven no-swimming mechanism, full-level
    // reloads, fly/noclip or a genuinely movable native water brush.
    if (!player || !IsReadable(player, sizeof(MyCharacter)) ||
        !IsReadable(player->movement, sizeof(MovementComponent)) ||
        !IsReadable(player->root_component, sizeof(SceneComponent)) ||
        offset <= 0 || WaterNoSwimming() || g_flying || g_noclip ||
        g_restart_requested || g_full_level_reload_requested ||
        !s_height_volume_warning_logged || s_height_volume_success_logged) {
        RaisedWaterRestoreVirtualVolume(player, objects);
        return;
    }
    float original_z = 0.0f;
    if (!RaisedWaterOriginalSurface(objects, original_z)) {
        RaisedWaterRestoreVirtualVolume(player, objects);
        return;
    }
    const float z = player->root_component->location.z;
    const float top = original_z + static_cast<float>(offset);
    if (!std::isfinite(z) || !std::isfinite(top)) return;
    void* current = SwimCurrentPhysicsVolume(player);
    const bool native_water = current && WaterIsA(current, L"PhysicsVolume") &&
        !WaterIsA(current, L"DefaultPhysicsVolume");
    const bool default_air = current && WaterIsA(current, L"DefaultPhysicsVolume");
    const bool prearm_native_exit = native_water &&
        z >= original_z - 120.0f && z < original_z + 32.0f;
    const float margin = s_raised_water.active ?
        (native_waterline_owned ? 0.0f : 5.0f) : 10.0f;
    const bool inside_raised = default_air && z >= original_z + 8.0f &&
        z < top - margin;
    const bool keep_prearmed = default_air && s_raised_water.active &&
        z >= original_z - 25.0f && z < top - margin;
    if (!prearm_native_exit && !inside_raised && !keep_prearmed) {
        RaisedWaterRestoreVirtualVolume(player, objects, native_waterline_owned);
        return;
    }
    // When rising out of WaterVolume_1, the current volume is still native
    // water. Set the destination DefaultPhysicsVolume water bit NOW instead
    // of waiting for GetPhysicsVolume to return it on a later tick.
    void* destination = default_air ? current :
        RaisedWaterDefaultVolume(player, objects, prearm_native_exit);
    if (!destination) return; // No guessed actor or arbitrary PhysicsVolume.
    if (!WaterIsA(destination, L"DefaultPhysicsVolume")) return;
    if (s_raised_water.active && s_raised_water.volume.actor != destination)
        RaisedWaterRestoreVirtualVolume(player, objects, native_waterline_owned);
    if (!s_raised_water.active) {
        if (!IsReadable(destination, sizeof(UObjectHeader))) return;
        const auto* hdr = static_cast<const UObjectHeader*>(destination);
        if (hdr->index < 0 || hdr->index >= objects->num_elements ||
            objects->objects[hdr->index].object != destination ||
            (static_cast<uint32_t>(hdr->flags) & 0x30u) != 0) return;
        int32_t flag_offset = -1;
        uint8_t flag_mask = 0;
        if (!SwimResolveWaterFlag(destination, flag_offset, flag_mask)) {
            if (!s_raised_water_warning_logged) {
                s_raised_water_warning_logged = true;
                Log(L"Water height: default physics volume bWaterVolume reflection unavailable; raised-water swimming fallback skipped");
            }
            return;
        }
        SwimVolumeTarget volume{destination, hdr->index,
            objects->objects[hdr->index].serial_number, flag_offset, flag_mask};
        uint8_t* byte = SwimFlagAddress(volume, objects);
        if (!byte || (*byte & flag_mask) != 0) return; // Never claim native water.
        // Record ownership BEFORE the write, so failed verification can be
        // restored later. Only one live default volume may be overridden.
        s_raised_water.volume = volume;
        s_raised_default_hint = volume;
        s_raised_water.active = true;
        *byte = static_cast<uint8_t>(*byte | flag_mask);
        if ((*byte & flag_mask) == 0) {
            RaisedWaterRestoreVirtualVolume(player, objects);
            return;
        }
        if (!s_raised_water_logged) {
            s_raised_water_logged = true;
            Log(L"Water height: static WaterVolume brush cannot move; native DefaultPhysicsVolume swimming fallback enabled for raised surface (requires in-game verification)");
        }
    }
    if (prearm_native_exit && !s_raised_prearm_logged) {
        s_raised_prearm_logged = true;
        Log(L"Water height: prearmed DefaultPhysicsVolume as water BEFORE old WaterVolume exit");
    }
    // Another native update may clear the temporary water bit. Keep our
    // tracked default volume coherent before any movement-mode transition.
    uint8_t* current_bit = SwimFlagAddress(s_raised_water.volume, objects);
    if (!current_bit) {
        RaisedWaterRestoreVirtualVolume(player, objects, native_waterline_owned);
        return;
    }
    if ((*current_bit & s_raised_water.volume.flag_mask) == 0) {
        *current_bit = static_cast<uint8_t>(
            *current_bit | s_raised_water.volume.flag_mask);
        if ((*current_bit & s_raised_water.volume.flag_mask) == 0) return;
    }
    const bool needs_initial_swim = !native_water &&
        player->movement->movement_mode != 4 &&
        (!native_waterline_owned || !s_raised_water.owns_swimming_mode);
    if (needs_initial_swim) {
        if (!RaisedWaterSetMovementMode(player, 4)) {
            if (!s_raised_water_warning_logged) {
                s_raised_water_warning_logged = true;
                Log(L"Water height: could not enter native MOVE_Swimming in the raised-water fallback");
            }
            RaisedWaterRestoreVirtualVolume(player, objects, native_waterline_owned);
            return;
        }
        s_raised_water.owns_swimming_mode = true;
    }
    // With FixedWaterHeight active, Refunct may switch to walking/falling as
    // the character breaks the surface or touches geometry. Do not force it
    // back to swimming every game tick (the observed collision bounce).

}

// The underwater post-process brush remains at its original level when the
// water mesh rises. Safely extend PostWater's visual influence *only* while
// the player's camera is in that extra layer. This does not alter water
// movement or collision. bEnabled and bUnbound are adjacent reflected native
// BoolProperties, so require their metadata to agree before any bit writes.
bool UnderwaterResolveRaisedFlags(const GlobalObjectArray* objects) {
    auto& target = s_underwater_target;
    if (!UnderwaterTargetValid(objects)) return false;
    if (target.unbound_offset >= 0 && target.enabled_offset >= 0 &&
        target.unbound_mask && target.enabled_mask) return true;
    void* klass = static_cast<const UObjectHeader*>(target.actor)->class_ptr;
    const auto unbound = UnderwaterFindBoolField(klass, L"bUnbound");
    const auto enabled = UnderwaterFindBoolField(klass, L"bEnabled");
    if (!unbound.type_correct || !enabled.type_correct) return false;
    constexpr size_t kLayouts[] = {0x50, 0x54, 0x58, 0x5C, 0x60, 0x64};
    UnderwaterBoolBits found_unbound{}, found_enabled{};
    int candidates = 0;
    for (const size_t layout : kLayouts) {
        UnderwaterBoolBits u{}, e{};
        if (!UnderwaterReadBoolBits(unbound, layout, u) ||
            !UnderwaterReadBoolBits(enabled, layout, e) ||
            !(UnderwaterAdjacentBoolBits(u, e) ||
              UnderwaterAdjacentBoolBits(e, u))) continue;
        if (u.address_offset > 0x1000 || e.address_offset > 0x1000 ||
            (u.address_offset == e.address_offset && u.mask == e.mask)) continue;
        ++candidates;
        found_unbound = u;
        found_enabled = e;
    }
    if (candidates != 1) return false;
    target.unbound_offset = found_unbound.address_offset;
    target.unbound_mask = found_unbound.mask;
    target.enabled_offset = found_enabled.address_offset;
    target.enabled_mask = found_enabled.mask;
    return true;
}

uint8_t* UnderwaterRaisedFlagAddress(int32_t offset,
                                     const GlobalObjectArray* objects) {
    if (!UnderwaterTargetValid(objects) || offset < 0 || offset > 0x1000)
        return nullptr;
    auto* address = static_cast<uint8_t*>(s_underwater_target.actor) + offset;
    return IsWritable(address, 1) ? address : nullptr;
}

// No unverified pointer writes; preserve unrelated packed flags in each byte.
void UnderwaterRestoreRaisedEffect(const GlobalObjectArray* objects) {
    auto& target = s_underwater_target;
    if (!target.raised_effect_owned) return;
    if (!UnderwaterTargetValid(objects)) {
        target.raised_effect_owned = false; // Actor was destroyed on reload.
        return;
    }
    auto* unbound = UnderwaterRaisedFlagAddress(target.unbound_offset, objects);
    auto* enabled = UnderwaterRaisedFlagAddress(target.enabled_offset, objects);
    if (!unbound || !enabled) return; // Retain ownership to retry restoration.
    const auto set_bit = [](uint8_t* field, uint8_t mask, bool value) {
        *field = static_cast<uint8_t>((*field & static_cast<uint8_t>(~mask)) |
                                      (value ? mask : 0));
        return ((*field & mask) != 0) == value;
    };
    const bool restored_unbound = set_bit(unbound, target.unbound_mask,
                                          target.original_unbound);
    const bool restored_enabled = set_bit(enabled, target.enabled_mask,
                                          target.original_enabled);
    if (restored_unbound && restored_enabled) target.raised_effect_owned = false;
}

// Hysteresis around the surface prevents the post-process from flickering on
// and off as the camera bobs near the boundary. Only the newly raised layer
// needs a virtual volume; below the native surface, PostWater works normally.
bool UnderwaterCameraInRaisedLayer(float camera_z, float original_z,
                                  float raised_z, bool already_active) {
    if (!std::isfinite(camera_z) || !std::isfinite(original_z) ||
        !std::isfinite(raised_z) || raised_z <= original_z + 30.0f)
        return false;
    const float lower = original_z + (already_active ? 4.0f : 12.0f);
    const float upper = raised_z + (already_active ? 8.0f : -12.0f);
    return camera_z > lower && camera_z < upper;
}

// Reuse the FOV module's validated camera-discovery strategy: on the
// shipped character the camera is a player-owned component, but not always
// exposed as a reflected BP property. Cache UObject index+serial so a reload
// cannot leave us following a reclaimed address. A full scan runs only if
// direct reflected lookup fails, and is throttled to once per five seconds.
struct RaisedWaterCameraTarget {
    void* component = nullptr;
    MyCharacter* owner = nullptr;
    int32_t index = -1;
    int32_t serial = 0;
};
RaisedWaterCameraTarget s_raised_water_camera{};
ULONGLONG s_raised_water_camera_next_scan = 0;

bool RaisedWaterCameraOwnedBy(void* candidate, MyCharacter* player) {
    if (!candidate || !player || !WaterIsA(candidate, L"CameraComponent") ||
        !IsReadable(candidate, sizeof(UObjectHeader))) return false;
    void* outer = static_cast<const UObjectHeader*>(candidate)->outer;
    for (int depth = 0; depth < 5 && IsReadable(outer, sizeof(UObjectHeader)); ++depth) {
        if (outer == player) return true;
        outer = static_cast<const UObjectHeader*>(outer)->outer;
    }
    // A verified reflected property on this player can point to a camera
    // whose Outer chain is not the pawn (e.g. nested BP component).
    for (const wchar_t* name : {L"FirstPersonCamera", L"FirstPersonCameraComponent", L"Camera"}) {
        if (ReadReflectedObject(player, name) == candidate) return true;
    }
    return false;
}

bool RaisedWaterFindCamera(MyCharacter* player, const GlobalObjectArray* objects,
                           Vec3& camera_location) {
    if (!player || !objects || !objects->objects) return false;
    const auto live = [&](void* camera, int32_t index, int32_t serial) {
        if (index < 0 || index >= objects->num_elements ||
            objects->objects[index].object != camera ||
            objects->objects[index].serial_number != serial ||
            !IsReadable(camera, sizeof(UObjectHeader))) return false;
        const auto* header = static_cast<const UObjectHeader*>(camera);
        return header->index == index &&
            (static_cast<uint32_t>(header->flags) & 0x30u) == 0;
    };
    if (s_raised_water_camera.owner == player &&
        live(s_raised_water_camera.component, s_raised_water_camera.index,
             s_raised_water_camera.serial) &&
        RaisedWaterCameraOwnedBy(s_raised_water_camera.component, player) &&
        WaterGetLocation(s_raised_water_camera.component, camera_location)) return true;
    s_raised_water_camera = {};
    const ULONGLONG now = GetTickCount64();
    if (now < s_raised_water_camera_next_scan) return false;
    s_raised_water_camera_next_scan = now + 5000;
    const auto bind = [&](void* camera) {
        if (!RaisedWaterCameraOwnedBy(camera, player)) return false;
        const auto* header = static_cast<const UObjectHeader*>(camera);
        const int32_t index = header->index;
        if (index < 0 || index >= objects->num_elements ||
            objects->objects[index].object != camera ||
            !live(camera, index, objects->objects[index].serial_number) ||
            !WaterGetLocation(camera, camera_location)) return false;
        s_raised_water_camera = {camera, player, index,
                                 objects->objects[index].serial_number};
        return true;
    };
    for (const wchar_t* name : {L"FirstPersonCamera", L"FirstPersonCameraComponent", L"Camera"}) {
        if (bind(ReadReflectedObject(player, name))) return true;
    }
    // Find only camera components whose Outer chain leads to this pawn.
    // Skip costly FName conversion for unrelated global UObjects.
    const ULONGLONG started = GetTickCount64();
    for (int32_t i = 0; i < objects->num_elements; ++i) {
        if (GetTickCount64() - started > 8) break;
        const auto* candidate = objects->objects[i].object;
        if (!candidate || !IsReadable(candidate, sizeof(UObjectHeader)) ||
            (static_cast<uint32_t>(candidate->flags) & 0x30u) != 0) continue;
        void* outer = candidate->outer;
        bool belongs = false;
        for (int depth = 0; depth < 5 && IsReadable(outer, sizeof(UObjectHeader)); ++depth) {
            if (outer == player) { belongs = true; break; }
            outer = static_cast<const UObjectHeader*>(outer)->outer;
        }
        if (!belongs) continue;
        const auto name = WaterObjectName(const_cast<UObjectHeader*>(candidate));
        if ((name == L"FirstPersonCamera" || name == L"FirstPersonCameraComponent") &&
            bind(const_cast<UObjectHeader*>(candidate))) return true;
    }
    return false;
}

// The rendered view is owned by PlayerCameraManager, not necessarily by a
// camera component whose Outer chain reaches the pawn. Resolve this first;
// the older component search remains a safe fallback for engine variants.
struct RaisedWaterViewTarget {
    PlayerController* controller = nullptr;
    void* manager = nullptr;
    int32_t index = -1;
    int32_t serial = 0;
    void* location_function = nullptr;
    int32_t return_offset = -1;
};
RaisedWaterViewTarget s_raised_water_view{};
ULONGLONG s_raised_water_view_retry_after = 0;

bool RaisedWaterRenderedViewLocation(MyCharacter* player,
                                     const GlobalObjectArray* objects,
                                     Vec3& location) {
    if (!player || !objects || !objects->objects || !g_process_event ||
        !IsReadable(player, sizeof(MyCharacter)) ||
        !IsReadable(player->controller, sizeof(PlayerController))) return false;
    const auto valid = [&]() {
        const auto& t = s_raised_water_view;
        if (t.controller != player->controller || !t.manager ||
            !t.location_function || t.return_offset < 0 ||
            t.index < 0 || t.index >= objects->num_elements ||
            objects->objects[t.index].object != t.manager ||
            objects->objects[t.index].serial_number != t.serial ||
            !IsReadable(t.manager, sizeof(UObjectHeader))) return false;
        const auto* h = static_cast<const UObjectHeader*>(t.manager);
        return h->index == t.index &&
               (static_cast<uint32_t>(h->flags) & 0x30u) == 0 &&
               WaterIsA(t.manager, L"PlayerCameraManager");
    };
    if (!valid()) {
        s_raised_water_view = {};
        const ULONGLONG now = GetTickCount64();
        if (now < s_raised_water_view_retry_after) return false;
        s_raised_water_view_retry_after = now + 5000;
        void* manager = ReadReflectedObject(player->controller, L"PlayerCameraManager");
        if (!manager || !IsReadable(manager, sizeof(UObjectHeader)) ||
            !WaterIsA(manager, L"PlayerCameraManager")) return false;
        const auto* h = static_cast<const UObjectHeader*>(manager);
        if (h->index < 0 || h->index >= objects->num_elements ||
            objects->objects[h->index].object != manager ||
            (static_cast<uint32_t>(h->flags) & 0x30u) != 0) return false;
        void* fn = FindReflectedFunction(manager, L"GetCameraLocation");
        const int32_t ret = WaterParam(fn, L"ReturnValue", L"StructProperty");
        if (ret < 0 || ret > 96 - static_cast<int32_t>(sizeof(Vec3))) return false;
        s_raised_water_view = {player->controller, manager, h->index,
            objects->objects[h->index].serial_number, fn, ret};
    }
    alignas(16) std::array<uint8_t, 128> params{};
    g_process_event(s_raised_water_view.manager,
                    s_raised_water_view.location_function, params.data());
    std::memcpy(&location, params.data() + s_raised_water_view.return_offset,
                sizeof(location));
    return std::isfinite(location.x) && std::isfinite(location.y) &&
           std::isfinite(location.z) &&
           std::fabs(location.z) < 200000.0f;
}

void UnderwaterUpdateRaisedEffect(MyCharacter* player,
                                 const GlobalObjectArray* objects) {
    auto& target = s_underwater_target;
    const bool feature_enabled = player && WaterConfig().height_offset > 0 &&
        !WaterNoSwimming() && !WaterNoTint() && !g_flying && !g_noclip &&
        !g_restart_requested && !g_full_level_reload_requested;
    if (!feature_enabled || !UnderwaterTargetValid(objects)) {
        UnderwaterRestoreRaisedEffect(objects);
        return;
    }
    float original_z = 0.0f;
    if (!RaisedWaterOriginalSurface(objects, original_z)) {
        UnderwaterRestoreRaisedEffect(objects);
        return;
    }
    // The FirstPersonCamera is the correct sensor for tint: using the feet
    // makes the entire screen turn blue while the head is above the water.
    Vec3 camera_location{};
    if (!RaisedWaterRenderedViewLocation(player, objects, camera_location) &&
        !RaisedWaterFindCamera(player, objects, camera_location)) {
        if (!s_raised_postprocess_warning_logged) {
            s_raised_postprocess_warning_logged = true;
            Log(L"Water height: raised-water tint cannot resolve a validated rendered camera view or player camera component");
        }
        UnderwaterRestoreRaisedEffect(objects);
        return;
    }
    const float raised_z = original_z + static_cast<float>(WaterConfig().height_offset);
    if (!UnderwaterCameraInRaisedLayer(camera_location.z, original_z, raised_z,
                                      target.raised_effect_owned)) {
        UnderwaterRestoreRaisedEffect(objects);
        return;
    }
    if (!UnderwaterResolveRaisedFlags(objects)) {
        if (!s_raised_postprocess_warning_logged) {
            s_raised_postprocess_warning_logged = true;
            Log(L"Water height: PostWater bUnbound/bEnabled reflected bitfield validation failed; raised-water tint skipped");
        }
        return;
    }
    auto* unbound = UnderwaterRaisedFlagAddress(target.unbound_offset, objects);
    auto* enabled = UnderwaterRaisedFlagAddress(target.enabled_offset, objects);
    if (!unbound || !enabled) return;
    if (!target.raised_effect_owned) {
        target.original_unbound = (*unbound & target.unbound_mask) != 0;
        target.original_enabled = (*enabled & target.enabled_mask) != 0;
        target.raised_effect_owned = true; // Own modifications before writing.
    }
    *unbound = static_cast<uint8_t>(*unbound | target.unbound_mask);
    *enabled = static_cast<uint8_t>(*enabled | target.enabled_mask);
    if (((*unbound & target.unbound_mask) != 0) &&
        ((*enabled & target.enabled_mask) != 0) &&
        !s_raised_postprocess_logged) {
        s_raised_postprocess_logged = true;
        Log(L"Water height: PostWater temporarily unbound/enabled while the camera is in raised water");
    }
}

// Suppress water mode transitions at their source. Scan in bounded batches
// because a player may enter a new PhysicsVolume later in the same level.
void SwimScanVolumes(const GlobalObjectArray* objects) {
    const ULONGLONG now = GetTickCount64();
    if (s_swim_scan_cursor < 0) {
        if (now < s_swim_next_scan) return;
        s_swim_scan_cursor = 0;
    }
    const ULONGLONG started = now;
    size_t scanned = 0;
    static std::unordered_map<void*, bool> is_physics_class;
    while (s_swim_scan_cursor < objects->num_elements &&
           scanned++ < kScanBatch && GetTickCount64() - started < kScanCpuBudgetMs) {
        const GlobalObjectItem& item = objects->objects[s_swim_scan_cursor++];
        if (!item.object || !IsReadable(item.object, sizeof(UObjectHeader))) continue;
        const auto* actor = item.object;
        if ((static_cast<uint32_t>(actor->flags) & 0x30u) != 0) continue;
        auto known = is_physics_class.find(actor->class_ptr);
        bool physics = false;
        if (known != is_physics_class.end()) physics = known->second;
        else {
            physics = WaterIsA(item.object, L"PhysicsVolume");
            if (is_physics_class.size() < 4096)
                is_physics_class.emplace(actor->class_ptr, physics);
        }
        if (physics) SwimDisableVolume(item.object, objects);
    }
    if (s_swim_scan_cursor >= objects->num_elements) {
        s_swim_scan_cursor = -1;
        s_swim_next_scan = now + 5000;
    }
}
} // namespace

bool IsWaterRemoved() { return WaterConfig().removed; }
void SetWaterRemoved(bool enabled) {
    if (WaterConfig().removed == enabled) return;
    WaterConfig().removed = enabled;
    s_world_geometry_warning_logged = false;
    s_scan_cursor = WaterSurfaceHidden() ? 0 : -1;
    s_next_scan = 0;
    s_next_update = 0;
    QueueWaterConfigSave(SurfaceRemoved);
    Notify(enabled ? L"Ocean rendering hidden (water physics unchanged)" :
                     L"Restoring native ocean rendering");
}
bool IsAllWaterRemoved() { return WaterConfig().remove_all; }
void SetAllWaterRemoved(bool enabled) {
    auto& settings = WaterConfig();
    if (settings.remove_all == enabled) return;
    settings.remove_all = enabled;
    // Reuse the existing per-feature restore/identity machinery. A single
    // master switch does not call the individual setters, so their saved
    // preferences remain exactly as they were.
    s_world_geometry_warning_logged = false;
    s_scan_cursor = WaterSurfaceHidden() ? 0 : -1;
    s_next_scan = 0;
    s_next_update = 0;
    if (WaterNoSwimming()) {
        s_swim_scan_cursor = 0;
        s_swim_next_scan = 0;
        s_swim_resolution_warned = false;
    } else {
        SwimRestoreVolumes();
        s_swim_scan_cursor = -1;
    }
    s_underwater_next_apply = 0;
    s_underwater_next_scan = 0;
    s_underwater_restore_pending = !WaterNoTint() &&
        !(settings.underwater_custom && settings.underwater_intensity_percent > 0);
    s_splash_scan_cursor = WaterNoSplashes() ? 0 : -1;
    s_splash_next_scan = 0;
    s_splash_retry_after = 0;
    if (!WaterNoSplashes()) WaterRestoreSplashTargets(WaterObjects());
    s_fixed_waterline_retry_after = 0;
    QueueWaterConfigSave(RemoveAll);
    Notify(enabled ? L"Remove water ON: surface, swimming, tint and splashes disabled" :
                     L"Remove water OFF: individual water settings restored");
}
bool IsWaterSwimmingDisabled() { return WaterConfig().swimming_disabled; }
void SetWaterSwimmingDisabled(bool enabled) {
    if (WaterConfig().swimming_disabled == enabled) return;
    WaterConfig().swimming_disabled = enabled;
    if (WaterNoSwimming()) {
        s_swim_scan_cursor = 0;
        s_swim_next_scan = 0;
        s_swim_resolution_warned = false;
    } else {
        SwimRestoreVolumes();
        s_swim_scan_cursor = -1;
    }
    s_fixed_waterline_retry_after = 0;
    QueueWaterConfigSave(SwimmingDisabled);
    Notify(enabled ? L"Swimming disabled: water physics volumes suppressed" :
                     L"Swimming restored to game control");
}
int GetWaterHeightOffset() { return WaterConfig().height_offset; }
void SetWaterHeightOffset(int offset) {
    if (offset < -100000 || offset > 100000 ||
        WaterConfig().height_offset == offset) return;
    WaterConfig().height_offset = offset;
    s_height_volume_scan_cursor = offset != 0 ? 0 : -1;
    s_height_volume_next_scan = 0;
    s_height_volume_next_update = 0;
    s_height_volume_warning_logged = false;
    s_height_volume_success_logged = false;
    s_raised_water_logged = false;
    s_raised_water_warning_logged = false;
    s_fixed_waterline_retry_after = 0;
    s_fixed_waterline_warned = false;
    s_fixed_waterline_logged = false;
    s_fixed_waterline_old_volume_logged = false;
    s_fixed_waterline_virtual_logged = false;
    s_world_geometry_warning_logged = false;
    s_scan_cursor = offset != 0 ? 0 : -1;
    s_next_scan = 0;
    s_next_update = 0;
    QueueWaterConfigSave(SurfaceHeight);
    Notify(L"Water height offset: " + std::to_wstring(offset) +
           L" uu (surface + supported physics/raised-water fallback)");
}
void AdjustWaterHeightOffset(int direction) {
    const int next = (std::clamp)(GetWaterHeightOffset() +
                                 (direction >= 0 ? 100 : -100), -100000, 100000);
    SetWaterHeightOffset(next);
}
void UpdateWaterSwimming(MyCharacter* player) {
    // Called both before and after native tick. For completely default water,
    // there is no physics override or outstanding restore to run.
    if (WaterConfig().height_offset == 0 && !WaterNoSwimming() &&
        !s_fixed_waterline.player && !s_raised_water.active && s_swim_volumes.empty()) {
        return;
    }
    UpdateNativeFixedWaterline(player);
    UpdateRaisedWaterFallback(player);
    if (!WaterNoSwimming()) {
        if (!s_swim_volumes.empty()) SwimRestoreVolumes();
        return;
    }
    if (!player || !IsReadable(player, sizeof(MyCharacter)) ||
        !IsReadable(player->movement, sizeof(MovementComponent)) ||
        !IsReadable(player->root_component, sizeof(SceneComponent))) return;
    if (s_swim_player != player) {
        SwimRestoreVolumes();
        s_swim_player = player;
        s_swim_scan_cursor = 0;
        s_swim_next_scan = 0;
    }
    if (g_restart_requested || g_full_level_reload_requested) {
        SwimRestoreVolumes();
        return;
    }
    const auto* objects = WaterObjects();
    if (!objects) return;
    // Invalidate handles whose UE4 object-array serials changed on reload.
    s_swim_volumes.erase(std::remove_if(s_swim_volumes.begin(), s_swim_volumes.end(),
        [objects](const SwimVolumeTarget& v) { return !SwimVolumeValid(v, objects); }),
        s_swim_volumes.end());
    void* current = nullptr;
    bool current_safe = false;
    if (!g_flying && player->movement->movement_mode == 4) {
        current = SwimCurrentPhysicsVolume(player);
        if (current) current_safe = SwimDisableVolume(current, objects);
    }
    SwimScanVolumes(objects);
    if (g_flying || player->movement->movement_mode != 4 || !g_process_event)
        return;
    // Only switch out of swimming once the current water volume is no
    // longer flagged as water. Otherwise collisions can re-enter swimming
    // and repeatedly bounce the player between MOVE_Falling / MOVE_Swimming.
    if (!current_safe && !(s_swim_scan_cursor < 0 && !s_swim_volumes.empty()))
        return;
    void* movement = player->movement;
    void* fn = FindReflectedFunction(movement, L"SetMovementMode");
    const int32_t mode = WaterParam(fn, L"NewMovementMode", L"ByteProperty");
    if (mode < 0) {
        if (!s_swim_resolution_warned) {
            s_swim_resolution_warned = true;
            Log(L"Water gameplay: SetMovementMode reflection unavailable");
            Notify(L"Swimming override unsupported; see RefunctUtility.log");
        }
        return;
    }
    alignas(16) std::array<uint8_t, 128> args{};
    args[mode] = 3; // MOVE_Falling; walking resumes when landing.
    g_process_event(movement, fn, args.data());
}
bool IsWaterCustomColourEnabled() { return WaterConfig().custom; }
void SetWaterCustomColourEnabled(bool enabled) {
    if (WaterConfig().custom == enabled) return;
    WaterConfig().custom = enabled;
    s_restore_pending = !enabled;
    // Start Game Default restoration immediately while still in the native
    // menu/tick context; later game ticks finish any remaining targets.
    if (!enabled) {
        const auto* objects = WaterObjects();
        size_t attempted = 0;
        for (auto& target : s_targets) {
            if (attempted >= kApplyBatch) break;
            if (!target.dynamic_material) continue;
            if (!WaterIsTargetValid(target, objects)) {
                target = {};
                continue;
            }
            target.next_verify = 0;
            WaterApplyColour(target, false, 0);
            ++attempted;
        }
    }
    WaterSettingsChanged(enabled ? L"Water colour: Custom" : L"Water colour: Game default",
                         SurfaceEnabled);
}
int GetWaterColourIntensityPercent() {
    return WaterConfig().surface_intensity_percent;
}
void SetWaterColourIntensityPercent(int percent) {
    if (percent < 0) return; // No artificial upper limit.
    auto& settings = WaterConfig();
    if (settings.surface_intensity_percent == percent && settings.custom)
        return;
    settings.surface_intensity_percent = percent;
    settings.custom = true;
    s_restore_pending = percent == 0;
    WaterSettingsChanged(L"Surface intensity: " +
                         std::to_wstring(percent) + L"%",
                         static_cast<uint8_t>(SurfaceIntensity | SurfaceEnabled));
}
std::wstring GetWaterCustomColourHex() { return WaterHex(WaterConfig().rgb); }
void SetWaterCustomColourHex(uint32_t rgb) {
    if (rgb > 0xFFFFFFu) return;
    auto& settings = WaterConfig();
    if (settings.rgb == rgb && settings.custom) return;
    settings.rgb = rgb;
    // Selecting a colour is an explicit request to display it. Do not make
    // the user toggle Game default -> Custom as a second, hidden step.
    settings.custom = true;
    s_restore_pending = false;
    for (auto& target : s_targets) target.unsupported = false;
    s_warning_logged = false;
    WaterSettingsChanged(L"Water colour: " + WaterHex(rgb),
                         static_cast<uint8_t>(SurfaceHex | SurfaceEnabled));
}
bool IsUnderwaterTintRemoved() { return WaterConfig().tint_removed; }
void SetUnderwaterTintRemoved(bool enabled) {
    auto& settings = WaterConfig();
    if (settings.tint_removed == enabled) return;
    settings.tint_removed = enabled;
    s_underwater_restore_pending = !WaterNoTint() &&
        !(settings.underwater_custom && settings.underwater_intensity_percent > 0);
    UnderwaterSettingsChanged(enabled ? L"Underwater tint removed" :
                                       L"Underwater tint restored", RemoveTint);
}
// Category changes rebuild one capture-owned hidden list. Restore first;
// leave unrelated game-managed entries untouched on verification failure.
void WaterReflectionCategoryChanged(uint32_t field, const wchar_t* label, bool visible) {
    s_geometry_unsupported_logged = false;
    s_geometry_enabled_logged = false;
    s_geometry_applied_mask = ~0u;
    s_geometry_retry_after = 0;
    s_reflection_next_verify = 0;
    s_reflection_apply_requested = true;
    if (WaterReflectionHideMask() && s_reflection_targets.empty()) {
        s_reflection_scan_complete = false;
        s_reflection_scan_cursor = 0;
    }
    QueueWaterConfigSave(field);
    Notify(std::wstring(label) + (visible ? L": ON" : L": OFF (capture only)"));
}
bool AreWaterReflectedWallsVisible() { return WaterConfig().reflection_geometry_visible; }
void SetWaterReflectedWallsVisible(bool visible) {
    auto& setting = WaterConfig();
    if (setting.reflection_geometry_visible == visible) return;
    setting.reflection_geometry_visible = visible;
    WaterReflectionCategoryChanged(ReflectionGeometryVisible, L"Water reflected walls/platforms", visible);
}
bool AreWaterReflectedButtonsVisible() { return WaterConfig().reflection_buttons_visible; }
void SetWaterReflectedButtonsVisible(bool visible) {
    auto& setting = WaterConfig();
    if (setting.reflection_buttons_visible == visible) return;
    setting.reflection_buttons_visible = visible;
    WaterReflectionCategoryChanged(ReflectionButtonsVisible, L"Water reflected buttons", visible);
}
bool AreWaterReflectedPipesVisible() { return WaterConfig().reflection_pipes_visible; }
void SetWaterReflectedPipesVisible(bool visible) {
    auto& setting = WaterConfig();
    if (setting.reflection_pipes_visible == visible) return;
    setting.reflection_pipes_visible = visible;
    WaterReflectionCategoryChanged(ReflectionPipesVisible, L"Water reflected pipes", visible);
}
bool AreWaterReflectedBeaconsVisible() { return WaterConfig().reflection_beacons_visible; }
void SetWaterReflectedBeaconsVisible(bool visible) {
    auto& setting = WaterConfig();
    if (setting.reflection_beacons_visible == visible) return;
    setting.reflection_beacons_visible = visible;
    WaterReflectionCategoryChanged(ReflectionBeaconsVisible, L"Water reflected beacon beams", visible);
}
bool AreWaterReflectedJumpPadsVisible() { return WaterConfig().reflection_jump_pads_visible; }
void SetWaterReflectedJumpPadsVisible(bool visible) {
    auto& setting = WaterConfig();
    if (setting.reflection_jump_pads_visible == visible) return;
    setting.reflection_jump_pads_visible = visible;
    WaterReflectionCategoryChanged(ReflectionJumpPadsVisible, L"Water reflected jump pads", visible);
}
bool AreWaterReflectedCubesVisible() { return WaterConfig().reflection_cubes_visible; }
void SetWaterReflectedCubesVisible(bool visible) {
    auto& setting = WaterConfig();
    if (setting.reflection_cubes_visible == visible) return;
    setting.reflection_cubes_visible = visible;
    WaterReflectionCategoryChanged(ReflectionCubesVisible, L"Water reflected cubes", visible);
}
bool AreWaterReflectionsVisible() { return WaterConfig().reflections_visible; }
void SetWaterReflectionsVisible(bool visible) {
    auto& settings = WaterConfig();
    if (settings.reflections_visible == visible) return;
    settings.reflections_visible = visible;
    s_geometry_retry_after = 0;
    if (visible) {
        // Retain this request across player changes and incremental scans.
        // Otherwise an OFF component can get orphaned and never rediscovered.
        s_reflection_restore_pending = true;
        if (s_reflection_targets.empty()) {
            s_reflection_scan_complete = false;
            s_reflection_scan_cursor = 0;
        }
    } else {
        s_reflection_restore_pending = false;
        if (s_reflection_targets.empty()) {
            s_reflection_scan_complete = false;
            s_reflection_retry_after = 0;
        }
    }
    s_reflection_apply_requested = true;
    QueueWaterConfigSave(ReflectionsVisible);
    Notify(visible ? L"Water reflections: ON (original capture)" :
                     L"Water reflections: OFF (planar capture only)");
}
bool AreWaterSplashesRemoved() { return WaterConfig().splashes_removed; }
void SetWaterSplashesRemoved(bool enabled) {
    auto& settings = WaterConfig();
    if (settings.splashes_removed == enabled) return;
    settings.splashes_removed = enabled;
    s_splash_scan_cursor = WaterNoSplashes() ? 0 : -1;
    s_splash_next_scan = 0;
    s_splash_retry_after = 0;
    if (!WaterNoSplashes()) WaterRestoreSplashTargets(WaterObjects());
    QueueWaterConfigSave(RemoveSplashes);
    Notify(enabled ? L"Water splash particles: hidden (visual only)" :
                     L"Water splash particles: restored (visual only)");
}
bool IsUnderwaterCustomColourEnabled() { return WaterConfig().underwater_custom; }
void SetUnderwaterCustomColourEnabled(bool enabled) {
    auto& settings = WaterConfig();
    if (settings.underwater_custom == enabled) return;
    settings.underwater_custom = enabled;
    s_underwater_restore_pending = !enabled;
    UnderwaterSettingsChanged(enabled ? L"Underwater colour: Custom" :
                                       L"Underwater colour: Game default", UnderwaterEnabled);
}
std::wstring GetUnderwaterCustomColourHex() {
    return WaterHex(WaterConfig().underwater_rgb);
}
void SetUnderwaterCustomColourHex(uint32_t rgb) {
    if (rgb > 0xFFFFFFu) return;
    auto& settings = WaterConfig();
    if (settings.underwater_rgb == rgb && settings.underwater_custom) return;
    settings.underwater_rgb = rgb;
    settings.underwater_custom = true;
    s_underwater_restore_pending = false;
    UnderwaterSettingsChanged(L"Underwater colour: " + WaterHex(rgb),
                              static_cast<uint8_t>(UnderwaterHex | UnderwaterEnabled));
}
int GetUnderwaterColourIntensityPercent() {
    return WaterConfig().underwater_intensity_percent;
}
void SetUnderwaterColourIntensityPercent(int percent) {
    if (percent < 0) return; // No artificial upper limit.
    auto& settings = WaterConfig();
    if (settings.underwater_intensity_percent == percent &&
        settings.underwater_custom) return;
    settings.underwater_intensity_percent = percent;
    // Adjusting the intensity is an explicit request to use this appearance.
    settings.underwater_custom = true;
    // At 0%, remove the native override rather than forcing a neutral tint.
    s_underwater_restore_pending = percent == 0;
    UnderwaterSettingsChanged(L"Underwater intensity: " +
                              std::to_wstring(percent) + L"%",
                              static_cast<uint8_t>(UnderwaterIntensity | UnderwaterEnabled));
}
void OpenWaterPicker(bool underwater) {
    if (s_picker_open.exchange(true, std::memory_order_acq_rel)) {
        Notify(L"Water colour picker already open");
        return;
    }
    s_picker_target.store(underwater ? 1 : 0, std::memory_order_release);
    s_picker_initial.store(underwater ? WaterConfig().underwater_rgb :
                                     WaterConfig().rgb, std::memory_order_relaxed);
    s_picker_result.store(kNoPickerResult, std::memory_order_release);
    HANDLE thread = CreateThread(nullptr, 0, WaterPickerThread, nullptr, 0, nullptr);
    if (!thread) {
        s_picker_open.store(false, std::memory_order_release);
        Notify(L"Could not open water colour picker");
    } else CloseHandle(thread);
}
void OpenWaterColourPicker() { OpenWaterPicker(false); }
void OpenUnderwaterColourPicker() { OpenWaterPicker(true); }
void UpdateWaterColours(MyCharacter* player) {
    FlushWaterConfigIfDue();
    const uint32_t picker = s_picker_result.exchange(kNoPickerResult,
                                                     std::memory_order_acq_rel);
    if (picker <= 0xFFFFFFu) {
        if (s_picker_target.load(std::memory_order_acquire) == 1)
            SetUnderwaterCustomColourHex(picker);
        else SetWaterCustomColourHex(picker);
    }
    if (!player || g_restart_requested || g_full_level_reload_requested) return;
    // Physics brush height is independent of the rendered ocean mesh. Run
    // before surface-colour early returns, including while Remove Water is ON.
    UpdateWaterPhysicsHeight();
    WaterUpdateReflections(player);
    if (WaterReflectionHideMask() || !s_geometry_hidden.empty())
        WaterUpdateGeometryReflections(player, WaterObjects());
    // The underwater volume and the surface mesh have independent settings
    // and update lifecycles. Switching off the surface must not disable the
    // post-process tint, and vice versa.
    UpdateUnderwaterColours(player);
    // Independent from the ocean mesh: even when water is hidden, native
    // splash particle systems can be spawned by overlap events.
    if (WaterNoSplashes() || !s_splash_targets.empty())
        WaterUpdateSplashEffects();
    const auto& config = WaterConfig();
    const bool surface_active = config.custom &&
        config.surface_intensity_percent > 0;
    const bool world_active = WaterSurfaceHidden() || config.height_offset != 0;
    if (!surface_active && !world_active && !s_restore_pending &&
        s_targets.empty()) return;
    if (s_player != player) {
        s_player = player;
        s_applied_logged = false;
        s_surface_edit_quiet_until = 0;
        s_scan_cursor = 0;
        s_next_scan = 0;
        s_next_update = 0;
        s_reported = false;
        s_examined_meshes = 0;
        s_ocean_meshes = 0;
    }
    const ULONGLONG now = GetTickCount64();
    if (now < s_next_update) return;
    s_next_update = now + kUpdateIntervalMs;
    const auto* objects = WaterObjects();
    if (!objects) return;
    // Preserve normal discovery on startup and after a level transition;
    // defer only optional rescans while an existing surface is being edited.
    const bool can_discover = world_active || s_targets.empty() ||
        now >= s_surface_edit_quiet_until;
    if (s_scan_cursor < 0 && (surface_active || world_active) && can_discover)
        s_scan_cursor = s_discovery_watermark.Resume(objects->objects, objects->num_elements, now, s_next_scan);
    // A cached water mesh is updated before any optional discovery work.
    // This is the fast path used by repeated palette changes.
    bool discovered_this_tick = false;
    if (s_targets.empty() && s_scan_cursor >= 0 && (surface_active || world_active)) {
        WaterDiscover(objects);
        discovered_this_tick = true;
    }
    size_t seen = 0;
    const size_t count = s_targets.size();
    while (seen++ < (std::min)(kApplyBatch, count)) {
        if (s_apply_cursor >= s_targets.size()) s_apply_cursor = 0;
        WaterTarget& target = s_targets[s_apply_cursor++];
        if (!WaterIsTargetValid(target, objects)) {
            s_next_scan = 0;
            target = {};
            continue;
        }
        if (surface_active || target.dynamic_material)
            WaterApplyColour(target, surface_active, config.rgb,
                             config.surface_intensity_percent);
        if (world_active || target.visibility_modified || target.height_modified)
            WaterApplyWorldGeometry(target, WaterSurfaceHidden(), config.height_offset);
    }
    if (!discovered_this_tick && !s_targets.empty() &&
        s_scan_cursor >= 0 && (surface_active || world_active) && can_discover)
        WaterDiscover(objects);
    s_targets.erase(std::remove_if(s_targets.begin(), s_targets.end(),
        [](const WaterTarget& target) { return target.actor == nullptr; }), s_targets.end());
    if (s_apply_cursor >= s_targets.size()) s_apply_cursor = 0;
    if (!surface_active) {
        s_restore_pending = std::any_of(s_targets.begin(), s_targets.end(),
            [](const WaterTarget& target) { return target.dynamic_material != nullptr; });
    }
    if (!surface_active && !world_active && !s_restore_pending &&
        std::none_of(s_targets.begin(), s_targets.end(), [](const WaterTarget& target) {
            return target.visibility_modified || target.height_modified;
        })) {
        s_targets.clear();
        s_scan_cursor = -1;
    }
}
} // namespace refunct

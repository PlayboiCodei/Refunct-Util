#include "refunct/crosshair.hpp"
#include "refunct/internal_api.hpp"
#include "refunct/performance.hpp"
#include <commdlg.h> // CHOOSECOLORW / ChooseColorW (already linked via Comdlg32)
#include "refunct/features/tiles.hpp"
#include "refunct/features/button_colours.hpp"
#include "refunct/menu_search.hpp"
#include <initializer_list>
#include <array>

namespace refunct {
namespace {
// No gameplay FOV cap. Only avoid signed integer overflow, since the
// configuration value uses an int and Unreal's camera function takes a float.
void AdjustPlayerFovDegrees(int direction) {
    constexpr int64_t kStep = 5;
    const int64_t current = g_player_fov_degrees == 0
        ? 90 : static_cast<int64_t>(g_player_fov_degrees);
    int64_t next = current + (direction < 0 ? -kStep : kStep);
    // Zero means "Default" in existing settings. Skip it when adjusting,
    // so the arrow controls can still reach negative FOV values.
    if (next == 0) next = direction < 0 ? -kStep : kStep;
    if (next < (std::numeric_limits<int>::min)() ||
        next > (std::numeric_limits<int>::max)()) return;
    g_player_fov_degrees = static_cast<int>(next);
    SaveSettings();
}

// Display user height adjustments rather than the absolute world height.
std::wstring SectionOffsetLabel(float offset) {
    const int cm = static_cast<int>(offset);
    return (cm > 0 ? L"+" : L"") + std::to_wstring(cm) + L" cm";
}

std::wstring AllSectionAxisOffsetLabel(int axis) {
    const float reference = GetSectionPositionOffset(1, axis);
    for (int i = 2; i <= kPracticeClusterCount; ++i) {
        if (GetSectionPositionOffset(i, axis) != reference) return L"Mixed offsets";
    }
    return SectionOffsetLabel(reference);
}

// World-only reset commands. They intentionally do not reset native button/tile
// progress, saved checkpoints, HUD, movement configuration, or PB profiles.
void ResetWorldGameplayOptions() {
    // Existing setters restore the native movement rates and persist their
    // individual settings, including the working jump-pad velocity fallback.
    ResetJumpPadHeight();
    ResetPlatformSpeed();
    ResetTubeTravelSpeed();
    ResetSectionSpeed();
    ResetWorldObjectRemovals();
    SetAllWaterRemoved(false);
    SetWaterRemoved(false);
    SetWaterSwimmingDisabled(false);
    SetWaterHeightOffset(0);

    g_animate_section_changes = true;

    // Turn off section/button automation before restoring native placements.
    bool success = true;
    if (g_buttons_sync_with_sections && !SetButtonsSyncWithSections(false))
        success = false;

    bool positions_changed = false;
    bool placements_changed = false;
    for (int section = 1; section <= kPracticeClusterCount; ++section) {
        if (GetSectionPlacement(section) != SectionPlacement::Auto)
            placements_changed = true;
        for (int axis = 0; axis < 3; ++axis) {
            if (GetSectionPositionOffset(section, axis) != 0.0f)
                positions_changed = true;
        }
    }
    if (placements_changed) {
        ResetAllSectionPlacements();
        for (int section = 1; section <= kPracticeClusterCount; ++section) {
            if (GetSectionPlacement(section) != SectionPlacement::Auto)
                success = false;
        }
    }
    if (positions_changed && !ResetAllSectionPositionOffsets()) success = false;

    // Automatic button repair remains enabled across World gameplay resets.
    if (IsButtonTileSelectionDisabled() && !ToggleButtonTileSelection()) success = false;
    if (IsTileSelectionOnContactDisabled() && !ToggleTileSelectionOnContact())
        success = false;
    if (IsWorldCubeOverrideActive() &&
        !RequestCubeWorldAction(CubeWorldAction::RestoreDefault, g_player))
        success = false;

    // Reconstruct vanilla enabled/disabled button gates at the CURRENT progress.
    // This does not rewind completed buttons, unlock future buttons, or clear
    // native tile selection and counters.
    if (!RestoreDefaultButtons()) success = false;
    SaveSettings();
    Notify(success
        ? L"World settings reset: speeds, launch, sections and controls"
        : L"World settings partly reset; some native controls need a map reload");
}

void ResetWorldCustomisation() {
    if (g_crosshair != CrosshairSettings{}) { g_crosshair = {}; SaveSettings(); }
    // Camera FOV is a visual customisation and has its own saved setting.
    if (g_player_fov_degrees != 0) {
        g_player_fov_degrees = 0;
        SaveSettings();
    }
    // Disable overrides rather than overwriting saved custom HEX selections.
    // Future re-enabling still recalls the user's selected colour.
    for (bool pressed : {false, true}) {
        if (IsButtonCustomColourEnabled(pressed))
            SetButtonCustomColourEnabled(pressed, false);
        if (IsButtonBorderCustomColourEnabled(pressed))
            SetButtonBorderCustomColourEnabled(pressed, false);
    }
    if (!IsButtonBeaconVisible()) SetButtonBeaconVisible(true);
    if (IsButtonBeaconCustomColourEnabled())
        SetButtonBeaconCustomColourEnabled(false);

    // Tile palettes and per-tile random RGB require a single batched restore
    // to avoid repeated material sweeps and dozens of settings writes.
    RestoreDefaultTileColours();
    for (bool selected : {false, true}) {
        if (IsWallCustomColourEnabled(selected))
            SetWallCustomColourEnabled(selected, false);
    }
    if (IsTubeCustomColourEnabled()) SetTubeCustomColourEnabled(false);
    if (IsPlatformCustomColourEnabled()) SetPlatformCustomColourEnabled(false);
    if (IsPillarColourEnabled()) SetPillarColourEnabled(false);
    if (ArePillarsRemoved()) SetPillarsRemoved(false);
    if (IsJumpPadCustomColourEnabled()) SetJumpPadCustomColourEnabled(false);
    if (IsWaterCustomColourEnabled()) SetWaterCustomColourEnabled(false);
    if (IsUnderwaterCustomColourEnabled()) SetUnderwaterCustomColourEnabled(false);
    if (IsUnderwaterTintRemoved()) SetUnderwaterTintRemoved(false);
    if (AreWaterSplashesRemoved()) SetWaterSplashesRemoved(false);
    if (!AreWaterReflectionsVisible()) SetWaterReflectionsVisible(true);
    if (!AreWaterReflectedWallsVisible()) SetWaterReflectedWallsVisible(true);
    if (!AreWaterReflectedButtonsVisible()) SetWaterReflectedButtonsVisible(true);
    if (!AreWaterReflectedPipesVisible()) SetWaterReflectedPipesVisible(true);
    if (!AreWaterReflectedBeaconsVisible()) SetWaterReflectedBeaconsVisible(true);
    if (!AreWaterReflectedJumpPadsVisible()) SetWaterReflectedJumpPadsVisible(true);
    if (!AreWaterReflectedCubesVisible()) SetWaterReflectedCubesVisible(true);
    if (IsCustomTimeOfDayEnabled()) SetCustomTimeOfDayEnabled(false);
    ResetSkyColours();
    ResetSkyDiscVisibility();
    ResetSkyEffectVisibility();
    ResetFogColours();
    ResetFogVisibility();
    ResetShadowCustomisation();
    Notify(L"Customisation reset: FOV, visuals, pillars and time of day");
}

// Page through large map folders without overflowing the on-screen HUD.
// Search is a transient menu view, not a saved setting. Matches refer to the
// existing page and selection index, so following a result never executes it.
struct MenuSearchMatch {
    Page page = Page::Main;
    int row = 0;
    std::wstring label;
    std::wstring breadcrumb;
};
std::wstring s_menu_search_query;
std::vector<MenuSearchMatch> s_menu_search_matches;
// Slate key callbacks and the character tick can run on different threads.
// Preserve typed-key order without std::mutex: the new queue is drained on
// EVERY native character tick, including before search is ever opened. Use
// a trivially initialized Win32 lock and fixed-size buffer so this path
// does not depend on MSVCP140's std::mutex implementation at startup.
std::atomic_bool s_menu_search_capture{false};
SRWLOCK s_menu_search_key_lock = SRWLOCK_INIT;
constexpr size_t kMenuSearchKeyQueueLimit = 64;
std::array<int, kMenuSearchKeyQueueLimit> s_menu_search_key_queue{};
size_t s_menu_search_key_count = 0; // Protected by s_menu_search_key_lock.
Page s_menu_search_return_page = Page::Main;
int s_menu_search_return_selection = 0;
constexpr size_t kMaxMenuSearchLength = 36;
constexpr size_t kMaxMenuSearchMatches = 40;

constexpr size_t kMapsPerPage = 7;
size_t s_maps_page = 0;
size_t MapPageStart() { return s_maps_page * kMapsPerPage; }
size_t MapsOnPage() {
    const auto count = CustomMapFileCount();
    const auto start = MapPageStart();
    return start >= count ? 0 : (std::min)(kMapsPerPage, count - start);
}
void ChangeMapPage(int delta) {
    const size_t pages = (CustomMapFileCount() + kMapsPerPage - 1) / kMapsPerPage;
    if (delta > 0 && s_maps_page + 1 < pages) ++s_maps_page;
    if (delta < 0 && s_maps_page > 0) --s_maps_page;
    g_selection = 3;
}
int s_position_edit_section = 1;

int s_tile_hex_axis = -1; // -1=inactive; 18 = ghost, 19 = crosshair colour editor.
std::wstring s_tile_hex_input;
size_t s_hud_style_element = 0;
// Return the style editor to the page that opened it (the normal customisation
// list or the dedicated key display page).
Page s_hud_style_return_page = Page::HudAppearance;
// Both profile shortcuts open the same pages and persistent data. Remember
// their entry route independently, including across delete confirmation pages.
Page s_hud_profiles_return_page = Page::Hud;
Page s_customisation_profiles_return_page = Page::Customisation;
Page s_world_profiles_return_page = Page::World;
Page s_hotkey_profiles_return_page = Page::Hotkeys;
// KeyDisplay keeps its saved HUD element ID but has an independent Inputs main-menu page.
constexpr size_t kSideHudElementCount = static_cast<size_t>(HudElement::KeyDisplay);
// Menu-only ordering: never reorder HudElement itself, as its numerical values
// are the persistent HUD appearance/position indices in settings.ini.
constexpr std::array<HudElement, 7> kSplitHudElements{{
    HudElement::Timer, HudElement::PBRun, HudElement::SumOfBest,
    HudElement::LastButtonSplit, HudElement::CompareTime,
    HudElement::BestSegmentTime, HudElement::PBSplitTime
}};
constexpr std::array<HudElement, kSideHudElementCount - kSplitHudElements.size()>
kGeneralHudElements{{
    HudElement::Coordinates, HudElement::Cubes, HudElement::Tiles,
    HudElement::FPS, HudElement::EnabledModules, HudElement::PracticeStatus,
    HudElement::RecordStatus, HudElement::Speed, HudElement::CompletionPercent,
    HudElement::ActiveCheats, HudElement::ActiveCustomisations,
    HudElement::GameTime, HudElement::LocalTime, HudElement::ButtonNumber
}};
static_assert(kGeneralHudElements.size() + kSplitHudElements.size() == kSideHudElementCount);

constexpr std::array<const wchar_t*, kHudColourModeCount> kHudColourLabels{{
    L"Default", L"White", L"Cyan", L"Green", L"Yellow", L"Red",
    L"Orange", L"Pink", L"Blue", L"Purple", L"Custom colour"
}};
std::wstring HudRgbString(uint32_t rgb) {
    wchar_t value[7]{};
    swprintf_s(value, L"%06X", rgb & 0xFFFFFFu);
    return value;
}
// Choose the input method independently for each persistent HUD element.
// Method selection is UI-only; the resulting RGB and active preset are saved
// using the existing backward-compatible HudAppearance INI settings.
std::array<bool, kHudElementCount> s_hud_use_windows_picker{};
constexpr uint32_t kHudPickerNoResult = 0xFFFFFFFFu;
constexpr uint32_t kHudPickerCancelled = 0xFFFFFFFEu;
constexpr uint32_t kHudPickerError = 0xFFFFFFFDu;
std::atomic_bool s_hud_picker_open{false};
std::atomic_uint32_t s_hud_picker_result{kHudPickerNoResult};
std::atomic_uint32_t s_hud_picker_initial{0xFFFFFFu};
std::atomic<HWND> s_hud_picker_owner{nullptr};
constexpr size_t kGhostColourPickerTarget = kHudElementCount + 1;
constexpr size_t kCrosshairColourPickerTarget = kHudElementCount + 2;
bool s_crosshair_use_windows_picker = false;
size_t s_hud_picker_target = kHudElementCount; // Owned by game-tick thread only.
bool s_ghost_use_windows_picker = false; // UI-only; colour is persisted.

DWORD WINAPI HudColourPickerThread(LPVOID) {
    COLORREF custom_swatches[16]{};
    const uint32_t rgb = s_hud_picker_initial.load(std::memory_order_relaxed);
    CHOOSECOLORW choice{};
    choice.lStructSize = sizeof(choice);
    choice.hwndOwner = s_hud_picker_owner.load(std::memory_order_relaxed);
    choice.lpCustColors = custom_swatches;
    choice.rgbResult = RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
    choice.Flags = CC_RGBINIT | CC_FULLOPEN | CC_ANYCOLOR;
    if (ChooseColorW(&choice)) {
        const uint32_t selected =
            (static_cast<uint32_t>(GetRValue(choice.rgbResult)) << 16) |
            (static_cast<uint32_t>(GetGValue(choice.rgbResult)) << 8) |
            static_cast<uint32_t>(GetBValue(choice.rgbResult));
        s_hud_picker_result.store(selected, std::memory_order_release);
    } else {
        s_hud_picker_result.store(CommDlgExtendedError() ? kHudPickerError
                                : kHudPickerCancelled, std::memory_order_release);
    }
    s_hud_picker_open.store(false, std::memory_order_release);
    return 0;
}

void OpenHudColourPicker() {
    if (s_hud_style_element >= kHudElementCount) return;
    // Don't overwrite an uncommitted previous result or change its target.
    if (s_hud_picker_result.load(std::memory_order_acquire) != kHudPickerNoResult) {
        Notify(L"Previous HUD colour selection is still being applied");
        return;
    }
    if (s_hud_picker_open.exchange(true, std::memory_order_acq_rel)) {
        Notify(L"HUD colour picker is already open");
        return;
    }
    const HWND foreground = GetForegroundWindow();
    DWORD pid = 0;
    if (foreground) GetWindowThreadProcessId(foreground, &pid);
    s_hud_picker_owner.store(pid == GetCurrentProcessId() ? foreground : nullptr,
                             std::memory_order_relaxed);
    s_hud_picker_target = s_hud_style_element;
    s_hud_picker_initial.store(g_hud_custom_colours[s_hud_style_element],
                               std::memory_order_relaxed);
    HANDLE thread = CreateThread(nullptr, 0, HudColourPickerThread, nullptr, 0, nullptr);
    if (!thread) {
        s_hud_picker_open.store(false, std::memory_order_release);
        Notify(L"Could not create the HUD colour picker thread");
        return;
    }
    CloseHandle(thread);
    Notify(L"Windows colour picker opened (Alt+Tab if hidden by fullscreen)");
}

void CycleHudColour(int direction) {
    if (s_hud_style_element >= kHudElementCount) return;
    const int current = g_hud_colour_modes[s_hud_style_element];
    g_hud_colour_modes[s_hud_style_element] = static_cast<uint8_t>(
        ((current >= 0 && current < kHudColourModeCount ? current : 0) +
            (direction < 0 ? kHudColourModeCount - 1 : 1)) % kHudColourModeCount);
    SaveSettings();
}
std::wstring GhostColourMethodLabel() {
    if (s_tile_hex_axis == 18)
        return L"Ghost colour: HEX #" + s_tile_hex_input +
               L"_  [Enter save / Esc cancel]";
    return s_ghost_use_windows_picker
        ? L"Ghost colour: Windows picker  < >  [Enter to open]"
        : L"Ghost colour: HEX #" + HudRgbString(GetPBGhostColourRgb()) +
          L"  < >  [Enter to edit]";
}

void OpenGhostColourPicker() {
    if (s_hud_picker_result.load(std::memory_order_acquire) != kHudPickerNoResult ||
        s_hud_picker_open.exchange(true, std::memory_order_acq_rel)) {
        Notify(L"A colour picker is already in use");
        return;
    }
    const HWND foreground = GetForegroundWindow();
    DWORD pid = 0;
    if (foreground) GetWindowThreadProcessId(foreground, &pid);
    s_hud_picker_owner.store(pid == GetCurrentProcessId() ? foreground : nullptr,
                             std::memory_order_relaxed);
    s_hud_picker_target = kGhostColourPickerTarget;
    s_hud_picker_initial.store(GetPBGhostColourRgb(), std::memory_order_relaxed);
    HANDLE thread = CreateThread(nullptr, 0, HudColourPickerThread, nullptr, 0, nullptr);
    if (!thread) {
        s_hud_picker_open.store(false, std::memory_order_release);
        s_hud_picker_target = kHudElementCount;
        Notify(L"Could not create the ghost colour picker thread");
        return;
    }
    CloseHandle(thread);
    Notify(L"Windows colour picker opened (Alt+Tab if hidden by fullscreen)");
}

void OpenGhostColourMethod() {
    if (s_ghost_use_windows_picker) {
        OpenGhostColourPicker();
    } else {
        s_tile_hex_axis = 18;
        s_tile_hex_input.clear();
        Notify(L"Type six HEX digits (RRGGBB), Enter to save / Esc to cancel");
    }
}

std::wstring CrosshairColourLabel() {
    if (s_tile_hex_axis == 19)
        return L"Colour: HEX #" + s_tile_hex_input + L"_  [Enter save / Esc cancel]";
    return s_crosshair_use_windows_picker ? L"Colour: Windows picker  < >  [Enter to open]" :
        L"Colour: HEX #" + HudRgbString(static_cast<uint32_t>(g_crosshair.rgb)) + L"  < >  [Enter to edit]";
}
void OpenCrosshairColour() {
    if (!s_crosshair_use_windows_picker) {
        s_tile_hex_axis = 19;
        s_tile_hex_input.clear();
        Notify(L"Type six HEX digits (RRGGBB), Enter to save / Esc to cancel");
        return;
    }
    if (s_hud_picker_result.load(std::memory_order_acquire) != kHudPickerNoResult ||
        s_hud_picker_open.exchange(true, std::memory_order_acq_rel)) {
        Notify(L"A colour picker is already in use");
        return;
    }
    const HWND foreground = GetForegroundWindow();
    DWORD pid = 0;
    if (foreground) GetWindowThreadProcessId(foreground, &pid);
    s_hud_picker_owner.store(pid == GetCurrentProcessId() ? foreground : nullptr,
                             std::memory_order_relaxed);
    s_hud_picker_target = kCrosshairColourPickerTarget;
    s_hud_picker_initial.store(static_cast<uint32_t>(g_crosshair.rgb), std::memory_order_relaxed);
    HANDLE thread = CreateThread(nullptr, 0, HudColourPickerThread, nullptr, 0, nullptr);
    if (!thread) {
        s_hud_picker_open.store(false, std::memory_order_release);
        s_hud_picker_target = kHudElementCount;
        Notify(L"Could not create the crosshair colour picker thread");
        return;
    }
    CloseHandle(thread);
    Notify(L"Windows colour picker opened (Alt+Tab if hidden by fullscreen)");
}
void AdjustCrosshairRow(int row, int direction) {
    const auto old = g_crosshair;
    const int delta = direction < 0 ? -1 : 1;
    switch (row) {
        case 0: g_crosshair.style = (g_crosshair.style + delta + 6) % 6; break;
        case 1: s_crosshair_use_windows_picker = !s_crosshair_use_windows_picker; break;
        case 2: g_crosshair.size = std::clamp(g_crosshair.size + delta, 1, 100); break;
        case 3: if (g_crosshair.style == 1) break; g_crosshair.thickness = std::clamp(g_crosshair.thickness + delta, 1, 20); break;
        case 4: if (g_crosshair.style == 1 || g_crosshair.style == 5) break; g_crosshair.gap = std::clamp(g_crosshair.gap + delta, 0, 50); break;
        case 5: g_crosshair.opacity = std::clamp(g_crosshair.opacity + delta * 5, 0, 100); break;
        default: break;
    }
    if (old != g_crosshair) {
        if (row == 4 && g_crosshair.style == 0) g_crosshair.style = 2;
        else if (row >= 2 && row <= 5) EnableCrosshairEditing(g_crosshair);
        g_crosshair_status = CrosshairStatus::Pending;
        SaveSettings();
    }
}
std::vector<std::wstring> CrosshairEntries() {
    constexpr const wchar_t* styles[] = {L"Game default", L"Dot", L"Cross", L"Cross + dot", L"Off", L"Diamond"};
    return {L"Crosshair: " + std::wstring(styles[g_crosshair.style]) + L"  < >" +
            (g_crosshair.style ? CrosshairStatusLabel() : L""),
        CrosshairColourLabel(),
        L"Size: " + std::to_wstring(g_crosshair.size) + L" px  < >",
        g_crosshair.style == 1 ? L"Thickness: not used for solid dot" :
            L"Thickness: " + std::to_wstring(g_crosshair.thickness) + L" px  < >",
        (g_crosshair.style == 1 || g_crosshair.style == 5) ? L"Gap: not used for this shape" :
            L"Gap: " + std::to_wstring(g_crosshair.gap) + L" px  < >",
        L"Opacity: " + std::to_wstring(g_crosshair.opacity) + L"%  < >",
        L"Restore default crosshair", L"Back"};
}
void AdjustHudTextSize(int direction) {
    if (s_hud_style_element >= kHudElementCount) return;
    const int old_size = g_hud_size_percent[s_hud_style_element];
    const int size = std::clamp(old_size >= kHudMinSizePercent &&
        old_size <= kHudMaxSizePercent ? old_size : 100,
        kHudMinSizePercent, kHudMaxSizePercent);
    const int next = std::clamp(size + (direction < 0 ? -kHudSizeStepPercent : kHudSizeStepPercent),
                                kHudMinSizePercent, kHudMaxSizePercent);
    if (next != old_size) {
        g_hud_size_percent[s_hud_style_element] = next;
        SaveSettings();
    }
}
void ResetHudElementStyle() {
    if (s_hud_style_element >= kHudElementCount) return;
    g_hud_colour_modes[s_hud_style_element] = 0;
    g_hud_custom_colours[s_hud_style_element] = 0xFFFFFFu;
    g_hud_size_percent[s_hud_style_element] = 100;
    g_hud_corners[s_hud_style_element] = kDefaultHudCorners[s_hud_style_element];
    g_hud_show_headers[s_hud_style_element] = kDefaultHudShowHeaders[s_hud_style_element];
    SaveSettings();
    Notify(L"Selected HUD element customisations restored");
}
std::wstring HudCustomColourMethodLabel() {
    if (s_hud_style_element >= kHudElementCount) return L"Custom colour";
    if (s_tile_hex_axis == 9)
        return L"Custom colour: HEX #" + s_tile_hex_input +
               L"_  [Enter save / Esc cancel]";
    if (s_hud_use_windows_picker[s_hud_style_element])
        return L"Custom colour: Windows picker  < >  [Enter to open]";
    return L"Custom colour: HEX #" +
        HudRgbString(g_hud_custom_colours[s_hud_style_element]) +
        L"  < >  [Enter to edit]";
}

void OpenHudCustomColourMethod() {
    if (s_hud_style_element >= kHudElementCount) return;
    if (s_hud_use_windows_picker[s_hud_style_element]) {
        OpenHudColourPicker();
    } else {
        s_tile_hex_axis = 9;
        s_tile_hex_input.clear();
        Notify(L"Type 6 HEX digits (RRGGBB), Enter to save / Esc to cancel");
    }
}

void CycleHudCustomColourMethod() {
    if (s_hud_style_element >= kHudElementCount) return;
    auto& use_windows_picker = s_hud_use_windows_picker[s_hud_style_element];
    use_windows_picker = !use_windows_picker;
}
// UI-only method selection: False = HEX input, True = Windows picker.
// Selected and unselected tiles remember their methods independently.
bool s_tile_use_windows_picker[2] = {false, false};

void CycleTileColourMethod(bool selected) {
    const size_t axis = selected ? 0 : 1;
    s_tile_use_windows_picker[axis] = !s_tile_use_windows_picker[axis];
}

void StartTileHexEdit(bool selected) {
    s_tile_hex_axis = selected ? 0 : 1;
    s_tile_hex_input.clear();
    Notify(L"Type six HEX digits (RRGGBB), Enter to apply, Esc to cancel");
}

std::wstring TileColourMethodLabel(bool selected) {
    const std::wstring prefix = std::wstring(selected ? L"Selected" : L"Unselected") +
                                L" custom colour: ";
    if (s_tile_hex_axis == (selected ? 0 : 1))
        return prefix + L"HEX #" + s_tile_hex_input +
               L"_  [Enter save / Esc cancel]";
    if (s_tile_use_windows_picker[selected ? 0 : 1])
        return prefix + L"Windows picker  < >  [Enter to open]";
    return prefix + L"HEX " + GetTileHexColour(selected) +
           L"  < >  [Enter to edit]";
}

void OpenSelectedTileColourMethod(bool selected) {
    if (s_tile_use_windows_picker[selected ? 0 : 1])
        OpenTileColourPicker(selected);
    else
        StartTileHexEdit(selected);
}

// Both wall states independently choose HEX input or the Windows picker.
bool s_wall_use_windows_picker[2] = {false, false};

std::wstring WallColourMethodLabel(bool selected) {
    const std::wstring prefix = std::wstring(selected ? L"Selected" : L"Unselected") +
                                L" custom wall colour: ";
    if (s_tile_hex_axis == (selected ? 2 : 3))
        return prefix + L"HEX #" + s_tile_hex_input +
               L"_  [Enter save / Esc cancel]";
    return s_wall_use_windows_picker[selected ? 0 : 1]
        ? prefix + L"Windows picker  < >  [Enter to open]"
        : prefix + L"HEX " + GetWallHexColour(selected) +
              L"  < >  [Enter to edit]";
}

void OpenWallColourMethod(bool selected) {
    if (s_wall_use_windows_picker[selected ? 0 : 1]) OpenWallColourPicker(selected);
    else {
        s_tile_hex_axis = selected ? 2 : 3;
        s_tile_hex_input.clear();
        Notify(L"Type six HEX digits (RRGGBB), Enter to apply, Esc to cancel");
    }
}

bool s_tube_use_windows_picker = false;
std::wstring TubeColourMethodLabel() {
    if (s_tile_hex_axis == 4)
        return L"Custom tube colour: HEX #" + s_tile_hex_input +
               L"_  [Enter save / Esc cancel]";
    return s_tube_use_windows_picker
        ? L"Custom tube colour: Windows picker  < >  [Enter to open]"
        : L"Custom tube colour: HEX " + GetTubeCustomColourHex() +
          L"  < >  [Enter to edit]";
}
bool s_pillar_use_windows_picker = false;
std::wstring PillarColourMethodLabel() {
    if (s_tile_hex_axis == 15)
        return L"Custom pillar colour: HEX #" + s_tile_hex_input +
            L"_  [Enter save / Esc cancel]";
    return s_pillar_use_windows_picker
        ? L"Custom pillar colour: Windows picker  < >  [Enter to open]"
        : L"Custom pillar colour: HEX " + GetPillarColourHex() +
          L"  < >  [Enter to edit]";
}
void OpenPillarColourMethod() {
    if (s_pillar_use_windows_picker) OpenPillarColourPicker();
    else {
        s_tile_hex_axis = 15;
        s_tile_hex_input.clear();
        Notify(L"Type six HEX digits (RRGGBB), Enter to apply, Esc to cancel");
    }
}
bool s_platform_use_windows_picker = false;
std::wstring PlatformColourMethodLabel() {
    if (s_tile_hex_axis == 5)
        return L"Custom platform colour: HEX #" + s_tile_hex_input +
               L"_  [Enter save / Esc cancel]";
    return s_platform_use_windows_picker
        ? L"Custom platform colour: Windows picker  < >  [Enter to open]"
        : L"Custom platform colour: HEX " + GetPlatformCustomColourHex() +
          L"  < >  [Enter to edit]";
}
void OpenPlatformColourMethod() {
    if (s_platform_use_windows_picker) OpenPlatformColourPicker();
    else {
        s_tile_hex_axis = 5;
        s_tile_hex_input.clear();
        Notify(L"Type six HEX digits (RRGGBB), Enter to apply, Esc to cancel");
    }
}
bool s_water_use_windows_picker = false;
// The same palette is reachable from Customisation and World > Water.
// Back returns to the page that opened it without changing the normal route.
Page s_water_colour_return_page = Page::Customisation;
std::wstring WaterColourMethodLabel() {
    if (s_tile_hex_axis == 7)
        return L"Custom water colour: HEX #" + s_tile_hex_input +
               L"_  [Enter save / Esc cancel]";
    return s_water_use_windows_picker
        ? L"Custom water colour: Windows picker  < >  [Enter to open]"
        : L"Custom water colour: HEX " + GetWaterCustomColourHex() +
          L"  < >  [Enter to edit]";
}
bool s_underwater_use_windows_picker = false;
std::wstring UnderwaterColourMethodLabel() {
    if (s_tile_hex_axis == 8)
        return L"Custom underwater colour: HEX #" + s_tile_hex_input +
               L"_  [Enter save / Esc cancel]";
    return s_underwater_use_windows_picker
        ? L"Custom underwater colour: Windows picker  < >  [Enter to open]"
        : L"Custom underwater colour: HEX " + GetUnderwaterCustomColourHex() +
          L"  < >  [Enter to edit]";
}
void AdjustWaterColourIntensity(int direction) {
    const int current = GetWaterColourIntensityPercent();
    constexpr int64_t kStep = 5;
    const int64_t next = (std::max)(int64_t{0},
        static_cast<int64_t>(current) + (direction < 0 ? -kStep : kStep));
    if (next > (std::numeric_limits<int>::max)()) return;
    if (next != current)
        SetWaterColourIntensityPercent(static_cast<int>(next));
}
void AdjustUnderwaterColourIntensity(int direction) {
    const int current = GetUnderwaterColourIntensityPercent();
    // No arbitrary maximum. Do arithmetic in int64_t so holding Right
    // cannot overflow the saved int percentage and wrap into negatives.
    constexpr int64_t kStep = 5;
    const int64_t next = (std::max)(int64_t{0},
        static_cast<int64_t>(current) + (direction < 0 ? -kStep : kStep));
    if (next > (std::numeric_limits<int>::max)()) return;
    if (next != current)
        SetUnderwaterColourIntensityPercent(static_cast<int>(next));
}
void OpenUnderwaterColourMethod() {
    if (s_underwater_use_windows_picker) OpenUnderwaterColourPicker();
    else {
        s_tile_hex_axis = 8;
        s_tile_hex_input.clear();
        Notify(L"Type six HEX digits (RRGGBB), Enter to apply, Esc to cancel");
    }
}
void OpenWaterColourMethod() {
    if (s_water_use_windows_picker) OpenWaterColourPicker();
    else {
        s_tile_hex_axis = 7;
        s_tile_hex_input.clear();
        Notify(L"Type six HEX digits (RRGGBB), Enter to apply, Esc to cancel");
    }
}
bool s_sky_use_windows_picker[4] = {false, false, false, false};
int SkyHexInputAxis(SkyRegion region) {
    const int index = static_cast<int>(region);
    return index == 3 ? 16 : 10 + index;
}
std::wstring SkyColourMethodLabel(SkyRegion region) {
    const int i = static_cast<int>(region);
    const std::wstring label = i == 0 ? L"Zenith" : i == 1 ? L"Horizon" :
                               i == 2 ? L"Sun" : L"Clouds";
    if (s_tile_hex_axis == SkyHexInputAxis(region))
        return label + L" colour: HEX #" + s_tile_hex_input +
            L"_ [Enter save / Esc cancel]";
    return s_sky_use_windows_picker[i]
        ? label + L" colour: Windows picker  < >  [Enter to open]"
        : label + L" colour: HEX " + GetSkyColourHex(region) +
          L"  < >  [Enter to edit]";
}
void OpenSkyColourMethod(SkyRegion region) {
    const int i = static_cast<int>(region);
    if (s_sky_use_windows_picker[i]) OpenSkyColourPicker(region);
    else {
        s_tile_hex_axis = SkyHexInputAxis(region);
        s_tile_hex_input.clear();
        Notify(L"Type six HEX digits (RRGGBB), Enter to apply, Esc to cancel");
    }
}
bool s_fog_use_windows_picker[2] = {false, false};
std::wstring FogColourMethodLabel(FogRegion region) {
    const int i = static_cast<int>(region);
    const std::wstring label = i == 0 ? L"Fog haze" : L"Directional fog";
    if (s_tile_hex_axis == 13 + i)
        return label + L": HEX #" + s_tile_hex_input +
            L"_ [Enter save / Esc cancel]";
    return s_fog_use_windows_picker[i]
        ? label + L": Windows picker  < >  [Enter to open]"
        : label + L": HEX " + GetFogColourHex(region) + L"  < >  [Enter to edit]";
}
void OpenFogColourMethod(FogRegion region) {
    const int i = static_cast<int>(region);
    if (s_fog_use_windows_picker[i]) OpenFogColourPicker(region);
    else {
        s_tile_hex_axis = 13 + i;
        s_tile_hex_input.clear();
        Notify(L"Type six HEX digits (RRGGBB), Enter to apply, Esc to cancel");
    }
}
bool s_shadow_use_windows_picker = false;
std::wstring ShadowAmbientMethodLabel() {
    if (s_tile_hex_axis == 17)
        return L"Shadow ambient: HEX #" + s_tile_hex_input +
            L"_ [Enter save / Esc cancel]";
    return s_shadow_use_windows_picker
        ? L"Shadow ambient: Windows picker  < >  [Enter to open]"
        : L"Shadow ambient: HEX " + GetShadowAmbientColourHex() +
          L"  < >  [Enter to edit]";
}
void OpenShadowAmbientMethod() {
    if (s_shadow_use_windows_picker) OpenShadowAmbientColourPicker();
    else {
        s_tile_hex_axis = 17;
        s_tile_hex_input.clear();
        Notify(L"Type six HEX digits (RRGGBB), Enter to apply, Esc to cancel");
    }
}
bool s_jumppad_use_windows_picker = false;
std::wstring JumpPadColourMethodLabel() {
    if (s_tile_hex_axis == 6)
        return L"Custom jump pad colour: HEX #" + s_tile_hex_input +
               L"_  [Enter save / Esc cancel]";
    return s_jumppad_use_windows_picker
        ? L"Custom jump pad colour: Windows picker  < >  [Enter to open]"
        : L"Custom jump pad colour: HEX " + GetJumpPadCustomColourHex() +
          L"  < >  [Enter to edit]";
}
void OpenJumpPadColourMethod() {
    if (s_jumppad_use_windows_picker) OpenJumpPadColourPicker();
    else {
        s_tile_hex_axis = 6;
        s_tile_hex_input.clear();
        Notify(L"Type six HEX digits (RRGGBB), Enter to apply, Esc to cancel");
    }
}
void OpenTubeColourMethod() {
    if (s_tube_use_windows_picker) {
        OpenTubeColourPicker();
    } else {
        s_tile_hex_axis = 4;
        s_tile_hex_input.clear();
        Notify(L"Type six HEX digits (RRGGBB), Enter to apply, Esc to cancel");
    }
}

void ProcessTileHexKey(int key) {
    if (key == VK_ESCAPE) { s_tile_hex_axis = -1; s_tile_hex_input.clear(); return; }
    if (key == VK_RETURN) {
        if (s_tile_hex_input.size() != 6) {
            Notify(L"Enter exactly six HEX digits, for example FF6600");
            return;
        }
        uint32_t hex = 0;
        for (wchar_t c : s_tile_hex_input) {
            const int value = c >= L'0' && c <= L'9' ? c - L'0' : c - L'A' + 10;
            hex = (hex << 4) | static_cast<uint32_t>(value);
        }
        if (s_tile_hex_axis == 9) {
            if (s_hud_style_element < kHudElementCount) {
                g_hud_custom_colours[s_hud_style_element] = hex;
                g_hud_colour_modes[s_hud_style_element] = 10;
                SaveSettings();
            }
        }
        else if (s_tile_hex_axis == 18) SetPBGhostColourRgb(hex);
        else if (s_tile_hex_axis == 19) { g_crosshair.rgb = static_cast<int>(hex); EnableCrosshairEditing(g_crosshair); SaveSettings(); }
        else if (s_tile_hex_axis == 10) SetSkyColourHex(SkyRegion::Zenith, hex);
        else if (s_tile_hex_axis == 11) SetSkyColourHex(SkyRegion::Horizon, hex);
        else if (s_tile_hex_axis == 12) SetSkyColourHex(SkyRegion::Sun, hex);
        else if (s_tile_hex_axis == 16) SetSkyColourHex(SkyRegion::Clouds, hex);
        else if (s_tile_hex_axis == 17) SetShadowAmbientColourHex(hex);
        else if (s_tile_hex_axis == 13) SetFogColourHex(FogRegion::General, hex);
        else if (s_tile_hex_axis == 14) SetFogColourHex(FogRegion::Directional, hex);
        else if (s_tile_hex_axis == 15) SetPillarColourHex(hex);
        else if (s_tile_hex_axis == 8) SetUnderwaterCustomColourHex(hex);
        else if (s_tile_hex_axis == 7) SetWaterCustomColourHex(hex);
        else if (s_tile_hex_axis == 6) SetJumpPadCustomColourHex(hex);
        else if (s_tile_hex_axis == 5) SetPlatformCustomColourHex(hex);
        else if (s_tile_hex_axis == 4) SetTubeCustomColourHex(hex);
        else if (s_tile_hex_axis == 2 || s_tile_hex_axis == 3)
            SetWallHexColour(s_tile_hex_axis == 2, hex);
        else SetTileHexColour(s_tile_hex_axis == 0, hex);
        s_tile_hex_axis = -1;
        s_tile_hex_input.clear();
        return;
    }
    if (key == VK_BACK) {
        if (!s_tile_hex_input.empty()) s_tile_hex_input.pop_back();
        return;
    }
    if (s_tile_hex_input.size() >= 6) return;
    if (key >= '0' && key <= '9')
        s_tile_hex_input.push_back(static_cast<wchar_t>(key));
    else if (key >= 'A' && key <= 'F')
        s_tile_hex_input.push_back(static_cast<wchar_t>(key));
    else if (key >= VK_NUMPAD0 && key <= VK_NUMPAD9)
        s_tile_hex_input.push_back(static_cast<wchar_t>(L'0' + key - VK_NUMPAD0));
}

// Same in-menu HEX editor and Win32 picker methods as Tile/Wall Colours.
// 0 = unpressed centre, 1 = pressed centre,
// 2 = unpressed border, 3 = pressed border, 4 = beacon.
// These are menu-only selectors; no button progression or collision is touched.
int s_button_hex_role = -1;
std::wstring s_button_hex_input;
bool s_button_picker_method[5] = {false, false, false, false, false};

std::wstring ButtonColourMethodLabel(int role) {
    if (role < 0 || role > 4) return L"";
    const std::wstring label = role == 4 ? L"Beacon colour: " :
        (role == 3 ? L"Pressed border colour: " :
        (role == 2 ? L"Unpressed border colour: " :
        (role == 1 ? L"Pressed centre colour: " : L"Unpressed centre colour: ")));
    if (s_button_hex_role == role)
        return label + L"HEX #" + s_button_hex_input +
               L"_  [Enter save / Esc cancel]";
    if (s_button_picker_method[role])
        return label + L"Windows picker  < >  [Enter to open]";
    return label + L"HEX " +
        (role == 4 ? GetButtonBeaconColourHex() :
            (role >= 2 ? GetButtonBorderColourHex(role == 3) :
                         GetButtonCustomColourHex(role == 1))) +
        L"  < >  [Enter to edit]";
}
void OpenButtonColourMethod(int role) {
    if (role < 0 || role > 4) return;
    if (s_button_picker_method[role]) {
        if (role == 4) OpenButtonBeaconColourPicker();
        else if (role >= 2) OpenButtonBorderColourPicker(role == 3);
        else OpenButtonColourPicker(role == 1);
        return;
    }
    s_button_hex_role = role;
    s_button_hex_input.clear();
    Notify(L"Type six HEX digits (RRGGBB), Enter to apply, Esc to cancel");
}
void HandleButtonHexKey(int key) {
    if (key == VK_ESCAPE) {
        s_button_hex_role = -1;
        s_button_hex_input.clear();
        return;
    }
    if (key == VK_BACK) {
        if (!s_button_hex_input.empty()) s_button_hex_input.pop_back();
        return;
    }
    if (key == VK_RETURN) {
        if (s_button_hex_input.size() != 6) {
            Notify(L"Enter exactly six HEX digits, for example FF4080");
            return;
        }
        uint32_t rgb = 0;
        for (const wchar_t c : s_button_hex_input) {
            const uint32_t digit = c <= L'9' ? c - L'0' : c - L'A' + 10;
            rgb = (rgb << 4) | digit;
        }
        const int role = s_button_hex_role;
        s_button_hex_role = -1;
        s_button_hex_input.clear();
        if (role == 4) SetButtonBeaconColourHex(rgb);
        else if (role >= 2) SetButtonBorderColourHex(role == 3, rgb);
        else SetButtonCustomColourHex(role == 1, rgb);
        return;
    }
    if (s_button_hex_input.size() >= 6) return;
    if ((key >= '0' && key <= '9') || (key >= 'A' && key <= 'F'))
        s_button_hex_input.push_back(static_cast<wchar_t>(key));
    else if (key >= VK_NUMPAD0 && key <= VK_NUMPAD9)
        s_button_hex_input.push_back(static_cast<wchar_t>(L'0' + key - VK_NUMPAD0));
}

int s_coordinate_axis = -1;
std::wstring s_coordinate_input;

float* TeleportAxis(int axis) {
    switch (axis) {
        case 0: return &g_teleport_target.x;
        case 1: return &g_teleport_target.y;
        case 2: return &g_teleport_target.z;
        default: return nullptr;
    }
}

std::wstring TeleportAxisLabel(int axis, const wchar_t* name) {
    if (g_coordinate_editing.load(std::memory_order_relaxed) && s_coordinate_axis == axis)
        return std::wstring(name) + L": " + s_coordinate_input +
               L"_  [Enter save / Esc cancel]";
    const float* value = TeleportAxis(axis);
    return std::wstring(name) + L": " + (value ? CheckpointFloat(*value) : L"0") +
           L"  [Enter to edit]";
}

void StartCoordinateEdit(int axis) {
    if (!TeleportAxis(axis)) return;
    s_coordinate_axis = axis;
    s_coordinate_input.clear();  // Replace the current coordinate with typed input.
    g_coordinate_editing.store(true, std::memory_order_relaxed);
    Notify(L"Type a coordinate (- and . supported); Enter to save");
}

void FinishCoordinateEdit(bool commit) {
    if (commit) {
        wchar_t* end = nullptr;
        errno = 0;
        const float value = std::wcstof(s_coordinate_input.c_str(), &end);
        if (s_coordinate_input.empty() || end == s_coordinate_input.c_str() ||
            *end != L'\0' || errno == ERANGE || !std::isfinite(value) ||
            std::abs(value) > 100000.0f) {
            Notify(L"Invalid coordinate: enter a number between -100000 and 100000");
            return;  // Stay in the editor so the user can correct the input.
        }
        *TeleportAxis(s_coordinate_axis) = value;
        g_teleport_target_initialized = true;
        SaveSettings();
    }
    s_coordinate_input.clear();
    s_coordinate_axis = -1;
    g_coordinate_editing.store(false, std::memory_order_relaxed);
}

void ProcessCoordinateEditKey(int key) {
    if (key == VK_ESCAPE) { FinishCoordinateEdit(false); return; }
    if (key == VK_RETURN) { FinishCoordinateEdit(true); return; }
    if (key == VK_BACK) {
        if (!s_coordinate_input.empty()) s_coordinate_input.pop_back();
        return;
    }
    if (s_coordinate_input.size() >= 20) return;
    wchar_t ch = 0;
    if (key >= '0' && key <= '9') ch = static_cast<wchar_t>(key);
    else if (key >= VK_NUMPAD0 && key <= VK_NUMPAD9)
        ch = static_cast<wchar_t>(L'0' + key - VK_NUMPAD0);
    else if ((key == VK_OEM_MINUS || key == VK_SUBTRACT) && s_coordinate_input.empty())
        ch = L'-';
    else if ((key == VK_OEM_PERIOD || key == VK_DECIMAL) &&
             s_coordinate_input.find(L'.') == std::wstring::npos)
        ch = L'.';
    if (ch) s_coordinate_input.push_back(ch);
}

bool SeedTeleportTargetFromPlayer(MyCharacter* player) {
    if (!player || !IsReadable(player, sizeof(*player)) ||
        !IsReadable(player->root_component, sizeof(SceneComponent)))
        return false;
    const Vec3 position = player->root_component->location;
    if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
        !std::isfinite(position.z) || std::abs(position.x) > 100000.0f ||
        std::abs(position.y) > 100000.0f || std::abs(position.z) > 100000.0f)
        return false;
    g_teleport_target = position;
    g_teleport_target_initialized = true;
    SaveSettings();
    return true;
}

const wchar_t* ProgressDisplayModeLabel(ProgressDisplayMode mode) {
    switch (mode) {
        case ProgressDisplayMode::Off: return L"Off";
        case ProgressDisplayMode::Total: return L"Total";
        case ProgressDisplayMode::Loaded: return L"Loaded";
    }
    return L"Off";
}

const wchar_t* HudCornerLabel(HudCorner corner) {
    switch (corner) {
        case HudCorner::TopRight: return L"Top right";
        case HudCorner::TopLeft: return L"Top left";
        case HudCorner::BottomRight: return L"Bottom right";
        case HudCorner::BottomLeft: return L"Bottom left";
        case HudCorner::Center: return L"Center";
    }
    return L"Top left";
}

const wchar_t* MenuPositionLabel(MenuPosition position) {
    switch (position) {
        case MenuPosition::TopRight: return L"Top right";
        case MenuPosition::TopLeft: return L"Top left";
        case MenuPosition::BottomRight: return L"Bottom right";
        case MenuPosition::BottomLeft: return L"Bottom left";
    }
    return L"Top right";
}

void CycleMenuPosition(int direction) {
    constexpr int count = 4;
    const int current = static_cast<int>(g_menu_position);
    const int offset = (current >= 1 && current <= 4) ? current - 1 : 0;
    g_menu_position = static_cast<MenuPosition>(
        1 + (offset + (direction < 0 ? count - 1 : 1)) % count);
    SaveSettings();
}


// Persistent side-screen elements in the exact saved HudElement order.
// Do not reorder the enum: the position/appearance keys must stay compatible.
bool HudElementIsVisible(HudElement element) {
    switch (element) {
        case HudElement::Timer: return g_show_timer;
        case HudElement::PBRun: return g_show_pb_run_hud;
        case HudElement::SumOfBest: return g_show_sum_of_best_hud;
        case HudElement::LastButtonSplit: return g_show_last_button_split_hud;
        case HudElement::Coordinates: return g_show_coordinates;
        case HudElement::Cubes: return g_cube_display_mode != ProgressDisplayMode::Off;
        case HudElement::Tiles: return g_tile_display_mode != ProgressDisplayMode::Off;
        case HudElement::FPS: return g_show_fps;
        case HudElement::EnabledModules: return g_show_enabled_modules;
        case HudElement::PracticeStatus: return g_show_practice_status_hud;
        case HudElement::RecordStatus: return g_show_record_status_hud;
        case HudElement::Speed: return g_show_speed;
        case HudElement::CompareTime: return g_show_compare_hud;
        case HudElement::BestSegmentTime: return g_show_best_segment_time_hud;
        case HudElement::PBSplitTime: return g_show_pb_split_time_hud;
        case HudElement::CompletionPercent: return g_show_completion_percent;
        case HudElement::ActiveCheats: return g_show_active_cheats;
        case HudElement::ActiveCustomisations: return g_show_active_customisations;
        case HudElement::GameTime: return g_show_game_time_hud;
        case HudElement::LocalTime: return g_show_local_time_hud;
        case HudElement::ButtonNumber: return g_show_button_number;
        case HudElement::KeyDisplay: return g_show_key_display;
        case HudElement::Count: break;
    }
    return false;
}

void ToggleHudElement(HudElement element) {
    switch (element) {
        case HudElement::Timer: g_show_timer = !g_show_timer; break;
        case HudElement::PBRun: g_show_pb_run_hud = !g_show_pb_run_hud; break;
        case HudElement::SumOfBest: g_show_sum_of_best_hud = !g_show_sum_of_best_hud; break;
        case HudElement::LastButtonSplit: g_show_last_button_split_hud = !g_show_last_button_split_hud; break;
        case HudElement::Coordinates: g_show_coordinates = !g_show_coordinates; break;
        case HudElement::Cubes:
            g_cube_display_mode = g_cube_display_mode == ProgressDisplayMode::Off
                ? ProgressDisplayMode::Total : ProgressDisplayMode::Off; break;
        case HudElement::Tiles:
            g_tile_display_mode = g_tile_display_mode == ProgressDisplayMode::Off
                ? ProgressDisplayMode::Total : ProgressDisplayMode::Off; break;
        case HudElement::FPS: g_show_fps = !g_show_fps; break;
        case HudElement::EnabledModules: g_show_enabled_modules = !g_show_enabled_modules; break;
        case HudElement::PracticeStatus: g_show_practice_status_hud = !g_show_practice_status_hud; break;
        case HudElement::RecordStatus: g_show_record_status_hud = !g_show_record_status_hud; break;
        case HudElement::Speed: g_show_speed = !g_show_speed; break;
        case HudElement::CompareTime: g_show_compare_hud = !g_show_compare_hud; break;
        case HudElement::BestSegmentTime: g_show_best_segment_time_hud = !g_show_best_segment_time_hud; break;
        case HudElement::PBSplitTime: g_show_pb_split_time_hud = !g_show_pb_split_time_hud; break;
        case HudElement::CompletionPercent: g_show_completion_percent = !g_show_completion_percent; break;
        case HudElement::ActiveCheats: g_show_active_cheats = !g_show_active_cheats; break;
        case HudElement::ActiveCustomisations: g_show_active_customisations = !g_show_active_customisations; break;
        case HudElement::GameTime: g_show_game_time_hud = !g_show_game_time_hud; break;
        case HudElement::LocalTime: g_show_local_time_hud = !g_show_local_time_hud; break;
        case HudElement::ButtonNumber: g_show_button_number = !g_show_button_number; break;
        case HudElement::KeyDisplay: g_show_key_display = !g_show_key_display; break;
        case HudElement::Count: return;
    }
    SaveSettings();
}

// Views for count and record elements are independent of their visibility.
std::wstring HudElementView(HudElement element) {
    if (element == HudElement::Cubes)
        return g_cube_display_mode == ProgressDisplayMode::Loaded ? L"Loaded only" : L"Whole map";
    if (element == HudElement::Tiles)
        return g_tile_display_mode == ProgressDisplayMode::Loaded ? L"Loaded only" : L"Whole map";
    const RecordTimeView* view = nullptr;
    if (element == HudElement::CompareTime) view = &g_compare_hud_view;
    else if (element == HudElement::BestSegmentTime) view = &g_best_segment_time_view;
    else if (element == HudElement::PBSplitTime) view = &g_pb_split_time_view;
    return view ? (*view == RecordTimeView::Total ? L"Cumulative total" : L"Single button") : L"";
}

bool HudElementHasView(HudElement element) {
    return element == HudElement::Cubes || element == HudElement::Tiles ||
        element == HudElement::CompareTime || element == HudElement::BestSegmentTime ||
        element == HudElement::PBSplitTime;
}

void ToggleHudElementView(HudElement element) {
    if (element == HudElement::Cubes) {
        g_cube_display_mode = g_cube_display_mode == ProgressDisplayMode::Loaded
            ? ProgressDisplayMode::Total : ProgressDisplayMode::Loaded;
    } else if (element == HudElement::Tiles) {
        g_tile_display_mode = g_tile_display_mode == ProgressDisplayMode::Loaded
            ? ProgressDisplayMode::Total : ProgressDisplayMode::Loaded;
    } else {
        RecordTimeView* view = nullptr;
        if (element == HudElement::CompareTime) view = &g_compare_hud_view;
        else if (element == HudElement::BestSegmentTime) view = &g_best_segment_time_view;
        else if (element == HudElement::PBSplitTime) view = &g_pb_split_time_view;
        if (!view) return;
        *view = *view == RecordTimeView::Total ? RecordTimeView::Segment : RecordTimeView::Total;
    }
    SaveSettings();
}

void ToggleHudHeader() {
    if (s_hud_style_element >= kHudElementCount) return;
    auto& value = g_hud_show_headers[s_hud_style_element];
    value = !value;
    SaveSettings();
}

void CycleHudCorner(size_t index, int direction) {
    if (index >= kHudElementCount) return;
    constexpr int count = static_cast<int>(kHudPlacementCount);
    const int stored = static_cast<int>(g_hud_corners[index]);
    const int current = stored >= 0 && stored < count
        ? stored : static_cast<int>(kDefaultHudCorners[index]);
    g_hud_corners[index] = static_cast<HudCorner>(
        (current + (direction < 0 ? count - 1 : 1)) % count);
    SaveSettings();
}

void CycleProgressDisplayMode(ProgressDisplayMode& mode, int direction) {
    constexpr int mode_count = 3;
    mode = static_cast<ProgressDisplayMode>(
        (static_cast<int>(mode) + (direction < 0 ? mode_count - 1 : 1)) % mode_count);
    SaveSettings();
}

// Reuse the existing visibility flag and time-view setting so older
// settings.ini files remain compatible. The UI exposes one three-way selector:
// Off -> Total time -> Segment time -> Off.
const wchar_t* RecordTimeHudModeLabel(bool enabled, RecordTimeView view) {
    if (!enabled) return L"Off";
    return view == RecordTimeView::Total ? L"Total time" : L"Segment time";
}

void CycleRecordTimeHudMode(bool& enabled, RecordTimeView& view, int direction) {
    constexpr int mode_count = 3;
    const int current = !enabled ? 0 : (view == RecordTimeView::Total ? 1 : 2);
    const int next = (current + (direction < 0 ? mode_count - 1 : 1)) % mode_count;
    enabled = next != 0;
    if (next != 0)
        view = next == 1 ? RecordTimeView::Total : RecordTimeView::Segment;
    SaveSettings();
}

const wchar_t* SplitRowModeLabel(SplitRowMode mode) {
    switch (mode) {
        case SplitRowMode::HeaderAndTime: return L"Header + time";
        case SplitRowMode::TimeOnly: return L"Time only";
        case SplitRowMode::Off: return L"Off";
    }
    return L"Off";
}

void ChangeSplitRowMode(SplitRowMode& mode, int direction) {
    mode = CycleSplitRowMode(mode, direction);
    SaveSettings();
}

// One five-way selector replaces the old independent label/visibility/time
// controls. Keep the existing INI flags so previous preferences migrate without
// changing any recording logic or the HUD renderer.
// 0 = Header + time, 1 = Time only, 2 = Header + sign,
// 3 = Sign only, 4 = Off.
int FastestDeltaMode() {
    if (!g_show_split_fastest_delta) return 4;
    if (g_show_fastest_delta_label)
        return g_show_fastest_delta_time ? 0 : 2;
    return g_show_fastest_delta_time ? 1 : 3;
}

const wchar_t* FastestDeltaModeLabel() {
    switch (FastestDeltaMode()) {
        case 0: return L"Header + time";
        case 1: return L"Time only";
        case 2: return L"Header + sign";
        case 3: return L"Sign only";
        default: return L"Off";
    }
}

const wchar_t* SectionCompareModeLabel(SectionCompareDisplayMode mode) {
    switch (mode) {
        case SectionCompareDisplayMode::HeaderAndTime: return L"Header + time";
        case SectionCompareDisplayMode::TimeOnly: return L"Time only";
        case SectionCompareDisplayMode::HeaderAndSign: return L"Header + sign";
        case SectionCompareDisplayMode::SignOnly: return L"Sign only";
        case SectionCompareDisplayMode::Off: return L"Off";
    }
    return L"Off";
}

void ChangeSectionCompareMode(SectionCompareDisplayMode& mode, int direction) {
    mode = CycleSectionCompareDisplayMode(mode, direction);
    SaveSettings();
}

const wchar_t* CheatRecordModeLabel() {
    switch (g_cheat_record_mode) {
        case CheatRecordMode::Off: return L"Off";
        case CheatRecordMode::PBOnly: return L"PB only";
        case CheatRecordMode::SegmentsOnly: return L"Fastest segments only";
        case CheatRecordMode::Both: return L"Both";
    }
    return L"Off";
}

const wchar_t* CompareModeLabel() {
    return g_compare_mode == ProfileCompareMode::BestSegments
        ? L"Best splits (cumulative)" : L"PB splits";
}

void CycleCompareMode() {
    g_compare_mode = g_compare_mode == ProfileCompareMode::PersonalBest
        ? ProfileCompareMode::BestSegments : ProfileCompareMode::PersonalBest;
    SaveSettings();
}

void CycleCheatRecordMode(int direction) {
    g_cheat_record_mode = NextCheatRecordMode(g_cheat_record_mode, direction);
    SaveSettings();
    Notify(L"Save with cheats: " + std::wstring(CheatRecordModeLabel()) +
           L" (PB permission applies next fresh run)");
}

void CycleFastestDeltaMode(int direction) {
    constexpr int kModeCount = 5;
    const int next = (FastestDeltaMode() + (direction < 0 ? kModeCount - 1 : 1)) % kModeCount;
    g_show_split_fastest_delta = next != 4;
    g_show_fastest_delta_time = next == 0 || next == 1;
    g_show_fastest_delta_label = next == 0 || next == 2;
    SaveSettings();
}

// Match the five-way signed-difference selector used by Best Segment Difference.
// Preserve the existing popup ON/OFF key while extending its presentation.
int ComparePopupDisplayMode() {
    if (!g_show_compare_popup) return 4;
    if (g_compare_popup_show_header)
        return g_compare_popup_show_time ? 0 : 2;
    return g_compare_popup_show_time ? 1 : 3;
}

const wchar_t* ComparePopupDisplayLabel() {
    switch (ComparePopupDisplayMode()) {
        case 0: return L"Header + time";
        case 1: return L"Time only";
        case 2: return L"Header + sign";
        case 3: return L"Sign only";
        default: return L"Off";
    }
}

void CycleComparePopupDisplayMode(int direction) {
    constexpr int kModeCount = 5;
    const int next = (ComparePopupDisplayMode() + (direction < 0 ? kModeCount - 1 : 1)) % kModeCount;
    g_show_compare_popup = next != 4;
    g_compare_popup_show_time = next == 0 || next == 1;
    g_compare_popup_show_header = next == 0 || next == 2;
    SaveSettings();
}

} // namespace

// Only reset World gameplay settings that are actually modified. Avoid a full
// actor/material sweep during every vanilla restart (including speedruns).
// Visual customisations, PBs, and native progression are not persistence data.
bool WorldGameplayOptionsModified() {
    if (!g_animate_section_changes ||
        g_buttons_sync_with_sections || IsSectionSpeedModified() ||
        IsTubeTravelSpeedModified() || IsPlatformSpeedModified() ||
        IsJumpPadHeightModified() || IsWaterRemoved() || IsAllWaterRemoved() ||
        IsWaterSwimmingDisabled() || GetWaterHeightOffset() != 0 ||
        IsButtonTileSelectionDisabled() || IsTileSelectionOnContactDisabled() ||
        IsWorldCubeOverrideActive()) return true;
    for (auto kind : {WorldObjectKind::Cubes, WorldObjectKind::Buttons,
                      WorldObjectKind::Tiles, WorldObjectKind::Tubes,
                      WorldObjectKind::Platforms, WorldObjectKind::JumpPads}) {
        if (IsWorldObjectRemoved(kind)) return true;
    }
    for (int section = 1; section <= kPracticeClusterCount; ++section) {
        if (GetSectionPlacement(section) != SectionPlacement::Auto) return true;
        for (int axis = 0; axis < 3; ++axis) {
            if (GetSectionPositionOffset(section, axis) != 0.0f) return true;
        }
    }
    return false;
}

void ResetWorldGameplayForPersistence() {
    if (!WorldGameplayOptionsModified()) return;

    // Unlike the user-facing complete World reset, the persistence reset only
    // touches modified gameplay configuration. In particular it must NOT call
    // RestoreDefaultButtons(), touch native button progress, or rewrite
    // visual customisations / PBs on each map restart.
    if (IsJumpPadHeightModified()) ResetJumpPadHeight();
    if (IsPlatformSpeedModified()) ResetPlatformSpeed();
    if (IsTubeTravelSpeedModified()) ResetTubeTravelSpeed();
    if (IsSectionSpeedModified()) ResetSectionSpeed();

    bool removed_objects = false;
    for (auto kind : {WorldObjectKind::Cubes, WorldObjectKind::Buttons,
                      WorldObjectKind::Tiles, WorldObjectKind::Tubes,
                      WorldObjectKind::Platforms, WorldObjectKind::JumpPads}) {
        if (IsWorldObjectRemoved(kind)) removed_objects = true;
    }
    if (removed_objects) ResetWorldObjectRemovals();
    if (IsAllWaterRemoved()) SetAllWaterRemoved(false);
    if (IsWaterRemoved()) SetWaterRemoved(false);
    if (IsWaterSwimmingDisabled()) SetWaterSwimmingDisabled(false);
    if (GetWaterHeightOffset() != 0) SetWaterHeightOffset(0);

    g_animate_section_changes = true;
    if (g_buttons_sync_with_sections && !SetButtonsSyncWithSections(false))
        Log(L"World persistence: could not restore button sync; native state unavailable");

    bool placements_changed = false;
    bool positions_changed = false;
    for (int section = 1; section <= kPracticeClusterCount; ++section) {
        if (GetSectionPlacement(section) != SectionPlacement::Auto)
            placements_changed = true;
        for (int axis = 0; axis < 3; ++axis) {
            if (GetSectionPositionOffset(section, axis) != 0.0f)
                positions_changed = true;
        }
    }
    if (placements_changed) ResetAllSectionPlacements();
    if (positions_changed && !ResetAllSectionPositionOffsets())
        Log(L"World persistence: section position defaults could not be restored");

    if (IsButtonTileSelectionDisabled()) ToggleButtonTileSelection();
    if (IsTileSelectionOnContactDisabled()) ToggleTileSelectionOnContact();
    if (IsWorldCubeOverrideActive())
        RequestCubeWorldAction(CubeWorldAction::RestoreDefault, g_player);
    SaveSettings();
    NoteWorldProfilePersistenceReset();
}

// These are defined in refunct (not in the anonymous helper namespace).
// Keep the forward declarations in the same scope as the definitions so
// MSVC does not see distinct overloads with identical signatures.
std::vector<std::wstring> BuildEntriesForPage(Page page);
void RefreshMenuSearch();
void OpenMenuSearch();

void SetPage(Page page) {
    // Retired reflection submenu IDs now lead to the single reflections page.
    if (page == Page::WaterObjectReflections || page == Page::WaterBeamReflections)
        page = Page::WaterReflections;
    // A restart or external page change must never leave input capture active.
    if (page != Page::TeleportCoordinates &&
        g_coordinate_editing.load(std::memory_order_relaxed))
        FinishCoordinateEdit(false);
    g_page = page;
    s_menu_search_capture.store(page == Page::Search, std::memory_order_release);
    g_selection = 0;
}

void ExecuteAction(Action action, MyCharacter* player) {
    switch (action) {
        case Action::Menu:
            // F5 hides/shows the overlay without discarding the current
            // submenu or selected row. Explicit Back navigation still resets
            // the selection when changing pages.
            g_menu_open.store(!g_menu_open.load());
            g_rebinding.store(false);
            g_rebind_action = -1;
            break;
        case Action::Restart:
            g_restart_requested = true;
            break;
        case Action::RestartWithProgress:
            RequestProgressRestart();
            break;
        case Action::FullLevelReload:
            // Identical to the confirmed menu command, but immediately
            // requests it like the existing R/G/H restart hotkeys.
            // The native reload happens safely after the character tick.
            RequestFullLevelReload();
            break;
        case Action::PreloadedRestart:
            if (!g_preloaded_hook_available)
                Notify(L"Preloaded restart unavailable: native new-game hook not installed");
            else
                g_preloaded_restart_requested = true;
            break;
        case Action::Fly:
            g_restore_flying_pending = false;
            g_restore_noclip_pending = false;
            SetFlying(player, !g_flying);
            SaveMovementModeSettings();
            break;
        case Action::Noclip:
            g_restore_flying_pending = false;
            g_restore_noclip_pending = false;
            SetNoclip(player, !g_noclip);
            SaveMovementModeSettings();
            break;
        case Action::SavePosition:
            SavePosition(player);
            break;
        case Action::RestorePosition:
            RestorePosition(player);
            break;
        case Action::Coordinates:
            g_show_coordinates = !g_show_coordinates;
            SaveSettings();
            break;
        case Action::Timer:
            g_show_timer = !g_show_timer;
            SaveSettings();
            break;
        case Action::ReplaySection:
            RequestPractice(g_practice_selected_button);
            break;
        case Action::SaveState:
            SaveCurrentCheckpoint(player);
            break;
        case Action::LoadState:
            LoadSavedCheckpoint();
            break;
        case Action::LoadLastButton:
            LoadLastButtonCheckpoint();
            break;
        case Action::LoadSecondLastButton:
            LoadSecondLastButtonCheckpoint();
            break;
        case Action::SpeedUp:
        case Action::SpeedDown:
            g_fly_speed = std::clamp(g_fly_speed + (action == Action::SpeedUp ? 200.0f : -200.0f),
                                      200.0f, 10000.0f);
            SaveSettings();
            break;
        case Action::TeleportLastButtonGlobal:
            TeleportToLastPressedButton(player);
            break;
        case Action::TeleportSavedCoordinates:
            if (g_teleport_target_initialized)
                TeleportToCoordinates(player, g_teleport_target);
            else Notify(L"No saved XYZ target - enter coordinates in World / Run & Practice");
            break;
        case Action::ToggleAirJump: ToggleAirJump(); break;
        case Action::TogglePreserveMomentum: TogglePreserveMomentum(); break;
        case Action::CycleScrollJump: CycleJumpAssistMode(true, +1); break;
        case Action::CycleHoldJump: CycleJumpAssistMode(false, +1); break;
        case Action::ToggleInGameTimeHUD:
            g_show_game_time_hud = !g_show_game_time_hud; SaveSettings(); break;
        case Action::ToggleRealTimeHUD:
            g_show_local_time_hud = !g_show_local_time_hud; SaveSettings(); break;
        case Action::ToggleSpeedHUD:
            g_show_speed = !g_show_speed; SaveSettings(); break;
        case Action::ToggleFPSHUD:
            g_show_fps = !g_show_fps; SaveSettings(); break;
        case Action::ToggleCheatsHUD:
            g_show_active_cheats = !g_show_active_cheats; SaveSettings(); break;
        case Action::ToggleCustomisationsHUD:
            g_show_active_customisations = !g_show_active_customisations; SaveSettings(); break;
        case Action::ToggleEnabledModulesHUD:
            g_show_enabled_modules = !g_show_enabled_modules; SaveSettings(); break;
        case Action::ToggleCompletionHUD:
            g_show_completion_percent = !g_show_completion_percent; SaveSettings(); break;
        case Action::ToggleSplitPopups:
            g_show_splits = !g_show_splits; SaveSettings(); break;
        case Action::TogglePBRunHUD:
            g_show_pb_run_hud = !g_show_pb_run_hud; SaveSettings(); break;
        case Action::ToggleSumOfBestHUD:
            g_show_sum_of_best_hud = !g_show_sum_of_best_hud; SaveSettings(); break;
        case Action::ToggleLastSplitHUD:
            g_show_last_button_split_hud = !g_show_last_button_split_hud; SaveSettings(); break;
        case Action::ToggleTimeOverride:
            SetCustomTimeOfDayEnabled(!IsCustomTimeOfDayEnabled()); break;
        case Action::ToggleTimeLock:
            if (!IsCustomTimeOfDayEnabled()) {
                Notify(L"Enable Custom time first in Customisation / Time");
                break;
            }
            SetCustomTimeLocked(!IsCustomTimeLocked()); break;
        case Action::IncreaseTimeSpeed: AdjustCustomTimeSpeedPercent(+1); break;
        case Action::DecreaseTimeSpeed: AdjustCustomTimeSpeedPercent(-1); break;
        case Action::IncreaseGameTime: AdjustCustomTimeMinutes(+1); break;
        case Action::DecreaseGameTime: AdjustCustomTimeMinutes(-1); break;
        case Action::RefreshMaps:
            RefreshCustomMapFiles(); Notify(L"Maps list refreshed"); break;
        case Action::RestoreOriginalMap:
            RequestCustomMapRestore(); break;
        case Action::ToggleMenuDescriptions:
            g_show_menu_descriptions = !g_show_menu_descriptions; SaveSettings(); break;
        case Action::SelectAllCubes:
            RequestCubeWorldAction(CubeWorldAction::CollectAll, player); break;
        case Action::ToggleCubesHUD: ToggleHudElement(HudElement::Cubes); break;
        case Action::ToggleTilesHUD: ToggleHudElement(HudElement::Tiles); break;
        case Action::ToggleButtonNumberHUD: ToggleHudElement(HudElement::ButtonNumber); break;
        case Action::ToggleKeyDisplayHUD: ToggleHudElement(HudElement::KeyDisplay); break;
        case Action::TogglePracticeStatusHUD: ToggleHudElement(HudElement::PracticeStatus); break;
        case Action::ToggleRecordStatusHUD: ToggleHudElement(HudElement::RecordStatus); break;
        case Action::ToggleComparisonHUD: ToggleHudElement(HudElement::CompareTime); break;
        case Action::ToggleBestSegmentHUD: ToggleHudElement(HudElement::BestSegmentTime); break;
        case Action::TogglePBSplitHUD: ToggleHudElement(HudElement::PBSplitTime); break;
        case Action::TogglePBGhost: SetPBGhostEnabled(!IsPBGhostEnabled()); break;
        case Action::ToggleGhostMode:
            SetPBGhostPassThroughEnabled(!IsPBGhostPassThroughEnabled()); break;
        case Action::ToggleGhostFastestSections:
            SetPBGhostFastestSectionsEnabled(!IsPBGhostFastestSectionsEnabled()); break;
        case Action::ToggleGhostReplay:
            if (!StartGhostReplayViewer()) Notify(L"No saved ghost recording for this split profile");
            else Notify(IsGhostReplayRequested()
                ? L"Preparing native button replay"
                : L"Replay stopped; level will reset if buttons were pressed");
            break;
        case Action::ToggleGhostReplayPause: ToggleGhostReplayPause(); break;
        case Action::SelectAllTiles: SelectWorldTiles(true); break;
        case Action::Count: break;
    }
}

// Keep the legacy page-local entry point for existing menu consumers.
std::vector<std::wstring> BuildEntries() { return BuildEntriesForPage(g_page); }

bool CanClearHotkey(size_t action) {
    if (action != static_cast<size_t>(Action::Menu)) return true;
    for (size_t i = 0; i < kActionCount; ++i) {
        if (i != action && g_keys[i].load() == VK_F5) {
            Notify(L"Reassign F5 before clearing the menu hotkey");
            return false;
        }
    }
    return true;
}

std::wstring MenuHotkeyDisplayName() {
    return Hotkey(Action::Menu) == 0 ? L"F5 fallback" : KeyName(Hotkey(Action::Menu));
}

namespace {
struct HotkeyCategory {
    Page page;
    const wchar_t* title;
};
constexpr std::array<HotkeyCategory, 6> kHotkeyCategories{{
    {Page::HotkeysGeneral, L"General & Restarts"},
    {Page::HotkeysMovement, L"Movement & Jump"},
    {Page::HotkeysPractice, L"Saves & Practice"},
    {Page::HotkeysHUD, L"HUD & Timers"},
    {Page::HotkeysWorld, L"Time, Maps & World"},
    {Page::HotkeysGhost, L"Ghost & Replay"},
}};

// Every action has exactly one stable category, without changing saved action IDs.
constexpr Page HotkeyCategoryForAction(Action action) {
    switch (action) {
        case Action::Menu:
        case Action::Restart:
        case Action::PreloadedRestart:
        case Action::RestartWithProgress:
        case Action::FullLevelReload:
        case Action::ToggleMenuDescriptions:
            return Page::HotkeysGeneral;
        case Action::Fly:
        case Action::Noclip:
        case Action::SpeedUp:
        case Action::SpeedDown:
        case Action::ToggleAirJump:
        case Action::TogglePreserveMomentum:
        case Action::CycleScrollJump:
        case Action::CycleHoldJump:
            return Page::HotkeysMovement;
        case Action::SavePosition:
        case Action::RestorePosition:
        case Action::ReplaySection:
        case Action::SaveState:
        case Action::LoadState:
        case Action::LoadLastButton:
        case Action::LoadSecondLastButton:
        case Action::TeleportLastButtonGlobal:
        case Action::TeleportSavedCoordinates:
            return Page::HotkeysPractice;
        case Action::Coordinates:
        case Action::Timer:
        case Action::ToggleInGameTimeHUD:
        case Action::ToggleRealTimeHUD:
        case Action::ToggleSpeedHUD:
        case Action::ToggleFPSHUD:
        case Action::ToggleCheatsHUD:
        case Action::ToggleCustomisationsHUD:
        case Action::ToggleEnabledModulesHUD:
        case Action::ToggleCompletionHUD:
        case Action::ToggleSplitPopups:
        case Action::TogglePBRunHUD:
        case Action::ToggleSumOfBestHUD:
        case Action::ToggleLastSplitHUD:
        case Action::ToggleCubesHUD:
        case Action::ToggleTilesHUD:
        case Action::ToggleButtonNumberHUD:
        case Action::ToggleKeyDisplayHUD:
        case Action::TogglePracticeStatusHUD:
        case Action::ToggleRecordStatusHUD:
        case Action::ToggleComparisonHUD:
        case Action::ToggleBestSegmentHUD:
        case Action::TogglePBSplitHUD:
            return Page::HotkeysHUD;
        case Action::ToggleTimeOverride:
        case Action::ToggleTimeLock:
        case Action::IncreaseTimeSpeed:
        case Action::DecreaseTimeSpeed:
        case Action::IncreaseGameTime:
        case Action::DecreaseGameTime:
        case Action::RefreshMaps:
        case Action::RestoreOriginalMap:
        case Action::SelectAllCubes:
        case Action::SelectAllTiles:
            return Page::HotkeysWorld;
        case Action::TogglePBGhost:
        case Action::ToggleGhostMode:
        case Action::ToggleGhostFastestSections:
        case Action::ToggleGhostReplay:
        case Action::ToggleGhostReplayPause:
            return Page::HotkeysGhost;
        case Action::Count: break;
    }
    return Page::HotkeysGeneral;
}

bool IsHotkeyCategoryPage(Page page) {
    for (const auto& category : kHotkeyCategories)
        if (category.page == page) return true;
    return false;
}

// Map a visible row to its actual action ID. Never use the row index as an ID.
size_t HotkeyActionAtRow(Page page, int row) {
    if (!IsHotkeyCategoryPage(page) || row < 0) return kActionCount;
    for (size_t i = 0; i < kActionCount; ++i) {
        if (HotkeyCategoryForAction(static_cast<Action>(i)) != page) continue;
        if (row-- == 0) return i;
    }
    return kActionCount; // Back / Hide rows or invalid selection.
}

std::vector<std::wstring> HotkeyCategoryEntries(Page page) {
    std::vector<std::wstring> names;
    for (size_t i = 0; i < kActionCount; ++i) {
        if (HotkeyCategoryForAction(static_cast<Action>(i)) == page)
            names.push_back(std::wstring(kActions[i].label) + L": " + KeyName(g_keys[i].load()));
    }
    names.emplace_back(L"Back");
    return names;
}
} // namespace

std::vector<std::wstring> BuildEntriesForPage(Page page) {
    switch (page) {
        case Page::Search: {
            std::vector<std::wstring> rows;
            rows.reserve(s_menu_search_matches.size() + 3);
            rows.push_back(L"Search: [" + (s_menu_search_query.empty()
                ? std::wstring(L"Type a setting name") : s_menu_search_query) + L"_] ");
            if (s_menu_search_query.empty()) {
                rows.emplace_back(L"Type to search all utility menus");
            } else if (s_menu_search_matches.empty()) {
                rows.emplace_back(L"No matching settings - try another term");
            } else {
                for (const auto& match : s_menu_search_matches) {
                    std::wstring path = match.breadcrumb.empty()
                        ? std::wstring(L"Main") : match.breadcrumb;
                    // Keep the actual option label visible even for deeply
                    // nested menus. Truncate the breadcrumb before the item.
                    std::wstring label = match.label;
                    constexpr size_t kMaxResultText = 48;
                    if (label.size() > 34) label = label.substr(0, 31) + L"...";
                    const size_t path_room = kMaxResultText - label.size() - 3;
                    if (path.size() > path_room)
                        path = path_room > 3 ? path.substr(0, path_room - 3) + L"..."
                                             : path.substr(0, path_room);
                    rows.push_back(path + L" > " + label);
                }
            }
            rows.emplace_back(L"Clear search");
            rows.emplace_back(L"Back");
            return rows;
        }
        case Page::Main:
            return {L"Movement  >", L"World  >", L"Maps  >",
                    L"Customisation  >", L"HUD  >", L"Splits  >", L"Inputs  >",
                    L"Ghost  >", L"Replay  >", L"Profiles  >", L"Settings  >",
                    L"Search settings  [/]  >",
                    L"Hide menu  [" + MenuHotkeyDisplayName() + L"]"};
        case Page::RestartOptions:
            return {
                L"Restart run  [" + KeyName(Hotkey(Action::Restart)) + L"]",
                L"Preloaded restart  [" + KeyName(Hotkey(Action::PreloadedRestart)) + L"]",
                L"Restart with progress  [" + KeyName(Hotkey(Action::RestartWithProgress)) + L"]",
                std::wstring(L"R / restore rise animation: ") +
                    (g_progress_rise_animation_enabled ? L"ON" : L"OFF"),
                L"Full level reload (keep game open)  [" +
                    KeyName(Hotkey(Action::FullLevelReload)) + L"]",
                L"Back"
            };
        case Page::ConfirmLevelReload:
            return {
                L"YES - reload current level",
                L"Cancel - return to Restart Options"
            };
        case Page::Movement:
            return {
                L"Movement Modes  >",
                L"Speed & Physics  >",
                L"Jump Controls  >",
                L"Reset movement settings",
                L"Back"
            };
        case Page::MovementModes:
            return {
                std::wstring(L"Flying: ") + (g_flying ? L"ON" : L"OFF"),
                std::wstring(L"Noclip: ") + (g_noclip ? L"ON" : L"OFF"),
                std::wstring(L"Preserve momentum: ") + (g_preserve_momentum ? L"ON" : L"OFF"),
                L"Back"
            };
        case Page::MovementPhysics:
            return {
                L"Fly speed: " + std::to_wstring(static_cast<int>(g_fly_speed)) + L"  < >",
                L"Movement speed: " + std::to_wstring(g_walk_speed_percent) + L"%  < >",
                L"Swim speed: " + std::to_wstring(g_swim_speed_percent) + L"%  < >",
                L"Climb speed: " + std::to_wstring(g_climb_speed_percent) + L"%  < >",
                L"Slide speed: " + std::to_wstring(g_slide_speed_percent) + L"%  < >",
                L"Slide distance: " + std::to_wstring(g_slide_distance_percent) + L"%  < >",
                L"Jump height: " + std::to_wstring(g_jump_height_percent) + L"%  < >",
                L"Acceleration: " + std::to_wstring(g_acceleration_percent) + L"%  < >",
                L"Game speed: " + std::to_wstring(g_game_speed_percent) + L"%  < >",
                L"Back"
            };
        case Page::World:
            return {
                L"Sections  >",
                L"Run & Practice  >",
                L"Buttons  >",
                L"Tiles  >",
                L"Cubes  >",
                L"Tubes  >",
                L"Platforms  >",
                L"Jump Pads  >",
                L"Water  >",
                std::wstring(L"Remove all objects: ") +
                    (AreAllWorldObjectsRemoved() ? L"ON" : L"OFF") + L"  < >",
                L"Restore all removed objects",
                L"World Profiles  >",
                L"Reset World gameplay settings",
                L"Back"
            };
        case Page::Maps: {
            const size_t pages = (CustomMapFileCount() + kMapsPerPage - 1) / kMapsPerPage;
            std::vector<std::wstring> entries{
                L"Refresh maps folder",
                L"Previous page  <",
                L"Next page  >  (" + std::to_wstring(s_maps_page + 1) + L"/" +
                    std::to_wstring((std::max)(size_t{1}, pages)) + L")"
            };
            for (size_t i = 0; i < MapsOnPage(); ++i) {
                const auto name = CustomMapFileLabel(MapPageStart() + i);
                const bool loaded = CustomMapActive() &&
                    CustomMapStatus() == L"Loaded: " + name;
                entries.push_back(L"Load: " + name + (loaded ? L"  [ACTIVE]" : L""));
            }
            entries.push_back(L"Restore original world");
            entries.push_back(L"Back");
            return entries;
        }
        case Page::WorldObjectRemoval:
            return {L"Back"};  // Retired page ID kept for compatibility.
        case Page::WorldCubes:
            return {
                L"Disable all cubes",
                L"Select all cubes (collect)",
                L"Spawn all cubes (including underwater)",
                L"Spawn all cubes (above water only)",
                L"Restore default cube visibility",
                std::wstring(L"Remove cubes: ") +
                    (IsWorldObjectRemoved(WorldObjectKind::Cubes) ? L"ON" : L"OFF") + L"  < >",
                L"Back"
            };
        case Page::Customisation:
            return {
                L"Button Colours  >",
                L"Button Beacons  >",
                L"Tile Colours  >",
                L"Wall Colours  >",
                L"Tube Colours  >",
                L"Platform Colours  >",
                L"Jump Pad Colours  >",
                L"Water  >",
                L"Time  >",
                L"Sky  >",
                L"Shadows  >",
                L"Pillars  >",
                L"Camera FOV  >",
                L"Water Reflections  >",
                L"Crosshair  >",
                L"Customisation Profiles  >",
                L"Restore default customisations",
                L"Back"
            };
        case Page::Crosshair: return CrosshairEntries();
        case Page::CustomisationProfiles: {
            std::vector<std::wstring> items;
            for (const auto& name : CustomisationProfileNames())
                items.emplace_back((name == ActiveCustomisationProfile() ? L"[ACTIVE] " : L"          ") + name);
            items.push_back(L"Create new profile...");
            items.push_back(L"Rename active profile...");
            items.push_back(L"Save active profile now");
            items.push_back(L"Reload active profile from disk");
            items.push_back(L"Refresh profile list");
            items.push_back(L"Delete active profile...");
            items.push_back(L"Back");
            return items;
        }
        case Page::ConfirmDeleteCustomisationProfile:
            return {L"YES - delete profile: " + ActiveCustomisationProfile(),
                    L"Cancel - keep profile"};
        case Page::WorldProfiles: {
            std::vector<std::wstring> items;
            for (const auto& name : WorldProfileNames())
                items.emplace_back((name == ActiveWorldProfile() ? L"[ACTIVE] " : L"          ") + name);
            items.push_back(L"Create new profile...");
            items.push_back(L"Rename active profile...");
            items.push_back(L"Save active profile now");
            items.push_back(L"Reload active profile from disk");
            items.push_back(L"Refresh profile list");
            items.push_back(L"Delete active profile...");
            items.push_back(L"Back");
            return items;
        }
        case Page::ConfirmDeleteWorldProfile:
            return {L"YES - delete profile: " + ActiveWorldProfile(),
                    L"Cancel - keep profile"};
        case Page::HotkeyProfiles: {
            std::vector<std::wstring> items;
            for (const auto& name : HotkeyProfileNames())
                items.emplace_back((name == ActiveHotkeyProfile() ? L"[ACTIVE] " : L"          ") + name);
            items.push_back(L"Create new profile...");
            items.push_back(L"Rename active profile...");
            items.push_back(L"Save active profile now");
            items.push_back(L"Reload active profile from disk");
            items.push_back(L"Refresh profile list");
            items.push_back(L"Delete active profile...");
            items.push_back(L"Back");
            return items;
        }
        case Page::ConfirmDeleteHotkeyProfile:
            return {L"YES - delete profile: " + ActiveHotkeyProfile(),
                    L"Cancel - keep profile"};
        case Page::HudProfiles: {
            std::vector<std::wstring> items;
            for (const auto& name : HudProfileNames())
                items.emplace_back((name == ActiveHudProfile() ? L"[ACTIVE] " : L"          ") + name);
            items.push_back(L"Create new profile...");
            items.push_back(L"Rename active profile...");
            items.push_back(L"Save active profile now");
            items.push_back(L"Reload active profile from disk");
            items.push_back(L"Refresh profile list");
            items.push_back(L"Delete active profile...");
            items.push_back(L"Back");
            return items;
        }
        case Page::ConfirmDeleteHudProfile:
            return {L"YES - delete profile: " + ActiveHudProfile(),
                    L"Cancel - keep profile"};
        case Page::CustomisationShadows:
            return {
                std::wstring(L"Shadows visible: ") +
                    (AreShadowsVisible() ? L"ON" : L"OFF") + L"  < >",
                std::wstring(L"Shadow ambient colour: ") +
                    (IsShadowAmbientColourEnabled() ? L"Custom" : L"Game default") + L"  < >",
                ShadowAmbientMethodLabel(),
                L"Restore default shadows",
                L"Back"
            };
        case Page::CustomisationPillars:
            return {
                std::wstring(L"Pillar colour: ") +
                    (IsPillarColourEnabled() ? L"Custom" : L"Game default") + L"  < >",
                PillarColourMethodLabel(),
                std::wstring(L"Remove pillars: ") +
                    (ArePillarsRemoved() ? L"ON" : L"OFF") + L"  < >",
                L"Back"
            };
        case Page::CustomisationTime:
            return {
                std::wstring(L"Time of day: ") +
                    (IsCustomTimeOfDayEnabled() ? L"Custom" : L"Game default") + L"  < >",
                L"Time: " + ([] {
                    const int minute = GetCustomTimeMinutes();
                    const int hour = minute / 60;
                    const int mins = minute % 60;
                    wchar_t clock[8]{};
                    swprintf_s(clock, L"%02d:%02d", hour, mins);
                    return std::wstring(clock);
                }()) + L"  < >  [Enter = 12:00]",
                std::wstring(L"Lock Time: ") +
                    (IsCustomTimeLocked() ? L"ON" : L"OFF") + L"  < >",
                L"Time Speed: " + std::to_wstring(GetCustomTimeSpeedPercent()) +
                    L"%  < >  [Enter = 100%]",
                L"Restore game-default time",
                L"Back"
            };
        case Page::CustomisationSky:
            return {
                L"Sky Colours  >",
                L"Sky Visibility  >",
                L"Fog Colours  >",
                L"Fog Visibility  >",
                L"Restore default sky and atmosphere",
                L"Back"
            };
        case Page::CustomisationSkyColours:
            return {
                std::wstring(L"Sky zenith: ") +
                    (IsSkyColourEnabled(SkyRegion::Zenith) ? L"Custom" : L"Game default") + L"  < >",
                SkyColourMethodLabel(SkyRegion::Zenith),
                std::wstring(L"Sky horizon: ") +
                    (IsSkyColourEnabled(SkyRegion::Horizon) ? L"Custom" : L"Game default") + L"  < >",
                SkyColourMethodLabel(SkyRegion::Horizon),
                std::wstring(L"Sky sun: ") +
                    (IsSkyColourEnabled(SkyRegion::Sun) ? L"Custom" : L"Game default") + L"  < >",
                SkyColourMethodLabel(SkyRegion::Sun),
                std::wstring(L"Cloud colour: ") +
                    (IsSkyColourEnabled(SkyRegion::Clouds) ? L"Custom" : L"Game default") + L"  < >",
                SkyColourMethodLabel(SkyRegion::Clouds),
                L"Back"
            };
        case Page::CustomisationSkyVisibility:
            return {
                std::wstring(L"Moon/Sun visible: ") +
                    (IsSkyDiscVisible() ? L"ON" : L"OFF") + L"  < >",
                std::wstring(L"Clouds visible: ") +
                    (IsSkyEffectVisible(SkyEffect::Clouds) ? L"ON" : L"OFF") + L"  < >",
                std::wstring(L"Stars visible: ") +
                    (IsSkyEffectVisible(SkyEffect::Stars) ? L"ON" : L"OFF") + L"  < >",
                L"Back"
            };
        case Page::CustomisationFogColours:
            return {
                std::wstring(L"Fog haze: ") +
                    (IsFogColourEnabled(FogRegion::General) ? L"Custom" : L"Game default") + L"  < >",
                FogColourMethodLabel(FogRegion::General),
                std::wstring(L"Directional fog: ") +
                    (IsFogColourEnabled(FogRegion::Directional) ? L"Custom" : L"Game default") + L"  < >",
                FogColourMethodLabel(FogRegion::Directional),
                L"Back"
            };
        case Page::CustomisationFogVisibility:
            return {
                std::wstring(L"Fog visible: ") +
                    (IsFogVisible(FogRegion::General) ? L"ON" : L"OFF") + L"  < >",
                std::wstring(L"Directional fog visible: ") +
                    (IsFogVisible(FogRegion::Directional) ? L"ON" : L"OFF") + L"  < >",
                L"Back"
            };
        case Page::WorldWater:
            return {
                std::wstring(L"Remove water: ") +
                    (IsAllWaterRemoved() ? L"ON" : L"OFF") + L"  < >",
                std::wstring(L"Remove water surface only: ") + (IsWaterRemoved() ? L"ON" : L"OFF") +
                    (IsAllWaterRemoved() ? L" (master ON)" : L"") + L"  < >",
                std::wstring(L"Remove water gravity / swimming: ") +
                    (IsWaterSwimmingDisabled() ? L"ON" : L"OFF") +
                    (IsAllWaterRemoved() ? L" (master ON)" : L"") + L"  < >",
                L"Water surface height: " + std::to_wstring(GetWaterHeightOffset()) +
                    L" uu  < >",
                L"Reset water surface height",
                L"Water customisation  >",
                L"Back"
            };
        case Page::WorldWaterColours:
            return {
                std::wstring(L"Water colour: ") +
                    (IsWaterCustomColourEnabled() ? L"Custom" : L"Game default") + L"  < >",
                WaterColourMethodLabel(),
                L"Surface intensity: " +
                    std::to_wstring(GetWaterColourIntensityPercent()) +
                    L"%  < >  [Enter = 100%]",
                std::wstring(L"Underwater colour: ") +
                    (IsUnderwaterCustomColourEnabled() ? L"Custom" : L"Game default") + L"  < >",
                UnderwaterColourMethodLabel(),
                L"Underwater intensity: " +
                    std::to_wstring(GetUnderwaterColourIntensityPercent()) +
                    L"%  < >  [Enter = 100%]",
                std::wstring(L"Remove underwater tint: ") +
                    (IsUnderwaterTintRemoved() ? L"ON" : L"OFF") +
                    (IsAllWaterRemoved() ? L" (master ON)" : L"") + L"  < >",
                std::wstring(L"Remove water splashes: ") +
                    (AreWaterSplashesRemoved() ? L"ON" : L"OFF") +
                    (IsAllWaterRemoved() ? L" (master ON)" : L"") + L"  < >",
                L"Back"
            };
        case Page::WaterReflections:
            return {
                std::wstring(L"All water reflections: ") +
                    (AreWaterReflectionsVisible() ? L"ON" : L"OFF") + L"  < >",
                std::wstring(L"Reflect walls/platforms: ") +
                    (AreWaterReflectedWallsVisible() ? L"ON" : L"OFF") + L"  < >",
                std::wstring(L"Reflect buttons: ") +
                    (AreWaterReflectedButtonsVisible() ? L"ON" : L"OFF") + L"  < >",
                std::wstring(L"Reflect pipes: ") +
                    (AreWaterReflectedPipesVisible() ? L"ON" : L"OFF") + L"  < >",
                std::wstring(L"Reflect jump pads: ") +
                    (AreWaterReflectedJumpPadsVisible() ? L"ON" : L"OFF") + L"  < >",
                std::wstring(L"Reflect cubes: ") +
                    (AreWaterReflectedCubesVisible() ? L"ON" : L"OFF") + L"  < >",
                std::wstring(L"Reflect button beacon beams: ") +
                    (AreWaterReflectedBeaconsVisible() ? L"ON" : L"OFF") + L"  < >",
                L"Back"
            };
        case Page::WaterObjectReflections: case Page::WaterBeamReflections:
            return {L"Back"};  // Retired page IDs kept for compatibility.
        case Page::WorldTubes:
            return {L"Tube Colours  >", L"Tube Speed  >", L"Back"};
        case Page::WorldPlatforms:
            return {L"Platform Colours  >", L"Platform Speed  >", L"Back"};
        case Page::WorldJumpPads:
            return {L"Jump Pad Colours  >", L"Launch Height  >", L"Back"};
        case Page::WorldJumpPadColours:
            return {
                std::wstring(L"Jump pad colour: ") +
                    (IsJumpPadCustomColourEnabled() ? L"Custom" : L"Game default") + L"  < >",
                JumpPadColourMethodLabel(), L"Back"
            };
        case Page::WorldJumpPadHeight:
            return {
                L"Launch height: " + std::to_wstring(GetJumpPadHeightPercent()) + L"%  < >",
                L"Reset to 100%",
                std::wstring(L"Remove jump pads: ") +
                    (IsWorldObjectRemoved(WorldObjectKind::JumpPads) ? L"ON" : L"OFF") + L"  < >",
                L"Back"
            };
        case Page::WorldPlatformColours:
            return {
                std::wstring(L"Platform colour: ") +
                    (IsPlatformCustomColourEnabled() ? L"Custom" : L"Game default") + L"  < >",
                PlatformColourMethodLabel(),
                L"Back"
            };
        case Page::WorldPlatformSpeed:
            return {
                L"Platform speed: " + std::to_wstring(GetPlatformSpeedPercent()) + L"%  < >",
                L"Reset to 100%",
                std::wstring(L"Remove moving platforms: ") +
                    (IsWorldObjectRemoved(WorldObjectKind::Platforms) ? L"ON" : L"OFF") + L"  < >",
                L"Back"
            };
        case Page::WorldTubeSpeed:
            return {
                L"Travel speed: " + std::to_wstring(GetTubeTravelSpeedPercent()) + L"%  < >",
                L"Reset to 100%",
                std::wstring(L"Remove tubes: ") +
                    (IsWorldObjectRemoved(WorldObjectKind::Tubes) ? L"ON" : L"OFF") + L"  < >",
                L"Back"
            };
        case Page::WorldTubeColours:
            return {
                std::wstring(L"Tube colour: ") +
                    (IsTubeCustomColourEnabled() ? L"Custom" : L"Game default") + L"  < >",
                TubeColourMethodLabel(),
                L"Back"
            };
        case Page::WorldSections:
            return {
                std::wstring(L"Animate section changes: ") +
                    (g_animate_section_changes ? L"ON" : L"OFF") + L"  < >",
                L"Raise all sections",
                L"Lower all sections",
                L"Reset all sections to Auto",
                SectionButtonSyncStatus(),
                L"Individual sections  >",
                L"Adjust section positions  >",
                L"Section Speed  >",
                L"Back"
            };
        case Page::WorldSectionSpeed:
            return {
                L"Section rise speed: " + std::to_wstring(GetSectionSpeedPercent()) + L"%  < >",
                L"Reset to 100%",
                L"Back"
            };
        case Page::WorldSectionHeights:
            return {
                L"All sections X: " + AllSectionAxisOffsetLabel(0) + L"  < >",
                L"All sections Y: " + AllSectionAxisOffsetLabel(1) + L"  < >",
                L"All sections Z: " + AllSectionAxisOffsetLabel(2) + L"  < >",
                L"Individual section positions  >",
                L"Restore all positions to default",
                L"Back"
            };
        case Page::WorldIndividualSectionHeights: {
            std::vector<std::wstring> lines;
            lines.reserve(kPracticeClusterCount + 1);
            for (int i = 1; i <= kPracticeClusterCount; ++i) {
                lines.emplace_back(L"Section " + std::to_wstring(i) + L": X" +
                    SectionOffsetLabel(GetSectionPositionOffset(i, 0)) + L" Y" +
                    SectionOffsetLabel(GetSectionPositionOffset(i, 1)) + L" Z" +
                    SectionOffsetLabel(GetSectionPositionOffset(i, 2)) + L"  >");
            }
            lines.emplace_back(L"Back");
            return lines;
        }
        case Page::WorldSectionPositionEdit:
            return {
                L"Section " + std::to_wstring(s_position_edit_section) + L" X: " +
                    SectionOffsetLabel(GetSectionPositionOffset(s_position_edit_section, 0)) + L"  < >",
                L"Section " + std::to_wstring(s_position_edit_section) + L" Y: " +
                    SectionOffsetLabel(GetSectionPositionOffset(s_position_edit_section, 1)) + L"  < >",
                L"Section " + std::to_wstring(s_position_edit_section) + L" Z: " +
                    SectionOffsetLabel(GetSectionPositionOffset(s_position_edit_section, 2)) + L"  < >",
                L"Restore this section's XYZ position",
                L"Back"
            };
        case Page::WorldButtons:
            return {
                L"Button Controls  >",
                L"Button Colours  >",
                L"Beacons  >",
                L"Back"
            };
        case Page::WorldButtonActions:
            return {
                L"Restore game-default button states",
                L"Enable buttons (above water)",
                L"Enable all buttons (including underwater)",
                L"Disable buttons (underwater)",
                L"Disable buttons (above water)",
                L"Disable all buttons",
                L"Press all available buttons",
                L"Repair ghost buttons now",
                std::wstring(L"Prevent button tile selection: ") +
                    (IsButtonTileSelectionDisabled() ? L"ON" : L"OFF") + L"  < >",
                std::wstring(L"Remove buttons: ") +
                    (IsWorldObjectRemoved(WorldObjectKind::Buttons) ? L"ON" : L"OFF") + L"  < >",
                L"Back"
            };
        case Page::WorldButtonColours:
            return {
                std::wstring(L"Unpressed button colour: ") +
                    (IsButtonCustomColourEnabled(false) ? L"Custom" : L"Game default") + L"  < >",
                ButtonColourMethodLabel(0),
                std::wstring(L"Pressed button colour: ") +
                    (IsButtonCustomColourEnabled(true) ? L"Custom" : L"Game default") + L"  < >",
                ButtonColourMethodLabel(1),
                std::wstring(L"Unpressed border colour: ") +
                    (IsButtonBorderCustomColourEnabled(false) ? L"Custom" : L"Game default") + L"  < >",
                ButtonColourMethodLabel(2),
                std::wstring(L"Pressed border colour: ") +
                    (IsButtonBorderCustomColourEnabled(true) ? L"Custom" : L"Game default") + L"  < >",
                ButtonColourMethodLabel(3),
                L"Back"
            };
        case Page::WorldButtonBeacons:
            return {
                std::wstring(L"Button beacons: ") +
                    (IsButtonBeaconVisible() ? L"Visible" : L"Hidden") + L"  < >",
                std::wstring(L"Beacon colour: ") +
                    (IsButtonBeaconCustomColourEnabled() ? L"Custom" : L"Game default") +
                    L"  < >",
                ButtonColourMethodLabel(4),
                L"Back"
            };
        case Page::WorldTiles:
            return {
                L"Tile Controls  >",
                L"Tile Colours  >",
                L"Wall Colours  >",
                L"Back"
            };
        case Page::WorldTileActions:
            return {
                L"Select tiles (above water)",
                L"Select all tiles (including underwater)",
                L"Deselect tiles (above water)",
                L"Deselect all tiles (including underwater)",
                std::wstring(L"Disable selection on touch: ") +
                    (IsTileSelectionOnContactDisabled() ? L"ON" : L"OFF") + L"  < >",
                std::wstring(L"Remove tiles: ") +
                    (IsWorldObjectRemoved(WorldObjectKind::Tiles) ? L"ON" : L"OFF") + L"  < >",
                L"Back"
            };
        case Page::WorldTileColours:
            return {
                L"Selected tile colour: " + TileColourPresetLabel(true) + L"  < >",
                TileColourMethodLabel(true),
                std::wstring(L"Random RGB for newly selected: ") +
                    (IsRandomTileRGBEnabled() ? L"ON" : L"OFF") + L"  < >",
                L"Unselected tile colour: " + TileColourPresetLabel(false) + L"  < >",
                TileColourMethodLabel(false),
                L"Back"
            };
        case Page::WorldWalls:
            return {
                L"Selected wall colour: " + WallColourLabel(true) + L"  < >",
                WallColourMethodLabel(true),
                L"Unselected wall colour: " + WallColourLabel(false) + L"  < >",
                WallColourMethodLabel(false),
                L"Back"
            };
        case Page::WorldIndividualSections: {
            std::vector<std::wstring> lines;
            lines.reserve(kPracticeClusterCount + 1);
            for (int section = 1; section <= kPracticeClusterCount; ++section) {
                const SectionPlacement mode = GetSectionPlacement(section);
                const wchar_t* label = mode == SectionPlacement::Auto ? L"Auto" :
                    (mode == SectionPlacement::Raised ? L"Raised" : L"Lowered");
                lines.emplace_back(L"Section " + std::to_wstring(section) +
                    L": " + label + L"  < >");
            }
            lines.emplace_back(L"Back");
            return lines;
        }
        case Page::WorldPlayer:
            return {
                L"FOV: " + (g_player_fov_degrees == 0
                    ? std::wstring(L"Default")
                    : std::to_wstring(g_player_fov_degrees) + L" degrees") + L"  < >",
                L"Reset FOV to default",
                L"Back"
            };
        case Page::MovementJump:
            return {
                std::wstring(L"Scroll jump: ") + JumpAssistModeLabel(true) + L"  < >",
                std::wstring(L"Hold Space jump: ") + JumpAssistModeLabel(false) + L"  < >",
                std::wstring(L"Air jump: ") + (g_air_jump_enabled ? L"ON" : L"OFF") +
                    L"  [" + KeyName(Hotkey(Action::ToggleAirJump)) + L"]  < >",
                L"Back"
            };
        case Page::Persistence:
            return {
                std::wstring(L"Keep movement on game launch: ") +
                    (g_keep_movement_on_game_launch ? L"ON" : L"OFF") + L"  < >",
                std::wstring(L"Keep movement on map restart: ") +
                    (g_keep_movement_on_map_restart ? L"ON" : L"OFF") + L"  < >",
                std::wstring(L"Keep world on game launch: ") +
                    (g_keep_world_on_game_launch ? L"ON" : L"OFF") + L"  < >",
                std::wstring(L"Keep world on map restart: ") +
                    (g_keep_world_on_map_restart ? L"ON" : L"OFF") + L"  < >",
                L"Back"
            };
        case Page::GameplayRecords:
            return {
                std::wstring(L"Save personal bests: ") +
                    (g_save_personal_bests ? L"ON" : L"OFF") + L"  < >",
                std::wstring(L"Save fastest segments: ") +
                    (g_save_fastest_segments ? L"ON" : L"OFF") + L"  < >",
                std::wstring(L"Save PB ghosts: ") +
                    (g_save_pb_ghosts ? L"ON" : L"OFF") + L"  < >",
                std::wstring(L"Save fastest-section ghosts: ") +
                    (g_save_fastest_ghosts ? L"ON" : L"OFF") + L"  < >",
                std::wstring(L"Save with cheats: ") + CheatRecordModeLabel() + L"  < >",
                std::wstring(L"Create profile on new PB: ") +
                    (g_auto_create_pb_profile ? L"ON" : L"OFF") + L"  < >",
                std::wstring(L"Copy fastest segments to new profile: ") +
                    (g_auto_pb_copy_best_segments ? L"ON" : L"OFF") + L"  < >",
                L"Back"
            };
        case Page::Ghost:
            return {
                std::wstring(L"Show ghost: ") +
                    (IsPBGhostEnabled() ? L"ON" : L"OFF") + L"  < >",
                std::wstring(L"Ghost mode: ") +
                    (IsPBGhostPassThroughEnabled() ? L"3D" : L"2D") + L"  < >",
                std::wstring(L"2D transparency: ") +
                    (IsPBGhost2DTransparencyEnabled() ? L"ON" : L"OFF") + L"  < >",
                std::wstring(L"3D transparency: ") +
                    (IsPBGhost3DTransparencyEnabled() ? L"ON" : L"OFF") + L"  < >",
                GhostColourMethodLabel(),
                std::wstring(L"Start next ghost on button: ") +
                    (IsPBGhostButtonSyncEnabled() ? L"ON" : L"OFF") + L"  < >",
                std::wstring(L"Use fastest sections: ") +
                    (IsPBGhostFastestSectionsEnabled() ? L"ON" : L"OFF") + L"  < >",
                L"Back"
            };
        case Page::GhostReplay:
            return {
                std::wstring(L"Replay: ") +
                    (IsGhostReplayRequested() ? L"Stop" : L"Start") + L"  [Enter]",
                L"Replay profile: " + (g_active_profile.empty() ? L"None" : g_active_profile) + L"  < >",
                L"Source: " + GhostReplaySourceLabel() + L"  < >",
                std::wstring(L"Pause / resume: ") +
                    (IsGhostReplayPaused() ? L"Resume" : L"Pause") + L"  [Enter]",
                L"Restart from beginning  [Enter]",
                L"Seek backward 5 seconds  [Enter]",
                L"Seek forward 5 seconds  [Enter]",
                L"Playback speed: " + GhostReplaySpeedLabel() + L"  < >",
                L"Playback time: " + GhostReplayStatus(),
                L"Camera: " + GhostReplayCameraLabel() + L"  < >",
                L"Back"
            };
        case Page::SaveStates:
            return {
                L"Restart Controls  >",
                L"Positions & Teleport  >",
                L"Save States  >",
                L"Button Checkpoints  >",
                L"Section Practice  >",
                L"Back"
            };
        case Page::SavePositions:
            return {
                L"Save position  [" + KeyName(Hotkey(Action::SavePosition)) + L"]",
                L"Load position  [" + KeyName(Hotkey(Action::RestorePosition)) + L"]",
                L"Teleport to coordinates  >",
                L"Teleport to last button (global)  [" +
                    KeyName(Hotkey(Action::TeleportLastButtonGlobal)) + L"]",
                g_saved_valid ? L"Saved position: available" : L"Saved position: none",
                L"Back"
            };
        case Page::TeleportCoordinates:
            return {
                TeleportAxisLabel(0, L"X"),
                TeleportAxisLabel(1, L"Y"),
                TeleportAxisLabel(2, L"Z"),
                L"Teleport now",
                L"Use current position as target",
                L"Back"
            };
        case Page::SavedCheckpoints:
            return {
                L"Save current state  [" + KeyName(Hotkey(Action::SaveState)) + L"]",
                L"Load saved state  [" + KeyName(Hotkey(Action::LoadState)) + L"]",
                g_saved_checkpoint_valid
                    ? L"Saved state: " + std::to_wstring(g_saved_checkpoint.buttons) + L" buttons"
                    : L"Saved state: none",
                L"Back"
            };
        case Page::ButtonCheckpoints:
            return {
                L"Load last button  [" + KeyName(Hotkey(Action::LoadLastButton)) + L"]",
                L"Load second last button  [" + KeyName(Hotkey(Action::LoadSecondLastButton)) + L"]",
                std::wstring(L"Keep momentum on last button: ") +
                    (g_keep_last_button_momentum ? L"ON" : L"OFF"),
                g_last_button_checkpoint_valid
                    ? L"Last button: " + ButtonLabel(g_last_button_checkpoint.buttons)
                    : L"Last button: none",
                SecondLastButtonNumber() > 0
                    ? L"Second last button: " + ButtonLabel(SecondLastButtonNumber())
                    : L"Second last button: none",
                L"Back"
            };
        case Page::Practice:
            return {
                L"Selected button: " + ButtonLabel(g_practice_selected_button) +
                    L"   [Left / Right]",
                L"Load / replay section  [" + KeyName(Hotkey(Action::ReplaySection)) + L"]",
                L"Status: " + PracticeStatus(),
                L"Back"
            };
        case Page::Hud:
            return {
                L"Side HUD Elements  >",
                L"Customisations  >",
                std::wstring(L"Show option descriptions: ") +
                    (g_show_menu_descriptions ? L"ON" : L"OFF"),
                L"Reset all HUD display settings (including splits)",
                L"HUD Profiles  >",
                L"Back"
            };
        case Page::Splits:
            return {
                L"Split HUD Elements  >",
                L"Split Popups & Comparisons  >",
                L"Saved Split Times  >",
                L"Split HUD Customisations  >",
                L"Back"
            };
        case Page::HudTiming:
            return {
                std::wstring(L"Show button split popups: ") + (g_show_splits ? L"ON" : L"OFF"),
                L"Configure Popup Rows  >",
                L"Compare Profile & Mode  >",
                L"Back"
            };
        case Page::HudRecordTimes: {
            const SumOfBest sob = CalculateSumOfBest();
            const std::wstring pb_time = std::isfinite(g_pb.total_seconds) &&
                g_pb.total_seconds >= 0.0 && g_pb.total_seconds <= 86400.0
                ? FormatSplitTime(g_pb.total_seconds) : L"N/A";
            const std::wstring sum_of_best = sob.recorded > 0 &&
                std::isfinite(sob.seconds) && sob.seconds <= 86400.0
                ? FormatSplitTime(sob.seconds) + L" (" +
                  std::to_wstring(sob.recorded) + L"/" +
                  std::to_wstring(kRefunctButtonCount) + L")"
                : L"N/A";
            const std::wstring last_split = g_button_splits.empty() ||
                !std::isfinite(g_button_splits.back().run_seconds) ||
                g_button_splits.back().run_seconds < 0.0 ||
                g_button_splits.back().run_seconds > 86400.0
                ? L"N/A"
                : ButtonLabel(g_button_splits.back().button_number) + L"  " +
                  FormatSplitTime(g_button_splits.back().run_seconds);
            const int reference_button = g_button_splits.empty()
                ? 0 : g_button_splits.back().button_number;
            const double best_seconds = ProfileRecordedTimeAtButton(
                g_pb.button_seconds, g_pb.best_segment_seconds, reference_button,
                ProfileCompareMode::BestSegments, g_best_segment_time_view);
            const double pb_seconds = ProfileRecordedTimeAtButton(
                g_pb.button_seconds, g_pb.best_segment_seconds, reference_button,
                ProfileCompareMode::PersonalBest, g_pb_split_time_view);
            const std::wstring best_segment_time = best_seconds >= 0.0
                ? FormatSplitTime(best_seconds) : L"N/A";
            const std::wstring pb_split_time = pb_seconds >= 0.0
                ? FormatSplitTime(pb_seconds) : L"N/A";
            return {
                std::wstring(L"Full-run PB time: ") + (g_show_pb_run_hud ? L"ON" : L"OFF") + L"  (" + pb_time + L")",
                std::wstring(L"Sum of best segments: ") + (g_show_sum_of_best_hud ? L"ON" : L"OFF") + L"  (" + sum_of_best + L")",
                std::wstring(L"Run time at last button: ") + (g_show_last_button_split_hud ? L"ON" : L"OFF") + L"  (" + last_split + L")",
                std::wstring(L"Other profile saved time: ") +
                    RecordTimeHudModeLabel(g_show_compare_hud, g_compare_hud_view) + L"  < >",
                std::wstring(L"Fastest button record: ") +
                    RecordTimeHudModeLabel(g_show_best_segment_time_hud, g_best_segment_time_view) +
                    L"  < >  (" + (g_show_best_segment_time_hud ? best_segment_time : L"Off") + L")",
                std::wstring(L"PB time at last button: ") +
                    RecordTimeHudModeLabel(g_show_pb_split_time_hud, g_pb_split_time_view) +
                    L"  < >  (" + (g_show_pb_split_time_hud ? pb_split_time : L"Off") + L")",
                L"Back"
            };
        }
        case Page::HudGameplay:
        case Page::SplitHud: {
            std::vector<std::wstring> entries;
            const bool splits = page == Page::SplitHud;
            const size_t count = splits ? kSplitHudElements.size() : kGeneralHudElements.size();
            entries.reserve(count + 1);
            for (size_t i = 0; i < count; ++i) {
                const auto element = splits ? kSplitHudElements[i] : kGeneralHudElements[i];
                const size_t index = static_cast<size_t>(element);
                entries.emplace_back(std::wstring(kHudPositionLabels[index]) + L": " +
                    (HudElementIsVisible(element) ? L"ON" : L"OFF"));
            }
            entries.emplace_back(L"Back");
            return entries;
        }
        case Page::HudKeyDisplay:
            return {
                std::wstring(L"Show key display: ") + (g_show_key_display ? L"ON" : L"OFF") + L"  < >",
                L"Customise key display  >",
                L"Back"
            };
        case Page::HudAppearance:
        case Page::SplitHudAppearance: {
            std::vector<std::wstring> entries;
            const bool splits = page == Page::SplitHudAppearance;
            // Input display has its own editor under Main / Inputs.
            const size_t count = splits ? kSplitHudElements.size() : kGeneralHudElements.size();
            entries.reserve(count + (splits ? 1 : 2));
            for (size_t i = 0; i < count; ++i) {
                const auto element = splits ? kSplitHudElements[i] : kGeneralHudElements[i];
                const size_t index = static_cast<size_t>(element);
                entries.emplace_back(std::wstring(kHudPositionLabels[index]) + L"  [" +
                    HudCornerLabel(g_hud_corners[index]) + L", " +
                    std::to_wstring(g_hud_size_percent[index]) + L"%]  >");
            }
            if (!splits)
                entries.emplace_back(std::wstring(L"F5 utility menu position: ") +
                    MenuPositionLabel(g_menu_position) + L"  < >");
            entries.emplace_back(L"Back");
            return entries;
        }
        case Page::HudAppearanceEdit: {
            if (s_hud_style_element >= kHudElementCount) return {L"Back"};
            const int mode = g_hud_colour_modes[s_hud_style_element];
            const auto element = static_cast<HudElement>(s_hud_style_element);
            std::vector<std::wstring> entries{
                std::wstring(L"Position: ") + HudCornerLabel(g_hud_corners[s_hud_style_element]) + L"  < >",
                std::wstring(L"Colour: ") + kHudColourLabels[std::min(mode, kHudColourModeCount - 1)] + L"  < >",
                HudCustomColourMethodLabel(),
                L"Text size: " + std::to_wstring(g_hud_size_percent[s_hud_style_element]) + L"%  < >",
                std::wstring(L"Show header: ") +
                    (g_hud_show_headers[s_hud_style_element] ? L"ON" : L"OFF") + L"  < >"
            };
            if (HudElementHasView(element))
                entries.emplace_back(L"Displayed values: " + HudElementView(element) + L"  < >");
            entries.emplace_back(L"Reset this element customisations");
            entries.emplace_back(L"Back");
            return entries;
        }
        case Page::HudPositions: {
            std::vector<std::wstring> entries;
            entries.reserve(kHudElementCount + 2);
            for (size_t i = 0; i < kHudElementCount; ++i)
                entries.emplace_back(std::wstring(kHudPositionLabels[i]) + L": " +
                    HudCornerLabel(g_hud_corners[i]) + L"  < >");
            entries.emplace_back(std::wstring(L"F5 utility menu: ") +
                MenuPositionLabel(g_menu_position) + L"  < >");
            entries.emplace_back(L"Back");
            return entries;
        }
        case Page::SplitDisplay:
            return {
                std::wstring(L"Show button number: ") + (g_show_split_button ? L"ON" : L"OFF"),
                std::wstring(L"Elapsed run time at button: ") + (g_show_split_total_time ? L"ON" : L"OFF"),
                std::wstring(L"Time since previous button: ") + SplitRowModeLabel(g_segment_row_mode) + L"  < >",
                std::wstring(L"Total ahead/behind PB: ") + SplitRowModeLabel(g_pb_row_mode) + L"  < >",
                std::wstring(L"Button time vs fastest: ") + FastestDeltaModeLabel() + L"  < >",
                std::wstring(L"Total vs other profile: ") + ComparePopupDisplayLabel() + L"  < >",
                // Every setting that can draw a row in the split popup belongs here.
                // Section rows were previously hidden under Comparison Settings.
                std::wstring(L"Section time vs PB: ") +
                    SectionCompareModeLabel(g_section_vs_pb_display) + L"  < >",
                std::wstring(L"Section time vs other profile: ") +
                    SectionCompareModeLabel(g_section_vs_file_display) + L"  < >",
                std::wstring(L"Time gained/lost at button: ") +
                    SectionCompareModeLabel(g_pb_gain_loss_display) + L"  < >",
                L"Back"
            };
        case Page::HudComparisons:
            return {
                std::wstring(L"Other profile reference: ") + CompareModeLabel() + L"  < >",
                L"Compare to split profile: " + (g_compare_profile.empty() ? L"None" : g_compare_profile) + L"  < >",
                L"Reload compared profile",
                L"Back"
            };
        case Page::ProfilesHub:
            return {L"Replay / Split Profiles  >", L"Customisation Profiles  >",
                    L"HUD Profiles  >", L"World Profiles  >", L"Hotkey Profiles  >", L"Back"};
        case Page::Profiles: {
            std::vector<std::wstring> items;
            for (const auto& name : g_profiles)
                items.emplace_back((name == g_active_profile ? L"[ACTIVE] " : L"          ") + name);
            items.push_back(L"Create new profile...");
            items.push_back(L"Rename active profile...");
            items.push_back(L"Refresh files from disk");
            items.push_back(L"Reload active split file");
            items.push_back(L"Record Saving  >");
            items.push_back(L"Back");
            return items;
        }
        case Page::Settings:
            return {L"Hotkeys  >", L"Persistence  >", L"Logs  >", L"Back"};
        case Page::Logs:
            return {std::wstring(L"Save Logs: ") +
                        (g_util_log_enabled.load(std::memory_order_relaxed) ? L"ON" : L"OFF"),
                    std::wstring(L"Startup Logs: ") +
                        (!g_startup_log_enabled.load(std::memory_order_relaxed) ? L"OFF" :
                            (g_util_log_enabled.load(std::memory_order_relaxed) ? L"ON" : L"ON (paused)")),
                    ClearLogsMenuLabel(), L"Back"};
        case Page::ConfirmClearLogs:
            return {L"YES - clear all runtime logs", L"Cancel"};
        case Page::Hotkeys: {
            std::vector<std::wstring> names;
            names.reserve(kHotkeyCategories.size() + 3);
            for (const auto& category : kHotkeyCategories)
                names.push_back(std::wstring(category.title) + L"  >");
            names.emplace_back(L"Hotkey Profiles  >");
            names.emplace_back(L"Restore default hotkeys");
            names.emplace_back(L"Back");
            return names;
        }
        case Page::HotkeysGeneral: case Page::HotkeysMovement:
        case Page::HotkeysPractice: case Page::HotkeysHUD:
        case Page::HotkeysWorld: case Page::HotkeysGhost:
            return HotkeyCategoryEntries(page);
    }
    return {};
}

void RefreshMenuSearch() {
    s_menu_search_matches.clear();
    if (s_menu_search_query.empty()) return;
    // Reuse the same matcher for every page and row. This also handles
    // multi-word queries spanning the menu breadcrumb and the setting name.
    const MenuSearchTerms query(s_menu_search_query);
    if (query.empty()) return;
    // Search static page IDs only; skip transient/confirmation pages and
    // generated map file lists. Dynamic HUD appearance pages are indexed using
    // the currently selected element; jumping still opens the normal editor.
    for (int raw = static_cast<int>(Page::Main);
         raw <= static_cast<int>(Page::Crosshair); ++raw) {
        const Page page = static_cast<Page>(raw);
        if (page == Page::Main || page == Page::Maps ||
            page == Page::ConfirmLevelReload ||
            page == Page::ConfirmDeleteCustomisationProfile ||
            page == Page::ConfirmDeleteWorldProfile ||
            page == Page::ConfirmDeleteHotkeyProfile ||
            page == Page::ConfirmDeleteHudProfile || page == Page::Search ||
            page == Page::HudAppearanceEdit ||
            // These legacy parent pages are no longer navigable from World.
            // Keep their enum IDs stable, but do not duplicate obsolete
            // "Controls" and cosmetic links in search results.
            page == Page::WorldButtons || page == Page::WorldTiles ||
            page == Page::WorldTubes || page == Page::WorldPlatforms ||
            page == Page::WorldJumpPads ||
            page == Page::WorldObjectRemoval ||
            page == Page::WaterObjectReflections || page == Page::WaterBeamReflections) continue;
        const auto rows = BuildEntriesForPage(page);
        // Match the route that direct search navigation will actually use.
        const std::wstring breadcrumb =
            page == Page::HudProfiles && s_menu_search_return_page != Page::HudProfiles
                ? L"Profiles / HUD Profiles"
            : page == Page::CustomisationProfiles && s_menu_search_return_page != Page::CustomisationProfiles
                ? L"Profiles / Customisation Profiles"
            : page == Page::WorldProfiles && s_menu_search_return_page != Page::WorldProfiles
                ? L"Profiles / World Profiles"
            : page == Page::HotkeyProfiles && s_menu_search_return_page != Page::HotkeyProfiles
                ? L"Profiles / Hotkey Profiles"
            : MenuBreadcrumb(page);
        for (size_t row = 0; row < rows.size(); ++row) {
            const std::wstring& label = rows[row];
            if (label == L"Back" || label.rfind(L"Hide menu", 0) == 0) continue;
            if (!query.matches(breadcrumb, label)) continue;
            s_menu_search_matches.push_back({page, static_cast<int>(row), label, breadcrumb});
            if (s_menu_search_matches.size() >= kMaxMenuSearchMatches) return;
        }
    }
}

void OpenMenuSearch() {
    if (g_page != Page::Search) {
        s_menu_search_return_page = g_page;
        s_menu_search_return_selection = g_selection;
    }
    SetPage(Page::Search);
    s_menu_search_query.clear();
    s_menu_search_matches.clear();
}

// Show the current location instead of repeating the keyboard instructions
// on every submenu. Back and Hide menu remain selectable on each page.
const wchar_t* MenuBreadcrumb(Page page) {
    switch (page) {
        case Page::Main: return L"";
        case Page::Search: return L"Search / Type to find settings";
        case Page::RestartOptions: return L"World / Run & Practice / Restart Controls";
        case Page::ConfirmLevelReload: return L"World / Run & Practice / Confirm Reload";
        case Page::Movement: return L"Movement";
        case Page::MovementModes: return L"Movement / Modes";
        case Page::MovementPhysics: return L"Movement / Speed & Physics";
        case Page::MovementJump: return L"Movement / Jump Controls";
        case Page::World: return L"World";
        case Page::Maps: return L"Maps / Custom Worlds";
        case Page::WorldCubes: return L"World / Cubes";
        case Page::WorldObjectRemoval: return L"World / Legacy Remove Objects";
        case Page::Customisation: return L"Customisation";
        case Page::Crosshair: return L"Customisation / Crosshair";
        case Page::HudProfiles: return s_hud_profiles_return_page == Page::ProfilesHub
            ? L"Profiles / HUD Profiles" : L"HUD / Profiles";
        case Page::ConfirmDeleteHudProfile: return s_hud_profiles_return_page == Page::ProfilesHub
            ? L"Profiles / HUD Profiles / Delete" : L"HUD / Profiles / Delete";
        case Page::CustomisationProfiles: return s_customisation_profiles_return_page == Page::ProfilesHub
            ? L"Profiles / Customisation Profiles" : L"Customisation / Profiles";
        case Page::ConfirmDeleteCustomisationProfile: return s_customisation_profiles_return_page == Page::ProfilesHub
            ? L"Profiles / Customisation Profiles / Delete" : L"Customisation / Profiles / Delete";
        case Page::WorldProfiles: return s_world_profiles_return_page == Page::ProfilesHub
            ? L"Profiles / World Profiles" : L"World / World Profiles";
        case Page::ConfirmDeleteWorldProfile: return s_world_profiles_return_page == Page::ProfilesHub
            ? L"Profiles / World Profiles / Delete" : L"World / World Profiles / Delete";
        case Page::HotkeyProfiles: return s_hotkey_profiles_return_page == Page::ProfilesHub
            ? L"Profiles / Hotkey Profiles" : L"Settings / Hotkeys / Hotkey Profiles";
        case Page::ConfirmDeleteHotkeyProfile: return s_hotkey_profiles_return_page == Page::ProfilesHub
            ? L"Profiles / Hotkey Profiles / Delete" : L"Settings / Hotkeys / Hotkey Profiles / Delete";
        case Page::CustomisationTime: return L"Customisation / Time";
        case Page::CustomisationSky: return L"Customisation / Sky";
        case Page::CustomisationSkyColours: return L"Customisation / Sky / Sky Colours";
        case Page::CustomisationSkyVisibility: return L"Customisation / Sky / Sky Visibility";
        case Page::CustomisationFogColours: return L"Customisation / Sky / Fog Colours";
        case Page::CustomisationFogVisibility: return L"Customisation / Sky / Fog Visibility";
        case Page::CustomisationPillars: return L"Customisation / Pillars";
        case Page::CustomisationShadows: return L"Customisation / Shadows";
        case Page::WorldPlayer: return L"Customisation / Camera FOV";
        case Page::WorldSections: return L"World / Sections";
        case Page::WorldIndividualSections: return L"World / Sections / Individual";
        case Page::WorldSectionHeights: return L"World / Sections / Positions";
        case Page::WorldSectionSpeed: return L"World / Sections / Speed";
        case Page::WorldIndividualSectionHeights: return L"World / Sections / Positions / Select";
        case Page::WorldSectionPositionEdit: return L"World / Sections / Positions / Edit";
        case Page::WorldButtons: return L"World / Buttons";
        case Page::WorldButtonActions: return L"World / Buttons";
        case Page::WorldButtonColours: return L"Customisation / Button Colours";
        case Page::WorldButtonBeacons: return L"Customisation / Button Beacons";
        case Page::WorldTiles: return L"World / Tiles & Walls";
        case Page::WorldTileActions: return L"World / Tiles";
        case Page::WorldTileColours: return L"Customisation / Tile Colours";
        case Page::WorldWalls: return L"Customisation / Wall Colours";
        case Page::WorldTubes: return L"World / Tubes";
        case Page::WorldTubeColours: return L"Customisation / Tube Colours";
        case Page::WorldTubeSpeed: return L"World / Tubes";
        case Page::WorldPlatforms: return L"World / Platforms";
        case Page::WorldPlatformColours: return L"Customisation / Platform Colours";
        case Page::WorldPlatformSpeed: return L"World / Platforms";
        case Page::WorldWater: return L"World / Water";
        case Page::WorldWaterColours: return L"Customisation / Water";
        case Page::WaterObjectReflections: case Page::WaterBeamReflections:
        case Page::WaterReflections: return L"Customisation / Water Reflections";
        case Page::WorldJumpPads: return L"World / Jump Pads";
        case Page::WorldJumpPadColours: return L"Customisation / Jump Pad Colours";
        case Page::WorldJumpPadHeight: return L"World / Jump Pads";
        case Page::SaveStates: return L"World / Run & Practice";
        case Page::SavePositions: return L"World / Run & Practice / Positions & Teleport";
        case Page::TeleportCoordinates: return L"World / Run & Practice / Teleport / XYZ";
        case Page::SavedCheckpoints: return L"World / Run & Practice / Save States";
        case Page::ButtonCheckpoints: return L"World / Run & Practice / Button Checkpoints";
        case Page::Practice: return L"World / Run & Practice / Section Practice";
        case Page::Hud: return L"HUD";
        case Page::Splits: return L"Splits";
        case Page::SplitHud: return L"Splits / Split HUD Elements";
        case Page::SplitHudAppearance: return L"Splits / Split HUD Customisations";
        case Page::HudTiming: return L"Splits / Popups & Comparisons";
        case Page::SplitDisplay: return L"Splits / Popup Rows";
        case Page::HudComparisons: return L"Splits / Profile Comparison";
        case Page::HudRecordTimes: return L"Splits / Saved Times";
        case Page::HudGameplay: return L"HUD / Side HUD Elements";
        case Page::HudKeyDisplay: return L"Inputs";
        case Page::HudPositions: return L"HUD / Positions";
        case Page::HudAppearance: return L"HUD / Customisations";
        case Page::HudAppearanceEdit:
            return s_hud_style_return_page == Page::HudKeyDisplay
                ? L"Inputs / Customisations"
                : s_hud_style_return_page == Page::SplitHudAppearance
                    ? L"Splits / Split HUD Customisations / Selected Element"
                    : L"HUD / Customisations / Selected Element";
        case Page::Settings: return L"Settings";
        case Page::Logs: return L"Settings / Logs";
        case Page::ConfirmClearLogs: return L"Settings / Logs / Confirm Clear";
        case Page::Ghost: return L"Ghost";
        case Page::GhostReplay: return L"Replay";
        case Page::Hotkeys: return L"Settings / Hotkeys";
        case Page::HotkeysGeneral: return L"Settings / Hotkeys / General & Restarts";
        case Page::HotkeysMovement: return L"Settings / Hotkeys / Movement & Jump";
        case Page::HotkeysPractice: return L"Settings / Hotkeys / Saves & Practice";
        case Page::HotkeysHUD: return L"Settings / Hotkeys / HUD & Timers";
        case Page::HotkeysWorld: return L"Settings / Hotkeys / Time, Maps & World";
        case Page::HotkeysGhost: return L"Settings / Hotkeys / Ghost & Replay";
        case Page::Persistence: return L"Settings / Persistence";
        case Page::GameplayRecords: return L"Profiles / Replay & Splits / Record Saving";
        case Page::Profiles: return L"Profiles / Replay & Splits";
        case Page::ProfilesHub: return L"Profiles";
    }
    return L"";
}

std::vector<std::wstring> Entries() {
    auto items = BuildEntries();
    // Offer Hide on every submenu without changing the indices of its
    // existing commands or the Back item. F5 provides the same action.
    if (g_page != Page::Main)
        items.emplace_back(L"Hide menu  [" + MenuHotkeyDisplayName() + L"]");
    return items;
}

// A description belongs to a selectable ROW, not merely to its submenu.
// Keep these indices aligned with BuildEntries() and ActivateSelected().
// Back/Hide are added after the page-specific rows and share descriptions.
namespace {
std::wstring DescriptionAt(int index, std::initializer_list<const wchar_t*> descriptions) {
    if (index < 0) return L"";
    for (const wchar_t* description : descriptions) {
        if (index-- == 0) return description;
    }
    return L"";
}
} // namespace

void PollHudColourPicker() {
    // The Win32 dialog never writes HUD state. Consume its result on the
    // character tick, so navigation and game rendering cannot race the picker.
    const uint32_t result = s_hud_picker_result.exchange(
        kHudPickerNoResult, std::memory_order_acq_rel);
    if (result <= 0xFFFFFFu && s_hud_picker_target < kHudElementCount) {
        const size_t target = s_hud_picker_target; // Captured when picker opened.
        g_hud_custom_colours[target] = result;
        g_hud_colour_modes[target] = 10;
        SaveSettings();
        Notify(std::wstring(kHudPositionLabels[target]) +
               L" colour set to #" + HudRgbString(result));
    } else if (result <= 0xFFFFFFu && s_hud_picker_target == kGhostColourPickerTarget) {
        SetPBGhostColourRgb(result);
        Notify(L"Ghost colour set to #" + HudRgbString(result));
    } else if (result <= 0xFFFFFFu && s_hud_picker_target == kCrosshairColourPickerTarget) {
        g_crosshair.rgb = static_cast<int>(result);
        EnableCrosshairEditing(g_crosshair);
        SaveSettings();
        Notify(L"Crosshair colour set to #" + HudRgbString(result));
    } else if (result == kHudPickerError) {
        Notify(L"Windows colour picker failed to open");
    }
    if (result != kHudPickerNoResult) s_hud_picker_target = kHudElementCount;
}

std::wstring MenuSelectionDescription(Page page, int selected,
                                      const std::vector<std::wstring>& entries) {
    if (selected < 0 || static_cast<size_t>(selected) >= entries.size()) return L"";
    const std::wstring& label = entries[static_cast<size_t>(selected)];
    if (label == L"Back") return L"Return to the previous menu without changing this setting.";
    if (label.rfind(L"Hide menu", 0) == 0)
        return L"Close the menu. Reopen it with your menu hotkey at the same page and row.";

    switch (page) {
        case Page::Search:
            if (selected == 0) return L"Type any part of a setting or module name. Use spaces to combine terms, Up/Down to choose a result, and Enter to jump directly to that setting. Search never changes the setting itself.";
            if (selected >= 1 && static_cast<size_t>(selected) <= s_menu_search_matches.size() &&
                !s_menu_search_query.empty()) {
                const auto& item = s_menu_search_matches[static_cast<size_t>(selected - 1)];
                return L"Go to " + item.breadcrumb + L" and highlight '" + item.label +
                       L"'. This does not execute or toggle the setting.";
            }
            return L"Clear the current search or return to the menu you were browsing.";
        case Page::Main:
            return DescriptionAt(selected, {
                L"Control flight, noclip, walking speed, jump behaviour and game speed. Movement changes can alter gameplay and may affect whether a run is eligible for saved records.",
                L"Adjust the actual level: sections, buttons, tiles, cubes, water and moving platforms. You can also restart, save a checkpoint or practise specific buttons from Run & Practice.",
                L"Browse Refunct TAS v1 JSON maps stored in RefunctUtility\\Maps. Loading a map applies its authored section and actor state; use Restore Original World to return to the state captured before the custom map.",
                L"Change how the level looks, including camera FOV, colours for buttons, tiles, walls, platforms and water, plus the day/night appearance.",
                L"Configure non-split overlays: FPS, coordinates, clocks and status counters. Input visualisation and split timing have their own modules.",
                L"Configure run timer, PB and segment overlays, split popups, profile comparisons, saved times and split display customisations.",
                L"Show W/A/S/D, Space and Shift key presses on screen and customise the input display's position, colours, size and header. It does not modify controls or gameplay.",
                L"Show or hide your active split profile's ghost, switch between 2D and 3D, and adjust transparency and playback settings.",
                L"Watch saved PB or best-segment replays and select a replay/split profile without leaving this menu.",
                L"Manage replay/split, world customisation and HUD profiles from one menu. Each profile type stores independent data.",
                L"Set keyboard shortcuts and control which movement and world settings persist across game and map restarts.",
                L"Search across all utility menus by typing a module or option name. Select a result to jump to its original menu row without activating it."
            });
        case Page::Maps:
            if (selected == 0) return L"Rescan the game's RefunctUtility\\Maps folder for map files.";
            if (selected == 1 || selected == 2) return L"Browse a different page of map files.";
            if (selected >= 3 && static_cast<size_t>(selected) < 3 + MapsOnPage())
                return L"Load the chosen Refunct TAS v1 world layout, updating its 31 sections and relevant native actors. This modifies world state and counts as assisted gameplay.";
            return L"Restore the world captured before the first custom map was loaded.";
        case Page::RestartOptions:
            return DescriptionAt(selected, {
                L"Restart from the beginning of the run using your normal restart key.",
                L"Restart with previously prepared level data for a quicker reset. This is different from a full level reload, which opens the map again.",
                L"Restart the run without discarding your completed-button progression. Useful for revisiting the level with the current progress retained; this is not a clean PB attempt.",
                L"Choose whether sections visibly rise and lower when the run restarts or a saved state is restored. Turning animation off makes those changes happen immediately.",
                L"Reload the current WorldBase level without closing Refunct. This destroys the current world and creates a fresh one; your active run is interrupted."
            });
        case Page::ConfirmLevelReload:
            return DescriptionAt(selected, {
                L"Confirm the full level reload. Your current run is interrupted.",
                L"Cancel the reload and return to restart controls."
            });
        case Page::Movement:
            return DescriptionAt(selected, {
                L"Turn flight and collision bypass on or off, and choose whether switching modes preserves momentum. These options affect how the character moves through the world.",
                L"Adjust individual movement multipliers for flying, walking, swimming, climbing and sliding, plus jump height, acceleration and game speed. Values are relative to their normal behaviour.",
                L"Choose when scrolling up or down causes a jump and when holding Space repeats jumps. Ground, wall and air-jump options are separate so you can tune each behaviour.",
                L"Return movement controls to their original utility defaults. This does not reset level progress, your split files or visual customisation settings."
            });
        case Page::MovementModes:
            return DescriptionAt(selected, {
                L"Switch into free flight to move around the level without normal ground movement. Flight controls change how your character responds to gravity and movement input.",
                L"Ignore collision with walls and other solid objects while this mode is active. You may pass through level geometry, so turn it off before resuming a normal run.",
                L"Preserve your current velocity when entering or leaving an alternate movement mode. Turn it off if you want mode changes to discard that momentum instead."
            });
        case Page::MovementPhysics:
            return DescriptionAt(selected, {
                L"Adjust how fast you fly. Use Left/Right or Enter to change it.",
                L"Multiply the speed of ordinary ground movement. This changes how far you can travel in the same amount of time and is separate from acceleration.",
                L"Multiply movement speed while the character is swimming. This controls the swim movement rate, not water height or whether water is enabled.",
                L"Adjust ledge climbing speed without altering jumps or walking.",
                L"Control how quickly your character travels during a slide. Slide speed and slide distance are separate settings, so increasing one does not automatically change the other.",
                L"Change the distance travelled during a slide independently of slide speed. Larger values can make slides carry you farther after starting them.",
                L"Multiply the height reached by normal jumps. This is separate from the jump-pad launch-height setting under World.",
                L"Adjust how quickly your character accelerates toward its movement speed. High speed with low acceleration can still take time to reach full pace.",
                L"Scale the speed of the entire simulation, not only the player. This can affect the run timer and gameplay timing; use normal speed for an unmodified run."
            });
        case Page::MovementJump:
            return DescriptionAt(selected, {
                L"Enable assisted jumping by scrolling the mouse wheel up or down. Choose which surfaces allow it so the wheel can trigger jumps from the ground, walls or both.",
                L"Keep jumping automatically while Space is held, according to the selected ground/wall rules. Disable it if you want one jump per key press.",
                L"Allow an additional jump input during a fall, rather than only while grounded. This changes ordinary movement and is separate from scroll direction."
            });
        case Page::Persistence:
            return DescriptionAt(selected, {
                L"When ON, movement tuning, flight and jump helpers survive closing and reopening Refunct. OFF starts with vanilla movement.",
                L"When ON, movement changes survive a normal map restart. OFF restores vanilla movement; practice checkpoint loads are exempt.",
                L"When ON, saved World gameplay settings (speeds, water and object-removal toggles) survive relaunch. FOV is a customisation and persists independently. Manual section, button, tile and cube actions do not become saved progress.",
                L"When ON, World gameplay settings survive normal map restarts. OFF restores those settings to defaults; native one-time actions and saved progress remain controlled by the game."
            });
        case Page::GameplayRecords:
            return DescriptionAt(selected, {
                L"Allow a finished, eligible run to replace the active split profile personal best when it is faster. Practice checkpoints and restricted assisted runs are not treated as normal full-run PBs.",
                L"Save a newly fastest individual button segment when permitted, even if the whole run was not a new PB. The sum of these best segments can come from different attempts.",
                L"Save a new PB movement ghost and your furthest-run ghost for this split profile. OFF prevents new ghost recordings but does not delete existing files. Save with cheats permissions also apply.",
                L"Save a movement recording for each newly earned fastest button segment in this profile. OFF does not affect fastest segment times. Save with cheats permissions also apply.",
                L"Allow cheat-assisted PBs and/or fastest segments, including their corresponding ghosts. OFF forbids cheat-assisted saves; PB only, Fastest segments only, and Both grant exactly those permissions. Synthetic progress and practice restrictions remain in force.",
                L"When a new PB is successfully saved, also create a numbered snapshot profile, such as Any%_PB_001. Keep the active category profile selected and updating normally. OFF by default.",
                L"When creating an automatic PB snapshot, copy its independently earned fastest button segments and their Run/Practice source labels. OFF creates a snapshot with only the new full-run PB, its splits and run rules."
            });
        case Page::Ghost:
            return DescriptionAt(selected, {
                L"Show or hide the current split profile's ghost. Recording continues when hidden, including fastest button sections and the furthest unfinished run.",
                L"3D: world-space no-collision ghost you can walk through. 2D: the original screen-space cyan silhouette.",
                L"ON: see-through cyan 2D silhouette. OFF: the same cyan silhouette with fully solid rectangles.",
                L"ON: translucent ghost using native M_Glass_Inst. OFF: solid-colour ghost using native opaque M_Basic (no developer texture). Ghost collision remains disabled.",
                L"Choose one colour for both 2D and 3D ghosts. Enter a six-digit RGB HEX value or switch to the Windows colour picker with Left/Right. Transparency settings remain independent.",
                L"ON: each ghost section starts when its corresponding native button is pressed; the ghost waits if it arrives first. OFF: play the chosen ghost source continuously.",
                L"ON: use the fastest saved movement for each button section, including incomplete runs. Missing sections fall back to the furthest clean run or PB when available. OFF: prefer the full PB run, or the furthest saved run when no PB exists."
            });
        case Page::GhostReplay:
            return DescriptionAt(selected, {
                L"Start a native button-by-button replay. The level restarts at replay start; stopping after a replay press resets it again. Saved PBs and ghost files are unchanged.",
                L"Cycle saved replay/split profiles with Left/Right or Enter. Changing profile stops active playback and loads that profile for both replay and normal splits/ghosts. Restart before recording a new PB.",
                L"Choose PB or Best segments independently of the normal Ghost setting. PB replays only the completed PB; Best segments uses saved gold tracks and PB fallbacks for missing golds. Switching stops an active replay.",
                L"Pause/resume recorded movement and button activation. The live game timer is unaffected by the replay clock.",
                L"Replay from the beginning. Already activated native buttons require a level restart.",
                L"Move five seconds earlier; if buttons have been pressed, the replay restarts the level.",
                L"Move five seconds later; native buttons are still activated in order.",
                L"Watch at half, normal or double speed; no recording is modified.",
                L"Replay clock for the spectator viewer. Press F5 to hide the menu and watch native sections rise.",
                L"Camera: Locked third person follows the recorded yaw and pitch. Third person uses mouse orbit and scroll wheel up/down to zoom, even with F5 open. First person is eye-level with 130 degree FOV. Free cam uses Fly movement.",
            });
        case Page::World:
            return DescriptionAt(selected, {
                L"Raise, lower or move sections, change rise animations and reset placements.",
                L"Restart, save and load positions, or practise from button checkpoints.",
                L"Manage button availability, ghost-button repair and remove native buttons.",
                L"Select or deselect tiles, disable touch selection or remove tile geometry.",
                L"Collect, restore or remove cube pickups without conflating removal with collection.",
                L"Adjust tube travel speed or remove travel pipes and collision.",
                L"Adjust moving-platform speed or remove native lifts and movers.",
                L"Adjust jump-pad launch height or remove jump pads and launch collision.",
                L"Manage water rendering, swimming, height and related options.",
                L"Remove all six object categories. OFF restores each category's saved actor states.",
                L"Turn all six object-removal toggles OFF and restore their native actor states.",
                L"Manage independent World profiles for persistent gameplay preferences.",
                L"Restore World gameplay settings without changing PB records or cosmetic colours."
            });
        case Page::WorldObjectRemoval:
            return DescriptionAt(selected, {});  // Legacy reserved page.
        case Page::WorldCubes:
            return DescriptionAt(selected, {
                L"Hide cubes that have not been collected and disable their pickup collision, without marking them as collected. Restore cubes later to make them available again.",
                L"Trigger the normal collection behaviour for all 18 cubes. This changes progression rather than only hiding the cube meshes.",
                L"Restore availability of uncollected cubes throughout the map, including cubes below the current waterline.",
                L"Restore only uncollected cubes positioned above the current water surface. Underwater cubes stay unavailable.",
                L"Restore the original cube visibility and collision settings. This is meant to undo availability changes, not erase your completed progress.",
                L"Remove cubes from the current world: hide native actors and disable collision. Toggle OFF to restore their captured state."
            });
        case Page::Customisation:
            return DescriptionAt(selected, {
                L"Choose centre and border colours independently for pressed and unpressed buttons. These are appearance changes, not button-availability controls.",
                L"Show or hide button beacons and customise their colour.",
                L"Change selected and unselected tile colours, including Random RGB.",
                L"Change the selected and unselected wall colours.",
                L"Change the appearance of travel tubes and pipes.",
                L"Change lift and moving-platform colours.",
                L"Choose a custom colour for jump-pad surfaces.",
                L"Customise the visible water surface and underwater PostWater tint separately. Colour and intensity controls do not change the waterline or swimming physics.",
                L"Change Refunct 24-hour time and day/night progression speed independently of the run timer. Lock Time holds the chosen time, while unlocking lets the sky continue to advance.",
                L"Customise the sky, sun, clouds, stars and atmospheric fog. Open separate colour and visibility pages or restore all defaults.",
                L"Disable directional shadows or change the ambient light tint seen most strongly in shaded areas. Ambient tint can also colour lit areas.",
                L"Customise pillar colours and remove or restore their meshes. Removal also disables pillar collision, but is classified as a customisation for the HUD and PB rules.",
                L"Change the camera field of view without counting it as a cheat or invalidating PB/segment records.",
                L"Control water reflections independently of water colour, including object and beacon exclusions.",
                L"Replace or hide the native crosshair and adjust its colour and dimensions.",
                L"Create and switch between saved sets of visual customisations. Split profiles, gameplay cheats, HUD settings and hotkeys stay independent.",
                L"Restore default camera FOV, visual settings, pillar meshes, time of day and beacon appearance. This does not reset gameplay progress or movement options."
            });
        case Page::Crosshair:
            return DescriptionAt(selected, {
                L"Choose game default, dot, cross, cross plus dot, Off, or editable diamond. The status reports whether the native visibility flag could be cleared.",
                L"Enter a HEX colour or use the Windows colour picker. Left/Right changes the input method.",
                L"Set arm length, dot diameter or diamond diameter. Editing game default selects the editable diamond.",
                L"Set diamond border or cross arm thickness. A solid dot uses size only.",
                L"Adjust the space between the centre and cross arms. Not used for dot or diamond.",
                L"Adjust transparency. Zero hides the crosshair.",
                L"Restore all custom crosshair settings and the native game crosshair."
            });
        case Page::CustomisationProfiles: {
            const size_t count = CustomisationProfileNames().size();
            if (selected < count) return L"Load this saved set of cosmetic settings. The previous active set is automatically saved before switching.";
            switch (selected - count) {
                case 0: return L"Create a new customisation profile copied from your current visual setup, then activate it.";
                case 1: return L"Rename the active customisation profile. Default cannot be renamed.";
                case 2: return L"Save current FOV and world appearance settings to the active customisation profile.";
                case 3: return L"Discard unsaved visual changes and reapply the saved active profile.";
                case 4: return L"Rescan the CustomisationProfiles folder for profiles added outside the game.";
                case 5: return L"Delete the active named customisation profile after confirmation. Default is protected.";
                default: return L"Return to the Customisation menu.";
            }
        }
        case Page::ConfirmDeleteCustomisationProfile:
            return selected == 0 ? L"Permanently remove the selected customisation profile file, then restore Default." : L"Cancel deletion.";
        case Page::WorldProfiles: {
            const size_t count = WorldProfileNames().size();
            if (selected < count) return L"Load this saved set of World gameplay settings. Live edits are saved before switching; automatic persistence resets do not overwrite profiles.";
            switch (selected - count) {
                case 0: return L"Create a new world profile copied from your current World settings, then activate it.";
                case 1: return L"Rename the active world profile. Default cannot be renamed.";
                case 2: return L"Save current persistent World gameplay settings to the active world profile.";
                case 3: return L"Discard unsaved World settings changes and reapply the saved active profile.";
                case 4: return L"Rescan the WorldProfiles folder for profiles added outside the game.";
                case 5: return L"Delete the active named world profile after confirmation. Default is protected.";
                default: return L"Return to the World menu.";
            }
        }
        case Page::ConfirmDeleteWorldProfile:
            return selected == 0 ? L"Permanently remove the selected world profile file, then restore Default." : L"Cancel deletion.";
        case Page::HotkeyProfiles: {
            const int count = static_cast<int>(HotkeyProfileNames().size());
            if (selected >= 0 && selected < count) return L"Load saved keyboard shortcuts. Live keybind edits are saved before switching.";
            switch (selected - count) {
                case 0: return L"Create a new hotkey profile copied from your current keyboard shortcuts, then activate it.";
                case 1: return L"Rename the active hotkey profile. Default cannot be renamed.";
                case 2: return L"Save current keyboard shortcuts to the active hotkey profile.";
                case 3: return L"Discard unsaved keybind changes and reapply the saved active profile.";
                case 4: return L"Rescan the HotkeyProfiles folder for profiles added outside the game.";
                case 5: return L"Delete the active named hotkey profile after confirmation. Default is protected.";
                default: return L"Return to the previous menu.";
            }
        }
        case Page::ConfirmDeleteHotkeyProfile:
            return selected == 0 ? L"Permanently remove the selected hotkey profile file, then restore Default." : L"Cancel deletion.";
        case Page::HudProfiles: {
            const int count = static_cast<int>(HudProfileNames().size());
            if (selected >= 0 && selected < count) return L"Load this saved set of HUD settings. The previous active set is automatically saved before switching.";
            switch (selected - count) {
                case 0: return L"Create a new HUD profile copied from your current HUD setup, then activate it.";
                case 1: return L"Rename the active HUD profile. Default cannot be renamed.";
                case 2: return L"Save current HUD, inputs and split display settings to the active HUD profile.";
                case 3: return L"Discard unsaved HUD changes and reapply the saved active profile.";
                case 4: return L"Rescan the HUDProfiles folder for profiles added outside the game.";
                case 5: return L"Delete the active named HUD profile after confirmation. Default is protected.";
                default: return L"Return to the HUD menu.";
            }
        }
        case Page::ConfirmDeleteHudProfile:
            return selected == 0 ? L"Permanently remove the selected HUD profile file, then restore Default." : L"Cancel deletion.";
        case Page::CustomisationShadows:
            return DescriptionAt(selected, {
                L"Enable or disable the WorldBase directional light's dynamic shadow casting. It does not remove baked shading or ambient occlusion. Restores the native CastShadows value.",
                L"Enable custom ambient cubemap tint on PostNormal. This changes ambient fill (especially noticeable in shadows), not the colour of cast shadow pixels alone.",
                L"Choose the ambient tint with HEX or the Windows colour picker. This may affect other indirectly lit surfaces too.",
                L"Restore directional shadow casting and the original ambient tint."
            });
        case Page::CustomisationPillars:
            return DescriptionAt(selected, {
                L"Use the original tall background-building colours or a custom pillar RGB tint. Gameplay and collision remain unchanged.",
                L"Enter a six-digit HEX colour or open the Windows picker. Both original concrete variants are tinted independently through their own material instances.",
                L"Remove decorative pillar meshes and their collision, or restore them. This is treated as a customisation for the Cosmetics HUD and PB eligibility."
            });
        case Page::CustomisationTime:
            return DescriptionAt(selected, {
                L"Switch between normal time-of-day behaviour and utility customisation. Your speedrun timer is independent from this setting.",
                L"Select the displayed time of day in 15-minute steps. If Lock Time is OFF, the clock keeps moving forward from the time you select.",
                L"ON freezes the day/night clock at the selected time. OFF resumes progression from that time, using the configured time-of-day speed.",
                L"Change how quickly the sky cycles through the day, from 25% to 1000% in 25% steps. It does not directly change player movement or the run timer.",
                L"Restore the original game time and day/night progression speed. This is independent of restoring HUD clocks or split records."
            });
        case Page::WorldPlayer:
            return DescriptionAt(selected, {
                L"Increase or decrease camera FOV in degrees. This is a customisation, not a cheat.",
                L"Restore the game's default camera field of view."
            });
        case Page::WorldSections:
            return DescriptionAt(selected, {
                L"ON uses the normal rise/lower animation when sections move. OFF moves them immediately; instant world changes count as assisted gameplay.",
                L"Move all 31 level sections to their raised positions. This bypasses the need to press each normal progress button.",
                L"Move all 31 level sections down together. This changes level geometry and can affect where the character can stand or travel.",
                L"Return section placement to game-controlled mode so later button events can move them normally. Manual section offsets are handled separately.",
                L"Reconcile which buttons should currently be usable based on section progression. This differs from enabling every button in Button Controls.",
                L"Pick an individual section and choose Auto, Raised or Lowered. Auto lets native game progress control the section; the other choices force a position.",
                L"Shift section geometry on the X, Y and Z axes, globally or per section. These offsets alter the actual level layout rather than just its appearance.",
                L"Change the speed used by section rise and lower animations. The setting matters when animations are enabled, not when sections move instantly."
            });
        case Page::WorldSectionSpeed:
            return DescriptionAt(selected, {
                L"Change rise speed in 25% steps. Used when section animation is ON (assisted run).",
                L"Restore original 100% rise speed (used when animation is ON)."
            });
        case Page::WorldSectionHeights:
            return DescriptionAt(selected, {
                L"Shift all 31 platforms along the X axis in 50 cm steps.",
                L"Shift all 31 platforms along the Y axis in 50 cm steps.",
                L"Shift all 31 platforms vertically in 50 cm steps.",
                L"Choose a specific level section to edit, then adjust its X, Y and Z offsets independently. This lets you move one section without shifting every other section.",
                L"Clear all saved manual X, Y and Z section offsets. Normal game-controlled raised/lowered behaviour remains available."
            });
        case Page::WorldIndividualSections:
            if (selected < kPracticeClusterCount)
                return L"Use Left/Right to cycle this section through Auto, Raised and Lowered. Auto follows normal game progress; the other modes force the section state.";
            break;
        case Page::WorldIndividualSectionHeights:
            if (selected < kPracticeClusterCount)
                return L"Open position controls for this section. Adjust its X, Y and Z offsets without moving the other sections.";
            break;
        case Page::WorldSectionPositionEdit:
            return DescriptionAt(selected, {
                L"Move this section along the X axis, 50 cm per step.",
                L"Move this section along the Y axis, 50 cm per step.",
                L"Move this section up or down on the Z axis, 50 cm per step.",
                L"Reset the selected section's X, Y and Z offsets."
            });
        case Page::WorldButtons:
            return DescriptionAt(selected, {
                L"Control native button availability above and below water, or restore normal progression locks. Automatic ghost detection watches for a previously working next button whose native activation gates become broken. Simply standing near a button no longer triggers a repair. If collision seems healthy but the button will not activate, use the manual repair command.",
                L"Set separate centre and border colours for pressed and unpressed buttons. Colours do not change whether a button can be activated.",
                L"Hide the tall button beacons or change their light colour."
            });
        case Page::WorldButtonActions:
            return DescriptionAt(selected, {
                L"Restore the normal enabled/disabled button arrangement for the current progression. Unlike Enable All, this keeps future progression buttons locked until they are needed.",
                L"Enable the buttons that are currently above water. Underwater buttons are not included; this changes availability, not section progression.",
                L"Enable all 37 native buttons, including underwater buttons and those normally locked by progression. This changes gameplay state.",
                L"Disable buttons currently positioned below the water surface. Buttons above water are not deliberately changed.",
                L"Disable buttons currently above water while leaving underwater ones alone. Use Restore Default to return to progression-based availability.",
                L"Disable every native button, whether pressed, unpressed, above water or underwater. You can restore default or enable them again afterward.",
                L"Activate currently available buttons in their game progression order. This advances level progress instead of simply making buttons visually available.",
                L"Immediately refresh native activation and contact collision for currently usable buttons. Use this when a button looks enabled but pressing or landing on it does nothing: automatic monitoring cannot confirm contact-only faults. Manual repair changes game state and marks this run as assisted, even if the button was already healthy.",
                L"Prevent button presses from selecting tiles beneath them. Ordinary tile contact can still select tiles unless you disable that separately.",
                L"Remove buttons from the current world: hide native actors and disable collision. Toggle OFF to restore their captured state."
            });
        case Page::WorldButtonColours:
            return DescriptionAt(selected, {
                L"Use the original or a custom centre colour on unpressed buttons.",
                L"Enter a HEX colour or open the Windows picker for unpressed centres.",
                L"Use the original or a custom centre colour on pressed buttons.",
                L"Enter a HEX colour or open the Windows picker for pressed centres.",
                L"Use the original or a custom border colour on unpressed buttons.",
                L"Enter a HEX colour or open the Windows picker for unpressed borders.",
                L"Use the original or a custom border colour on pressed buttons.",
                L"Enter a HEX colour or open the Windows picker for pressed borders."
            });
        case Page::WorldButtonBeacons:
            return DescriptionAt(selected, {
                L"Show or hide the tall beacon above each button.",
                L"Use the original beacon colour or enable your custom colour.",
                L"Choose the custom beacon colour using HEX or the Windows picker."
            });
        case Page::WorldTiles:
            return DescriptionAt(selected, {
                L"Select, deselect or disable touch activation of floor tiles.",
                L"Choose colours for selected and unselected tiles.",
                L"Choose colours for selected and unselected walls."
            });
        case Page::WorldTileActions:
            return DescriptionAt(selected, {
                L"Select tiles that are currently above the waterline. Tiles below water stay unchanged.",
                L"Select every tile in the map, including those currently underwater. This changes the tile-selection progress state.",
                L"Deselect selected tiles that are currently above the waterline. Underwater tiles stay unchanged.",
                L"Deselect every tile in the map, including those under water. This changes tile progress rather than merely recolouring it.",
                L"Turn off selection caused by touching tiles. This prevents normal contact from selecting new tiles; button-triggered selection has its own setting.",
                L"Remove tiles from the current world: hide native actors and disable collision. Toggle OFF to restore their captured state."
            });
        case Page::WorldTileColours:
            return DescriptionAt(selected, {
                L"Select the preset or custom appearance of selected tiles.",
                L"Edit the selected-tile custom HEX colour or use the Windows picker.",
                L"Give newly selected tiles random RGB colours.",
                L"Select the preset or custom appearance of unselected tiles.",
                L"Edit the unselected-tile HEX colour or use the Windows picker."
            });
        case Page::WorldWalls:
            return DescriptionAt(selected, {
                L"Use the original or custom appearance for selected walls.",
                L"Edit the selected-wall HEX colour or use the Windows picker.",
                L"Use the original or custom appearance for unselected walls.",
                L"Edit the unselected-wall HEX colour or use the Windows picker."
            });
        case Page::WorldWater:
            return DescriptionAt(selected, {
                L"Remove all water effects together: hide its visible surface, disable swimming and underwater tint, and hide identifiable splashes. Turning this OFF restores your individual water options.",
                L"Hide the visible ocean mesh without removing the swimming volume. If Remove Water is ON, it overrides this visual-only choice.",
                L"Prevent swimming so the player falls through water using normal gravity. This does not remove the visible water mesh unless you hide it separately.",
                L"Move the visible ocean and verified swimming volume in steps of 100 Unreal units. The original cooked water-volume boundary does not move, so transitions may behave differently.",
                L"Return the ocean surface to its starting height. This removes the custom vertical offset without resetting your colour, tint or splash settings.",
                L"Open water visual settings for surface colour, underwater tint, tint intensity and splashes. These controls do not raise the swimming surface."
            });
        case Page::CustomisationSky:
            return DescriptionAt(selected, {
                L"Change the zenith, horizon, sun-disc and cloud colours using HEX or the Windows colour picker.",
                L"Show or hide the Moon/Sun, clouds and stars independently without changing world lighting.",
                L"Change general fog haze and directional fog colours independently.",
                L"Hide or restore general fog and directional fog independently.",
                L"Restore sky, cloud, sun and fog colours and visibility to game defaults. Saved custom HEX selections remain available."
            });
        case Page::CustomisationSkyColours:
            return DescriptionAt(selected, {
                L"Switch between the game's automatic upper-sky colour and a custom zenith colour. This changes BP_DynamicSky's sky material only, not the ocean, fog, platforms or ambient lighting.",
                L"Choose a colour for the upper part of the sky. Left/Right selects HEX entry or the Windows picker; Enter edits. Choosing a colour also enables its override.",
                L"Override the sky colour toward the horizon independently of the zenith. The game's day/night gradient can continue to affect other sky effects.",
                L"Choose the sky's horizon colour using a HEX code or the Windows colour picker. Cancel preserves the previous colour and does not enable the override.",
                L"Switch the sun disc between Refunct's dynamic day/night colour and your custom colour. This affects the sky material, not world lighting or shadows.",
                L"Choose a HEX colour or use the Windows colour picker for the sun disc. Choosing a colour also enables its override.",
                L"Override the sky material's native Cloud color vector, independently of the sky gradient and cloud opacity. This is cosmetic only.",
                L"Choose the colour of the clouds using HEX or the Windows colour picker. The setting is saved and is retained when clouds are hidden."
            });
        case Page::CustomisationSkyVisibility:
            return DescriptionAt(selected, {
                L"Show or hide the Moon/Sun sky disc with one toggle. OFF suppresses the sky material's native brightness; ON restores the last captured non-zero value and releases native control. Sunlight and shadows are unaffected.",
                L"Show or hide sky clouds by controlling the sky material's Cloud opacity. If the native scalar name isn't available at runtime, the mod logs why it cannot apply the override.",
                L"Show or hide night-time stars through the sky material's Stars brightness. The native day/night cycle remains active and original brightness is restored when enabled."
            });
        case Page::CustomisationFogColours:
            return DescriptionAt(selected, {
                L"Override the Exponential Height Fog's general inscattering colour, which controls distant atmospheric haze without changing fog density.",
                L"Choose the colour of distant fog using a HEX code or Windows colour picker.",
                L"Override the sun-facing directional fog inscattering colour separately from the general haze and sun disc.",
                L"Choose a HEX colour or use the Windows colour picker for directional fog."
            });
        case Page::CustomisationFogVisibility:
            return DescriptionAt(selected, {
                L"Hide all exponential height fog by temporarily setting its verified native density to zero. ON restores the original density. Your custom fog colour is preserved.",
                L"Hide only the directional sun-facing fog contribution by suppressing its native inscattering colour. The general fog remains active, and its saved custom colour is preserved."
            });
        case Page::WorldWaterColours:
            return DescriptionAt(selected, {
                L"Enable a custom colour on the rendered ocean surface, or switch back to the original material colour. This does not recolour the underwater view.",
                L"Enter a six-digit surface HEX colour or use the Windows picker.",
                L"Adjust how strongly the custom surface colour is applied. Zero uses the game default, 100% is normal strength and higher values can boost HDR brightness.",
                L"Enable custom colour on the PostWater underwater view. This changes the screen tint when underwater, separately from the visible surface colour.",
                L"Enter an underwater HEX colour or use the Windows picker.",
                L"Change the intensity of the underwater colour effect. Zero is the default, 100% is normal strength and values above 100% can increase HDR colour intensity.",
                L"Replace the underwater tint with neutral white while enabled. Disabling this option returns to your saved original or custom underwater colour.",
                L"Hide identifiable water splash and ripple particles. Other unrelated particle effects are left alone.",
            });
        case Page::WaterReflections:
            return DescriptionAt(selected, {
                L"Enable or disable the level's native planar reflection capture. Unlike Shadows visible, this does not change directional lighting or ocean material colour. Confirm the visual result in-game.",
                L"Exclude the island wall and moving platform meshes from the water capture only.",
                L"Exclude the button press surface and border meshes from the water capture only.",
                L"Exclude identified travel-pipe meshes from the water capture only.",
                L"Exclude jump pad visual meshes from the water capture only. Launches and actual mesh visibility are unchanged.",
                L"Exclude collectible cube meshes from the water capture only. Collection and actual mesh visibility are unchanged.",
                L"Exclude the button Beacon primitive component from planar reflections only. Actual beacon visibility is unchanged."
            });
        case Page::WaterObjectReflections: case Page::WaterBeamReflections:
            return DescriptionAt(selected, {});  // Legacy reserved pages.
        case Page::WorldJumpPads:
            return DescriptionAt(selected, {
                L"Use the original or a custom colour on jump pad surfaces.",
                L"Change the vertical height of jump pad launches, not normal jumps."
            });
        case Page::WorldJumpPadColours:
            return DescriptionAt(selected, {
                L"Enable or disable a custom colour for the metallic pad surface.",
                L"Enter a HEX colour or open the Windows colour picker."
            });
        case Page::WorldJumpPadHeight:
            return DescriptionAt(selected, {
                L"Adjust launch height in 25% steps, without a fixed menu maximum.",
                L"Restore normal 100% jump pad launch height.",
                L"Remove jump pads from the current world: hide native actors and disable collision. Toggle OFF to restore their captured state."
            });
        case Page::WorldPlatforms:
            return DescriptionAt(selected, {
                L"Choose original or custom moving-platform colours.",
                L"Scale how quickly native lifts and moving platforms travel. This adjusts platform timelines, not the character walk speed or game-wide time scale."
            });
        case Page::WorldPlatformColours:
            return DescriptionAt(selected, {
                L"Switch between game-default lift colours and custom RGB.",
                L"Enter a six-digit HEX value or use the Windows colour picker."
            });
        case Page::WorldPlatformSpeed:
            return DescriptionAt(selected, {
                L"Adjust moving-platform speed in 25% steps (no UI maximum).",
                L"Restore the game's normal moving-platform timeline speed.",
                L"Remove moving platforms from the current world: hide native actors and disable collision. Toggle OFF to restore their captured state."
            });
        case Page::WorldTubes:
            return DescriptionAt(selected, {
                L"Choose the original or a custom tube material colour.",
                L"Change your travel speed inside pipes independently of walking and swimming speed. Extreme settings may change how traversal feels."
            });
        case Page::WorldTubeSpeed:
            return DescriptionAt(selected, {
                L"Adjust pipe travel speed in 25% steps (no fixed maximum); assisted run.",
                L"Restore the game's normal 100% pipe travel speed.",
                L"Remove tubes from the current world: hide native actors and disable collision. Toggle OFF to restore their captured state."
            });
        case Page::WorldTubeColours:
            return DescriptionAt(selected, {
                L"Switch between the original tube appearance and custom RGB.",
                L"Enter a six-digit HEX colour or use the Windows colour picker."
            });
        case Page::SaveStates:
            return DescriptionAt(selected, {
                L"Choose between normal restart, preloaded restart, restart with progress and full-level reload. These methods differ in what level state is retained.",
                L"Save your current location, restore it later or teleport to entered X/Y/Z coordinates. These actions move the player but do not load a different map.",
                L"Store a practice state containing both your position and completed-button progress, then reload both. This is different from saving position alone.",
                L"Load a checkpoint around the last or second-last completed button. Momentum preservation can be configured for last-button replay.",
                L"Select a specific button/section and start practice near that point. Practice runs are not treated as normal uninterrupted PB attempts."
            });
        case Page::SavePositions:
            return DescriptionAt(selected, {
                L"Store the exact player position for later recall. This saves coordinates only; use a full checkpoint if you also need the button-progress state.",
                L"Move the player back to the previously saved position. This is a teleport, not a full checkpoint restore.",
                L"Set an exact X, Y and Z location and teleport there. The entered coordinates are world coordinates rather than distances from your current position.",
                L"Teleport to the latest real button press, across maps and restarts; this does not switch maps or reset progress. The target is the last button position remembered globally.",
                L"Read-only status showing whether a position has been saved."
            });
        case Page::TeleportCoordinates:
            return DescriptionAt(selected, {
                L"Edit the target X coordinate; press Enter to type a value.",
                L"Edit the target Y coordinate; press Enter to type a value.",
                L"Edit the target Z coordinate; press Enter to type a value.",
                L"Teleport to the entered X, Y and Z coordinates.",
                L"Fill the target coordinates using your current position."
            });
        case Page::SavedCheckpoints:
            return DescriptionAt(selected, {
                L"Capture your current position and completed-button progress in one saved state. Use Save Position instead if you only want to remember coordinates.",
                L"Restore both the saved player position and saved button progression. This changes more than a simple position teleport and is intended for practice.",
                L"Read-only status showing whether a saved state is available."
            });
        case Page::ButtonCheckpoints:
            return DescriptionAt(selected, {
                L"Move to the checkpoint for the most recently completed button. Intended for replaying that part of the level without starting a fresh normal run.",
                L"Move to the checkpoint before the previous button, letting you practise a slightly longer sequence. It does not erase your saved PB file.",
                L"When enabled, last-button checkpoint loading preserves velocity and movement direction. Turn it off to resume without the previous momentum.",
                L"Read-only status of the most recently completed button.",
                L"Read-only status of the second-latest completed button."
            });
        case Page::Practice:
            return DescriptionAt(selected, {
                L"Choose which button or section to practise with Left/Right. The selected starting point is used when you load the practice checkpoint.",
                L"Start practice at the selected section instead of a normal beginning-of-level run. Practice restrictions prevent these attempts from replacing a regular full-run PB.",
                L"Read-only status showing whether section practice is active."
            });
        case Page::Hud:
            return DescriptionAt(selected, {
                L"Show or hide non-split overlays such as FPS, coordinates, clocks and status counters. Split timing and the WASD input visualiser have their own main-menu modules.",
                L"Customise the position, size, colour, header and supported data views of non-split HUD elements. Split display appearance is under Splits and key display appearance is under Inputs.",
                L"Show detailed help for the currently selected menu setting. Longer descriptions expand into multiple lines.",
                L"Restore default HUD visibility, placement, colour, size, header and popup settings, including split displays. PB records and profiles are preserved.",
                L"Save and switch complete HUD layouts, including Inputs and split popups. Split records, hotkeys and world customisations are independent."
            });
        case Page::Splits:
            return DescriptionAt(selected, {
                L"Show or hide the run timer, full-run PB, sum of best, last-button time and saved segment comparison overlays.",
                L"Configure button-triggered popups and the reference profile or comparison mode used by split differences.",
                L"Show or hide the saved PB, sum-of-best and segment-time information, and select cumulative versus individual segment views.",
                L"Change the split HUD elements' positions, colours, text sizes, headers and display views independently."
            });
        case Page::HudTiming:
            return DescriptionAt(selected, {
                L"Show a temporary summary when you hit a button. This is independent of the persistent run timer under Side HUD Elements.",
                L"Choose the rows in the button popup: cumulative run time, overall PB difference, gain or loss at this button, section references and other comparisons.",
                L"Select the other profile used for split comparisons and whether its reference is a completed PB or a sum of fastest segments."
            });
        case Page::HudRecordTimes:
            return DescriptionAt(selected, {
                L"Show the saved fastest completed full run for your currently active profile. This value changes when that profile saves a faster eligible PB.",
                L"Add the fastest recorded time for each button segment, even if those times came from separate runs. This theoretical sum is not necessarily a time you have completed in one run.",
                L"Show cumulative run time at the last button you pressed. This is a run-start-to-button timestamp, not the duration of only that button segment.",
                L"Display a total or single-segment record from the selected other profile. Use Compare Profile & Mode to choose which profile supplies those records.",
                L"Show the fastest saved button-segment time, or the sum up to the current button, from your active profile. The displayed format depends on the selected view.",
                L"Show the saved PB reference time for the current button. Choose either its individual segment duration or the cumulative timestamp from the start of that PB run."
            });
        case Page::HudGameplay:
        case Page::SplitHud: {
            const bool splits = page == Page::SplitHud;
            const size_t count = splits ? kSplitHudElements.size() : kGeneralHudElements.size();
            if (selected >= 0 && static_cast<size_t>(selected) < count) {
                const auto element = splits ? kSplitHudElements[selected] : kGeneralHudElements[selected];
                return std::wstring(L"Toggle the ") + kHudPositionLabels[static_cast<size_t>(element)] +
                    L" overlay ON or OFF without changing gameplay or saved records. " +
                    (splits ? L"Its appearance is under Splits / Split HUD Customisations."
                            : L"Its appearance is under HUD / Customisations.");
            }
            break;
        }
        case Page::HudKeyDisplay:
            return DescriptionAt(selected, {
                L"Enable or hide the W, A, S, D, Space and Shift key display. Keys light up as they are held, and dim when released. It only reads keyboard state while Refunct is focused, so it does not change movement, hotkeys or cheat status.",
                L"Open the existing input-display appearance editor. Set its position (corners or centre), size, colour, custom HEX/Windows picker and optional header. All appearance settings are saved automatically."
            });
        case Page::HudAppearance:
        case Page::SplitHudAppearance: {
            const bool splits = page == Page::SplitHudAppearance;
            const size_t count = splits ? kSplitHudElements.size() : kGeneralHudElements.size();
            if (selected >= 0 && static_cast<size_t>(selected) < count) {
                const auto element = splits ? kSplitHudElements[selected] : kGeneralHudElements[selected];
                return std::wstring(L"Open the customisations for ") +
                    kHudPositionLabels[static_cast<size_t>(element)] +
                    L": position (including centre), colour, custom HEX/Windows picker, scale and header visibility. "
                    L"Selected options are saved per element and do not change its underlying values.";
            }
            if (!splits && selected == static_cast<int>(count))
                return L"Move the F5 utility menu between the four screen corners. The centre option is available for HUD overlays, not the menu.";
            break;
        }
        case Page::HudAppearanceEdit: {
            const auto element = static_cast<HudElement>(s_hud_style_element);
            const int view_row = HudElementHasView(element) ? 5 : -1;
            const int reset_row = view_row == -1 ? 5 : 6;
            if (selected == 0) return L"Set this element's independent HUD position with Left/Right: Top Left, Top Right, Bottom Left, Bottom Right or Center. Multiple elements at one position stack together.";
            if (selected == 1) return L"Choose a colour preset with Left/Right. Default keeps original warning/status colours. Custom uses the saved HEX or Windows-picker value.";
            if (selected == 2) return L"Switch between manual six-digit HEX entry and the Windows colour picker with Left/Right; press Enter to edit. Cancelling keeps the previous colour. A chosen custom colour activates Custom mode.";
            if (selected == 3) return L"Change this element's size between 50% and 200% in 10% steps. Vertical spacing scales too so neighbouring elements don't overlap.";
            if (selected == 4) return L"Hide or show this element's text heading without changing its value. For list HUDs, turning the header OFF removes only the list title; all item rows remain visible. Warnings keep their meaningful status content.";
            if (selected == view_row) return L"Choose cumulative total vs individual button time for saved records, or whole-map vs currently loaded counters for cube/tile displays. Changing this does not rewrite progress or PB records.";
            if (selected == reset_row) return L"Reset this element's position, colour, size and header preference to the original defaults. Visibility and saved records remain unchanged.";
            break;
        }
        case Page::HudPositions:
            if (static_cast<size_t>(selected) < kHudElementCount)
                return std::wstring(L"Use Left/Right to place ") +
                    kHudPositionLabels[static_cast<size_t>(selected)] + L" at the chosen screen corner or Center. Each HUD item can be placed independently.";
            if (selected == static_cast<int>(kHudElementCount))
                return L"Move the F5 utility menu to any of the four screen corners. The centre placement is for HUD items only, not the menu.";
            break;
        case Page::SplitDisplay:
            return DescriptionAt(selected, {
                L"Include the number of the button that just triggered the split. This identifies the checkpoint shown by the popup.",
                L"Display your cumulative elapsed run time at the current button. It measures from run start, not just from the previous button.",
                L"Display the time between the previous button and the latest button. This is the duration of the current button segment, not the cumulative run time.",
                L"Compare your total time at this button against the same timestamp in your PB: negative = ahead, positive = behind PB. This is the overall lead or deficit, not time gained only on this section.",
                L"Compare this single button segment with the fastest recorded time for that same segment, using the PB as a fallback when needed. A negative value means this segment was faster.",
                L"Compare your cumulative run time at this button with the selected other profile. This uses a separate reference profile, not the PB of the active profile.",
                L"Compare the completed section duration with the corresponding section in the active profile PB. This shows how this section performed, separately from your overall cumulative lead.",
                L"Compare the completed section duration against the corresponding section from the other selected profile. The comparison reference is chosen in Compare Profile & Mode.",
                L"Show how your cumulative PB lead changed at this button relative to the previous button: green minus = time gained, red plus = time lost. Example: going from 0.400 ahead to 0.440 ahead displays -0.040."
            });
        case Page::HudComparisons:
            return DescriptionAt(selected, {
                L"Compare against the other profile completed PB, or a total constructed from its individual fastest segments. These are different references and can produce different deltas.",
                L"Select which saved split profile supplies the other-profile comparisons. This does not change the profile used for saving the current run.",
                L"Read the selected other profile records from disk again. Use this after changing that profile file externally or updating it in another session."
            });
        case Page::ProfilesHub:
            return DescriptionAt(selected, {
                L"Manage PBs, best segments, ghost recordings and record-saving permissions using independent replay/split profiles.",
                L"Open the existing world customisation profiles. Changes also appear in Customisation / Profiles.",
                L"Open the existing HUD profiles, including inputs and split overlays. Changes also appear in HUD / Profiles.",
                L"Manage World gameplay profiles; also available in World / World Profiles.",
                L"Manage keyboard shortcut profiles; also available in Settings / Hotkeys."
            });
        case Page::Profiles: {
            const size_t count = g_profiles.size();
            if (static_cast<size_t>(selected) < count)
                return L"Make this profile the active destination for PB and fastest-segment saving. Other profiles keep their own separate records.";
            if (selected == static_cast<int>(count))
                return L"Create a new named split profile with its own saved PB and fastest-segment data. You can switch profiles later without replacing the others.";
            if (selected == static_cast<int>(count) + 1)
                return L"Rename the active split profile while retaining its recorded PB and segments. This changes the profile name, not the stored split values.";
            if (selected == static_cast<int>(count) + 2)
                return L"Scan the split-profile folder again for available files. Use this when you have added or removed profiles outside the game.";
            if (selected == static_cast<int>(count) + 3)
                return L"Reload the active profile PB and segment records from disk. This updates the comparison reference without changing the current run time.";
            if (selected == static_cast<int>(count) + 4)
                return L"Set the existing global permissions for saving PBs, best segments and ghosts, including assisted runs. These options do not alter existing profile files.";
            break;
        }
        case Page::Settings:
            return DescriptionAt(selected, {
                L"Browse grouped keyboard shortcuts; Esc or Backspace sets a binding to None. This does not affect saved runs or gameplay state.",
                L"Choose independently whether Movement or World gameplay settings survive game launch and map restart. Cosmetic colours and PBs are unaffected.",
                L"Manage runtime log saving, startup tracing and the optimisation monitor."
            });
        case Page::ConfirmClearLogs:
            return DescriptionAt(selected, {L"Permanently delete runtime log files in Logs. Files in use may be retained; check the result on the Logs page.", L"Return without deleting anything."});
        case Page::Logs:
            return DescriptionAt(selected, {
                L"Enable or disable all runtime logs, including five-second performance reports and startup logs. OFF stops timing and discards pending messages. Existing logs are kept.",
                L"Save RefunctUtility.startup.txt independently. Applies to the next game launch and any later startup trace calls. Save Logs must also be ON; its OFF state preserves this preference.",
                L"Clear existing runtime logs after confirmation. Settings and profiles are kept. Logging resumes in a new file if Save Logs is ON."
            });
        case Page::Hotkeys:
            if (selected >= 0 && static_cast<size_t>(selected) < kHotkeyCategories.size())
                return L"Choose a category to change its hotkeys. All existing assignments are preserved.";
            if (selected == static_cast<int>(kHotkeyCategories.size()))
                return L"Manage saved keyboard shortcut profiles.";
            if (selected == static_cast<int>(kHotkeyCategories.size()) + 1)
                return L"Reset every assigned keyboard shortcut to its default, including unassigned options.";
            break;
        case Page::HotkeysGeneral: case Page::HotkeysMovement:
        case Page::HotkeysPractice: case Page::HotkeysHUD:
        case Page::HotkeysWorld: case Page::HotkeysGhost: {
            const size_t action = HotkeyActionAtRow(page, selected);
            if (action < kActionCount)
                return std::wstring(L"Press Enter, then a key for ") +
                    kActions[action].label + L". Esc or Backspace sets None.";
            break;
        }
    }
    // A last-resort hint for future menu rows: do not leave a blank help panel.
    return L"Press Enter to select; use Left/Right when adjustment arrows are shown.";
}


// HUD reset remains display-only: no records or profile files are changed.
void RestoreHudDisplayDefaults() {
    g_show_timer = true;
    g_show_splits = true;
    g_show_pb_run_hud = false;
    g_show_sum_of_best_hud = false;
    g_show_last_button_split_hud = false;
    g_show_compare_hud = false;
    g_compare_hud_view = RecordTimeView::Total;
    g_show_best_segment_time_hud = false;
    g_show_pb_split_time_hud = false;
    g_best_segment_time_view = RecordTimeView::Segment;
    g_pb_split_time_view = RecordTimeView::Total;
    g_show_compare_popup = false;
    g_pb_gain_loss_display = SectionCompareDisplayMode::Off;
    g_section_vs_pb_display = SectionCompareDisplayMode::Off;
    g_section_vs_file_display = SectionCompareDisplayMode::Off;
    g_compare_popup_show_time = true;
    g_compare_popup_show_header = true;
    g_compare_mode = ProfileCompareMode::PersonalBest;
    g_show_fps = false;
    g_show_game_time_hud = false;
    g_show_local_time_hud = false;
    g_show_speed = false;
    g_show_completion_percent = false;
    g_show_button_number = false;
    g_show_key_display = false;
    g_show_practice_status_hud = true;
    g_show_record_status_hud = true;
    g_show_enabled_modules = false;
    g_show_active_cheats = false;
    g_show_active_customisations = false;
    g_show_menu_descriptions = false;
    g_hud_corners = kDefaultHudCorners;
    g_hud_colour_modes.fill(0);
    g_hud_custom_colours.fill(0xFFFFFFu);
    g_hud_size_percent.fill(100);
    g_hud_show_headers = kDefaultHudShowHeaders;
    g_menu_position = MenuPosition::TopRight;
    g_segment_row_mode = SplitRowMode::HeaderAndTime;
    g_pb_row_mode = SplitRowMode::TimeOnly;
    g_show_split_button = true;
    g_show_split_total_time = true;
    g_show_split_fastest_delta = true;
    g_show_fastest_delta_time = true;
    g_show_fastest_delta_label = false;
    g_show_coordinates = true;
    g_cube_display_mode = ProgressDisplayMode::Off;
    g_tile_display_mode = ProgressDisplayMode::Off;
    SaveSettings();
    Notify(L"Display defaults restored");
}

void GoBack() {
    switch (g_page) {
        case Page::Main: g_menu_open.store(false); break;
        case Page::Search:
            SetPage(s_menu_search_return_page);
            g_selection = s_menu_search_return_selection;
            break;
        case Page::RestartOptions:
            SetPage(Page::SaveStates); break;
        case Page::ConfirmLevelReload:
            SetPage(Page::RestartOptions); break;
        case Page::ConfirmClearLogs:
            SetPage(Page::Logs); break;
        case Page::ConfirmDeleteHudProfile:
            SetPage(Page::HudProfiles); break;
        case Page::HudProfiles:
            SetPage(s_hud_profiles_return_page); break;
        case Page::ConfirmDeleteCustomisationProfile:
            SetPage(Page::CustomisationProfiles); break;
        case Page::CustomisationProfiles:
            SetPage(s_customisation_profiles_return_page); break;
        case Page::ConfirmDeleteWorldProfile:
            SetPage(Page::WorldProfiles); break;
        case Page::WorldProfiles:
            SetPage(s_world_profiles_return_page); break;
        case Page::ConfirmDeleteHotkeyProfile:
            SetPage(Page::HotkeyProfiles); break;
        case Page::HotkeyProfiles:
            SetPage(s_hotkey_profiles_return_page); break;
        case Page::Movement: case Page::World: case Page::Customisation:
        case Page::Maps: case Page::Hud: case Page::Splits: case Page::HudKeyDisplay:
        case Page::Settings: case Page::Ghost: case Page::ProfilesHub:
            SetPage(Page::Main); break;
        case Page::Profiles:
            SetPage(Page::ProfilesHub); break;
        case Page::GhostReplay: SetPage(Page::Main); break;
        case Page::SaveStates: SetPage(Page::World); break;
        case Page::TeleportCoordinates: SetPage(Page::SavePositions); break;
        case Page::Practice: case Page::SavePositions:
        case Page::SavedCheckpoints: case Page::ButtonCheckpoints:
            SetPage(Page::SaveStates); break;
        case Page::WorldSections:
        case Page::WorldButtonActions: case Page::WorldTileActions:
        case Page::WorldCubes: case Page::WorldObjectRemoval:
        case Page::WorldTubeSpeed: case Page::WorldPlatformSpeed:
        case Page::WorldJumpPadHeight:
        case Page::WorldButtons: case Page::WorldTiles: case Page::WorldTubes:
        case Page::WorldPlatforms: case Page::WorldJumpPads: case Page::WorldWater:
            SetPage(Page::World); break;
        case Page::WorldPlayer: case Page::WorldButtonColours: case Page::WorldButtonBeacons:
        case Page::WorldTileColours: case Page::WorldWalls:
        case Page::WorldTubeColours: case Page::WorldPlatformColours:
        case Page::WorldJumpPadColours: case Page::CustomisationTime:
        case Page::CustomisationSky: case Page::CustomisationPillars:
        case Page::CustomisationShadows: case Page::Crosshair:
            SetPage(Page::Customisation); break;
        case Page::CustomisationSkyColours: case Page::CustomisationSkyVisibility:
        case Page::CustomisationFogColours: case Page::CustomisationFogVisibility:
            SetPage(Page::CustomisationSky); break;
        case Page::WorldWaterColours:
            SetPage(s_water_colour_return_page); break;
        case Page::WaterReflections:
            SetPage(Page::Customisation); break;
        case Page::WaterObjectReflections: case Page::WaterBeamReflections:
            SetPage(Page::WaterReflections); break;
        case Page::WorldIndividualSections: case Page::WorldSectionHeights:
        case Page::WorldSectionSpeed:
            SetPage(Page::WorldSections); break;
        case Page::WorldIndividualSectionHeights:
            SetPage(Page::WorldSectionHeights); break;
        case Page::WorldSectionPositionEdit:
            SetPage(Page::WorldIndividualSectionHeights); break;
        case Page::MovementModes: case Page::MovementPhysics: case Page::MovementJump:
            SetPage(Page::Movement); break;
        case Page::GameplayRecords: SetPage(Page::Profiles); break;
        case Page::Persistence: case Page::Hotkeys: case Page::Logs:
            SetPage(Page::Settings); break;
        case Page::HotkeysGeneral: case Page::HotkeysMovement:
        case Page::HotkeysPractice: case Page::HotkeysHUD:
        case Page::HotkeysWorld: case Page::HotkeysGhost:
            SetPage(Page::Hotkeys); break;
        case Page::SplitDisplay: case Page::HudComparisons:
            SetPage(Page::HudTiming); break;
        case Page::HudTiming: case Page::HudRecordTimes:
        case Page::SplitHud: case Page::SplitHudAppearance:
            SetPage(Page::Splits); break;
        case Page::HudPositions: case Page::HudAppearance:
        case Page::HudGameplay:
            SetPage(Page::Hud); break;
        case Page::HudAppearanceEdit:
            SetPage(s_hud_style_return_page); break;
    }
}

void ToggleProgressRiseAnimation() {
    g_progress_rise_animation_enabled = !g_progress_rise_animation_enabled;
    SaveSettings();
    Notify(g_progress_rise_animation_enabled
        ? L"R / button restore: normal platform rise animation ON"
        : L"R / button restore: fast platform rise ON (fast R disables records)");
}

// Keep the two input paths for the flat reflection menu identical.
bool ToggleWaterReflectionRow(int row) {
    switch (row) {
        case 0: SetWaterReflectionsVisible(!AreWaterReflectionsVisible()); return true;
        case 1: SetWaterReflectedWallsVisible(!AreWaterReflectedWallsVisible()); return true;
        case 2: SetWaterReflectedButtonsVisible(!AreWaterReflectedButtonsVisible()); return true;
        case 3: SetWaterReflectedPipesVisible(!AreWaterReflectedPipesVisible()); return true;
        case 4: SetWaterReflectedJumpPadsVisible(!AreWaterReflectedJumpPadsVisible()); return true;
        case 5: SetWaterReflectedCubesVisible(!AreWaterReflectedCubesVisible()); return true;
        case 6: SetWaterReflectedBeaconsVisible(!AreWaterReflectedBeaconsVisible()); return true;
        default: return false;
    }
}

// Replay and splits use the same saved profile. Only explicit menu input
// loads recordings; rendering a menu row never scans or reads disk.
static void CycleReplayProfile(int direction) {
    if (g_profiles.empty()) {
        Notify(L"No replay/split profiles; refresh from Profiles first");
        return;
    }
    const auto current = std::find(g_profiles.begin(), g_profiles.end(), g_active_profile);
    const auto count = g_profiles.size();
    const size_t index = current == g_profiles.end()
        ? (direction < 0 ? count - 1 : 0)
        : (static_cast<size_t>(current - g_profiles.begin()) +
           (direction < 0 ? count - 1 : 1)) % count;
    const std::wstring chosen = g_profiles[index];
    if (chosen == g_active_profile) return;
    // SelectProfile drains outstanding writes before switching and invalidates
    // unfinished split history. Do not stop playback if switching fails.
    if (!SelectProfile(chosen, true)) {
        Notify(L"Could not switch replay profile");
        return;
    }
    StopGhostReplayViewer();
    LoadPersonalBest();
    Notify(L"Replay profile: " + chosen + L"; select Start to watch");
}

void ActivateSelected(MyCharacter* player) {
    const int i = g_selection;
    // The appended submenu Hide row retains both the page and selection.
    // It must run before the existing page-specific index handling.
    if (g_page != Page::Main && i >= 0 &&
        i == static_cast<int>(Entries().size()) - 1) {
        g_menu_open.store(false);
        return;
    }
    switch (g_page) {
        case Page::Search:
            if (i >= 1 && static_cast<size_t>(i) <= s_menu_search_matches.size() &&
                !s_menu_search_query.empty()) {
                const auto found = s_menu_search_matches[static_cast<size_t>(i - 1)];
                // Direct search entry uses the profile hub unless the search
                // began on that same profile page (preserve its original route).
                if (found.page == Page::HudProfiles && s_menu_search_return_page != Page::HudProfiles)
                    s_hud_profiles_return_page = Page::ProfilesHub;
                if (found.page == Page::CustomisationProfiles && s_menu_search_return_page != Page::CustomisationProfiles)
                    s_customisation_profiles_return_page = Page::ProfilesHub;
                if (found.page == Page::WorldProfiles && s_menu_search_return_page != Page::WorldProfiles)
                    s_world_profiles_return_page = Page::ProfilesHub;
                if (found.page == Page::HotkeyProfiles && s_menu_search_return_page != Page::HotkeyProfiles)
                    s_hotkey_profiles_return_page = Page::ProfilesHub;
                SetPage(found.page);
                g_selection = found.row;
            } else if (i == static_cast<int>(Entries().size()) - 3) {
                s_menu_search_query.clear();
                s_menu_search_matches.clear();
                g_selection = 0;
            } else if (i == static_cast<int>(Entries().size()) - 2) GoBack();
            break;
        case Page::Main:
            if (i == 0) SetPage(Page::Movement);
            else if (i == 1) SetPage(Page::World);
            else if (i == 2) {
                s_maps_page = 0;
                RefreshCustomMapFiles();
                SetPage(Page::Maps);
            }
            else if (i == 3) SetPage(Page::Customisation);
            else if (i == 4) SetPage(Page::Hud);
            else if (i == 5) SetPage(Page::Splits);
            else if (i == 6) SetPage(Page::HudKeyDisplay);
            else if (i == 7) SetPage(Page::Ghost);
            else if (i == 8) SetPage(Page::GhostReplay);
            else if (i == 9) SetPage(Page::ProfilesHub);
            else if (i == 10) SetPage(Page::Settings);
            else if (i == 11) OpenMenuSearch();
            else GoBack();
            break;
        case Page::Maps:
            if (i == 0) {
                RefreshCustomMapFiles();
                s_maps_page = 0;
                Notify(L"Maps folder refreshed");
            } else if (i == 1) ChangeMapPage(-1);
            else if (i == 2) ChangeMapPage(+1);
            else if (i >= 3 && static_cast<size_t>(i) < 3 + MapsOnPage())
                RequestCustomMapLoad(MapPageStart() + static_cast<size_t>(i - 3));
            else if (i == static_cast<int>(3 + MapsOnPage())) RequestCustomMapRestore();
            else GoBack();
            break;
        case Page::RestartOptions:
            if (i == 0) ExecuteAction(Action::Restart, player);
            else if (i == 1) ExecuteAction(Action::PreloadedRestart, player);
            else if (i == 2) ExecuteAction(Action::RestartWithProgress, player);
            else if (i == 3) ToggleProgressRiseAnimation();
            else if (i == 4) {
                // Match the L hotkey: request a full level reload immediately.
                // Close the menu only if the request was accepted.
                if (RequestFullLevelReload())
                    g_menu_open.store(false);
            }
            else GoBack();
            break;
        case Page::ConfirmLevelReload:
            if (i == 0) RequestFullLevelReload();
            else GoBack();
            break;
        case Page::Movement:
            if (i == 0) SetPage(Page::MovementModes);
            else if (i == 1) SetPage(Page::MovementPhysics);
            else if (i == 2) SetPage(Page::MovementJump);
            else if (i == 3) RestoreMovementDefaults(player);
            else GoBack();
            break;
        case Page::MovementModes:
            if (i == 0) ExecuteAction(Action::Fly, player);
            else if (i == 1) ExecuteAction(Action::Noclip, player);
            else if (i == 2) TogglePreserveMomentum();
            else GoBack();
            break;
        case Page::MovementPhysics:
            if (i == 0) ExecuteAction(Action::SpeedUp, player);
            else if (i == 1) AdjustMovementPercent(true, +1);
            else if (i == 2) AdjustSwimSpeedPercent(+1);
            else if (i == 3) AdjustClimbSpeedPercent(+1);
            else if (i == 4) AdjustSlidePercent(true, +1);
            else if (i == 5) AdjustSlidePercent(false, +1);
            else if (i == 6) AdjustMovementPercent(false, +1);
            else if (i == 7) AdjustAccelerationPercent(+1);
            else if (i == 8) AdjustGameSpeedPercent(+1);
            else GoBack();
            break;
        case Page::World:
            if (i == 0) SetPage(Page::WorldSections);
            else if (i == 1) SetPage(Page::SaveStates);
            else if (i == 2) SetPage(Page::WorldButtonActions);
            else if (i == 3) SetPage(Page::WorldTileActions);
            else if (i == 4) SetPage(Page::WorldCubes);
            else if (i == 5) SetPage(Page::WorldTubeSpeed);
            else if (i == 6) SetPage(Page::WorldPlatformSpeed);
            else if (i == 7) SetPage(Page::WorldJumpPadHeight);
            else if (i == 8) SetPage(Page::WorldWater);
            else if (i == 9) SetAllWorldObjectsRemoved(!AreAllWorldObjectsRemoved());
            else if (i == 10) ResetWorldObjectRemovals();
            else if (i == 11) {
                s_world_profiles_return_page = Page::World;
                RefreshWorldProfiles();
                SetPage(Page::WorldProfiles);
            } else if (i == 12) ResetWorldGameplayOptions();
            else GoBack();
            break;
        case Page::WorldObjectRemoval:
            GoBack();  // Retired: individual removal controls live in World categories.
            break;
        case Page::WorldCubes:
            if (i >= 0 && i <= 4)
                RequestCubeWorldAction(static_cast<CubeWorldAction>(i), player);
            else if (i == 5)
                SetWorldObjectRemoved(WorldObjectKind::Cubes,
                    !IsWorldObjectRemoved(WorldObjectKind::Cubes));
            else GoBack();
            break;
        case Page::Customisation:
            if (i == 0) SetPage(Page::WorldButtonColours);
            else if (i == 1) SetPage(Page::WorldButtonBeacons);
            else if (i == 2) SetPage(Page::WorldTileColours);
            else if (i == 3) SetPage(Page::WorldWalls);
            else if (i == 4) SetPage(Page::WorldTubeColours);
            else if (i == 5) SetPage(Page::WorldPlatformColours);
            else if (i == 6) SetPage(Page::WorldJumpPadColours);
            else if (i == 7) {
                s_water_colour_return_page = Page::Customisation;
                SetPage(Page::WorldWaterColours);
            }
            else if (i == 8) SetPage(Page::CustomisationTime);
            else if (i == 9) SetPage(Page::CustomisationSky);
            else if (i == 10) SetPage(Page::CustomisationShadows);
            else if (i == 11) SetPage(Page::CustomisationPillars);
            else if (i == 12) SetPage(Page::WorldPlayer);
            else if (i == 13) SetPage(Page::WaterReflections);
            else if (i == 14) SetPage(Page::Crosshair);
            else if (i == 15) {
                s_customisation_profiles_return_page = Page::Customisation;
                RefreshCustomisationProfiles();
                SetPage(Page::CustomisationProfiles);
            }
            else if (i == 16) ResetWorldCustomisation();
            else GoBack();
            break;
        case Page::Crosshair:
            if (i == 1) OpenCrosshairColour();
            else if (i >= 0 && i <= 5) AdjustCrosshairRow(i, +1);
            else if (i == 6) { g_crosshair = {}; SaveSettings(); }
            else GoBack();
            break;
        case Page::CustomisationProfiles: {
            const auto& profiles = CustomisationProfileNames();
            const size_t count = profiles.size();
            if (i >= 0 && static_cast<size_t>(i) < count) {
                SelectCustomisationProfile(profiles[static_cast<size_t>(i)]);
            } else if (i == static_cast<int>(count)) {
                StartNameEdit(NameEditMode::CustomCreate);
            } else if (i == static_cast<int>(count) + 1) {
                StartNameEdit(NameEditMode::CustomRename);
            } else if (i == static_cast<int>(count) + 2) {
                if (SaveActiveCustomisationProfile()) Notify(L"Customisation profile saved");
            } else if (i == static_cast<int>(count) + 3) {
                ReloadActiveCustomisationProfile();
            } else if (i == static_cast<int>(count) + 4) {
                RefreshCustomisationProfiles();
                Notify(L"Customisation profiles refreshed");
            } else if (i == static_cast<int>(count) + 5) {
                if (ActiveCustomisationProfile() == L"Default")
                    Notify(L"Default customisation profile is protected");
                else SetPage(Page::ConfirmDeleteCustomisationProfile);
            } else GoBack();
            break;
        }
        case Page::ConfirmDeleteCustomisationProfile:
            if (i == 0) {
                if (DeleteActiveCustomisationProfile()) SetPage(Page::CustomisationProfiles);
                else GoBack();
            } else GoBack();
            break;
        case Page::WorldProfiles: {
            const auto& profiles = WorldProfileNames();
            const size_t count = profiles.size();
            if (i >= 0 && static_cast<size_t>(i) < count) {
                SelectWorldProfile(profiles[static_cast<size_t>(i)]);
            } else if (i == static_cast<int>(count)) {
                StartNameEdit(NameEditMode::WorldCreate);
            } else if (i == static_cast<int>(count) + 1) {
                StartNameEdit(NameEditMode::WorldRename);
            } else if (i == static_cast<int>(count) + 2) {
                if (SaveActiveWorldProfile()) Notify(L"World profile saved");
            } else if (i == static_cast<int>(count) + 3) {
                ReloadActiveWorldProfile();
            } else if (i == static_cast<int>(count) + 4) {
                RefreshWorldProfiles();
                Notify(L"World profiles refreshed");
            } else if (i == static_cast<int>(count) + 5) {
                if (ActiveWorldProfile() == L"Default")
                    Notify(L"Default world profile is protected");
                else SetPage(Page::ConfirmDeleteWorldProfile);
            } else GoBack();
            break;
        }
        case Page::ConfirmDeleteWorldProfile:
            if (i == 0) {
                if (DeleteActiveWorldProfile()) SetPage(Page::WorldProfiles);
                else GoBack();
            } else GoBack();
            break;
        case Page::HotkeyProfiles: {
            const auto& profiles = HotkeyProfileNames();
            const size_t count = profiles.size();
            if (i >= 0 && static_cast<size_t>(i) < count) {
                SelectHotkeyProfile(profiles[static_cast<size_t>(i)]);
            } else if (i == static_cast<int>(count)) {
                StartNameEdit(NameEditMode::HotkeyCreate);
            } else if (i == static_cast<int>(count) + 1) {
                StartNameEdit(NameEditMode::HotkeyRename);
            } else if (i == static_cast<int>(count) + 2) {
                if (SaveActiveHotkeyProfile()) Notify(L"Hotkey profile saved");
            } else if (i == static_cast<int>(count) + 3) {
                ReloadActiveHotkeyProfile();
            } else if (i == static_cast<int>(count) + 4) {
                RefreshHotkeyProfiles();
                Notify(L"Hotkey profiles refreshed");
            } else if (i == static_cast<int>(count) + 5) {
                if (ActiveHotkeyProfile() == L"Default")
                    Notify(L"Default hotkey profile is protected");
                else SetPage(Page::ConfirmDeleteHotkeyProfile);
            } else GoBack();
            break;
        }
        case Page::ConfirmDeleteHotkeyProfile:
            if (i == 0) {
                if (DeleteActiveHotkeyProfile()) SetPage(Page::HotkeyProfiles);
                else GoBack();
            } else GoBack();
            break;
        case Page::HudProfiles: {
            const auto& profiles = HudProfileNames();
            const size_t count = profiles.size();
            if (i >= 0 && static_cast<size_t>(i) < count) {
                SelectHudProfile(profiles[static_cast<size_t>(i)]);
            } else if (i == static_cast<int>(count)) {
                StartNameEdit(NameEditMode::HudCreate);
            } else if (i == static_cast<int>(count) + 1) {
                StartNameEdit(NameEditMode::HudRename);
            } else if (i == static_cast<int>(count) + 2) {
                if (SaveActiveHudProfile()) Notify(L"HUD profile saved");
            } else if (i == static_cast<int>(count) + 3) {
                ReloadActiveHudProfile();
            } else if (i == static_cast<int>(count) + 4) {
                RefreshHudProfiles();
                Notify(L"HUD profiles refreshed");
            } else if (i == static_cast<int>(count) + 5) {
                if (ActiveHudProfile() == L"Default")
                    Notify(L"Default HUD profile is protected");
                else SetPage(Page::ConfirmDeleteHudProfile);
            } else GoBack();
            break;
        }
        case Page::ConfirmDeleteHudProfile:
            if (i == 0) {
                if (DeleteActiveHudProfile()) SetPage(Page::HudProfiles);
                else GoBack();
            } else GoBack();
            break;
        case Page::CustomisationShadows:
            if (i == 0) SetShadowsVisible(!AreShadowsVisible());
            else if (i == 1) SetShadowAmbientColourEnabled(!IsShadowAmbientColourEnabled());
            else if (i == 2) OpenShadowAmbientMethod();
            else if (i == 3) ResetShadowCustomisation();
            else GoBack();
            break;
        case Page::CustomisationPillars:
            if (i == 0) SetPillarColourEnabled(!IsPillarColourEnabled());
            else if (i == 1) OpenPillarColourMethod();
            else if (i == 2) SetPillarsRemoved(!ArePillarsRemoved());
            else GoBack();
            break;
        case Page::CustomisationTime:
            if (i == 0) SetCustomTimeOfDayEnabled(!IsCustomTimeOfDayEnabled());
            else if (i == 1) SetCustomTimeMinutes(720);
            else if (i == 2) SetCustomTimeLocked(!IsCustomTimeLocked());
            else if (i == 3) SetCustomTimeSpeedPercent(100);
            else if (i == 4) SetCustomTimeOfDayEnabled(false);
            else GoBack();
            break;
        case Page::CustomisationSky:
            if (i == 0) SetPage(Page::CustomisationSkyColours);
            else if (i == 1) SetPage(Page::CustomisationSkyVisibility);
            else if (i == 2) SetPage(Page::CustomisationFogColours);
            else if (i == 3) SetPage(Page::CustomisationFogVisibility);
            else if (i == 4) {
                ResetSkyDiscVisibility(); ResetSkyEffectVisibility();
                ResetSkyColours(); ResetFogColours(); ResetFogVisibility();
            }
            else GoBack();
            break;
        case Page::CustomisationSkyColours:
            if (i == 0) SetSkyColourEnabled(SkyRegion::Zenith,
                !IsSkyColourEnabled(SkyRegion::Zenith));
            else if (i == 1) OpenSkyColourMethod(SkyRegion::Zenith);
            else if (i == 2) SetSkyColourEnabled(SkyRegion::Horizon,
                !IsSkyColourEnabled(SkyRegion::Horizon));
            else if (i == 3) OpenSkyColourMethod(SkyRegion::Horizon);
            else if (i == 4) SetSkyColourEnabled(SkyRegion::Sun,
                !IsSkyColourEnabled(SkyRegion::Sun));
            else if (i == 5) OpenSkyColourMethod(SkyRegion::Sun);
            else if (i == 6) SetSkyColourEnabled(SkyRegion::Clouds,
                !IsSkyColourEnabled(SkyRegion::Clouds));
            else if (i == 7) OpenSkyColourMethod(SkyRegion::Clouds);
            else GoBack();
            break;
        case Page::CustomisationSkyVisibility:
            if (i == 0) SetSkyDiscVisible(!IsSkyDiscVisible());
            else if (i == 1) SetSkyEffectVisible(SkyEffect::Clouds,
                !IsSkyEffectVisible(SkyEffect::Clouds));
            else if (i == 2) SetSkyEffectVisible(SkyEffect::Stars,
                !IsSkyEffectVisible(SkyEffect::Stars));
            else GoBack();
            break;
        case Page::CustomisationFogColours:
            if (i == 0) SetFogColourEnabled(FogRegion::General,
                !IsFogColourEnabled(FogRegion::General));
            else if (i == 1) OpenFogColourMethod(FogRegion::General);
            else if (i == 2) SetFogColourEnabled(FogRegion::Directional,
                !IsFogColourEnabled(FogRegion::Directional));
            else if (i == 3) OpenFogColourMethod(FogRegion::Directional);
            else GoBack();
            break;
        case Page::CustomisationFogVisibility:
            if (i == 0) SetFogVisible(FogRegion::General,
                !IsFogVisible(FogRegion::General));
            else if (i == 1) SetFogVisible(FogRegion::Directional,
                !IsFogVisible(FogRegion::Directional));
            else GoBack();
            break;
        case Page::WorldWater:
            if (i == 0) SetAllWaterRemoved(!IsAllWaterRemoved());
            else if (i == 1) SetWaterRemoved(!IsWaterRemoved());
            else if (i == 2) SetWaterSwimmingDisabled(!IsWaterSwimmingDisabled());
            else if (i == 3) AdjustWaterHeightOffset(+1);
            else if (i == 4) SetWaterHeightOffset(0);
            else if (i == 5) {
                s_water_colour_return_page = Page::WorldWater;
                SetPage(Page::WorldWaterColours);
            }
            else GoBack();
            break;
        case Page::WorldWaterColours:
            if (i == 0) SetWaterCustomColourEnabled(!IsWaterCustomColourEnabled());
            else if (i == 1) OpenWaterColourMethod();
            else if (i == 2) SetWaterColourIntensityPercent(100);
            else if (i == 3)
                SetUnderwaterCustomColourEnabled(!IsUnderwaterCustomColourEnabled());
            else if (i == 4) OpenUnderwaterColourMethod();
            else if (i == 5) SetUnderwaterColourIntensityPercent(100);
            else if (i == 6) SetUnderwaterTintRemoved(!IsUnderwaterTintRemoved());
            else if (i == 7) SetWaterSplashesRemoved(!AreWaterSplashesRemoved());
            else GoBack();
            break;
        case Page::WaterReflections:
            if (!ToggleWaterReflectionRow(i)) GoBack();
            break;
        case Page::WaterObjectReflections: case Page::WaterBeamReflections:
            GoBack();
            break;
        case Page::WorldJumpPads:
            if (i == 0) SetPage(Page::WorldJumpPadColours);
            else if (i == 1) SetPage(Page::WorldJumpPadHeight);
            else GoBack();
            break;
        case Page::WorldJumpPadColours:
            if (i == 0) SetJumpPadCustomColourEnabled(!IsJumpPadCustomColourEnabled());
            else if (i == 1) OpenJumpPadColourMethod();
            else GoBack();
            break;
        case Page::WorldJumpPadHeight:
            if (i == 0) AdjustJumpPadHeight(+1);
            else if (i == 1) ResetJumpPadHeight();
            else if (i == 2)
                SetWorldObjectRemoved(WorldObjectKind::JumpPads,
                    !IsWorldObjectRemoved(WorldObjectKind::JumpPads));
            else GoBack();
            break;
        case Page::WorldPlatforms:
            if (i == 0) SetPage(Page::WorldPlatformColours);
            else if (i == 1) SetPage(Page::WorldPlatformSpeed);
            else GoBack();
            break;
        case Page::WorldPlatformColours:
            if (i == 0) SetPlatformCustomColourEnabled(!IsPlatformCustomColourEnabled());
            else if (i == 1) OpenPlatformColourMethod();
            else GoBack();
            break;
        case Page::WorldPlatformSpeed:
            if (i == 0) AdjustPlatformSpeed(+1);
            else if (i == 1) ResetPlatformSpeed();
            else if (i == 2)
                SetWorldObjectRemoved(WorldObjectKind::Platforms,
                    !IsWorldObjectRemoved(WorldObjectKind::Platforms));
            else GoBack();
            break;
        case Page::WorldTubes:
            if (i == 0) SetPage(Page::WorldTubeColours);
            else if (i == 1) SetPage(Page::WorldTubeSpeed);
            else GoBack();
            break;
        case Page::WorldTubeSpeed:
            if (i == 0) AdjustTubeTravelSpeed(+1);
            else if (i == 1) ResetTubeTravelSpeed();
            else if (i == 2)
                SetWorldObjectRemoved(WorldObjectKind::Tubes,
                    !IsWorldObjectRemoved(WorldObjectKind::Tubes));
            else GoBack();
            break;
        case Page::WorldTubeColours:
            if (i == 0) SetTubeCustomColourEnabled(!IsTubeCustomColourEnabled());
            else if (i == 1) OpenTubeColourMethod();
            else GoBack();
            break;
        case Page::WorldSections:
            if (i == 0) {
                g_animate_section_changes = !g_animate_section_changes;
                if (!g_animate_section_changes) {
                    FinishSectionPlacementAnimations();
                    g_recording_tainted = true;
                    if (!g_pb_cheats_allowed_for_run) g_run_pb_eligible = false;
                }
                SaveSettings();
                Notify(g_animate_section_changes
                    ? L"Section animation ON: normal automatic rises"
                    : L"Section animation OFF: automatic rises instant (assisted)");
            } else if (i == 1) SetAllSectionPlacements(SectionPlacement::Raised);
            else if (i == 2) SetAllSectionPlacements(SectionPlacement::Lowered);
            else if (i == 3) ResetAllSectionPlacements();
            else if (i == 4) {
                if (SetButtonsSyncWithSections(!g_buttons_sync_with_sections))
                    SaveSettings();
            }
            else if (i == 5) SetPage(Page::WorldIndividualSections);
            else if (i == 6) SetPage(Page::WorldSectionHeights);
            else if (i == 7) SetPage(Page::WorldSectionSpeed);
            else GoBack();
            break;
        case Page::WorldSectionSpeed:
            if (i == 0) AdjustSectionSpeed(+1);
            else if (i == 1) ResetSectionSpeed();
            else GoBack();
            break;
        case Page::WorldSectionHeights:
            if (i >= 0 && i < 3) AdjustAllSectionPositionOffsets(i, +1);
            else if (i == 3) SetPage(Page::WorldIndividualSectionHeights);
            else if (i == 4) ResetAllSectionPositionOffsets();
            else GoBack();
            break;
        case Page::WorldIndividualSectionHeights:
            if (i >= 0 && i < kPracticeClusterCount) {
                s_position_edit_section = i + 1;
                SetPage(Page::WorldSectionPositionEdit);
            } else GoBack();
            break;
        case Page::WorldSectionPositionEdit:
            if (i >= 0 && i < 3)
                AdjustSectionPositionOffset(s_position_edit_section, i, +1);
            else if (i == 3) ResetSectionPositionOffsets(s_position_edit_section);
            else GoBack();
            break;
        case Page::WorldButtons:
            if (i == 0) SetPage(Page::WorldButtonActions);
            else if (i == 1) SetPage(Page::WorldButtonColours);
            else if (i == 2) SetPage(Page::WorldButtonBeacons);
            else GoBack();
            break;
        case Page::WorldButtonActions:
            if (i == 0) RestoreDefaultButtons();
            else if (i == 1) RestoreButtons(false);
            else if (i == 2) RestoreButtons(true);
            else if (i == 3) DisableUnderwaterButtons();
            else if (i == 4) DisableAboveWaterButtons();
            else if (i == 5) DisableAllButtons();
            else if (i == 6) PressAllAvailableButtons();
            else if (i == 7) RepairGhostButtonsNow();
            else if (i == 8) ToggleButtonTileSelection();
            else if (i == 9)
                SetWorldObjectRemoved(WorldObjectKind::Buttons,
                    !IsWorldObjectRemoved(WorldObjectKind::Buttons));
            else GoBack();
            break;
        case Page::WorldButtonColours:
            if (i == 0 || i == 2) {
                const bool pressed = i == 2;
                SetButtonCustomColourEnabled(pressed,
                    !IsButtonCustomColourEnabled(pressed));
            } else if (i == 4 || i == 6) {
                const bool pressed = i == 6;
                SetButtonBorderCustomColourEnabled(pressed,
                    !IsButtonBorderCustomColourEnabled(pressed));
            } else if (i == 1 || i == 3 || i == 5 || i == 7) {
                OpenButtonColourMethod(i / 2);
            } else GoBack();
            break;
        case Page::WorldButtonBeacons:
            if (i == 0) SetButtonBeaconVisible(!IsButtonBeaconVisible());
            else if (i == 1) SetButtonBeaconCustomColourEnabled(
                !IsButtonBeaconCustomColourEnabled());
            else if (i == 2) OpenButtonColourMethod(4);
            else GoBack();
            break;
        case Page::WorldTiles:
            if (i == 0) SetPage(Page::WorldTileActions);
            else if (i == 1) SetPage(Page::WorldTileColours);
            else if (i == 2) SetPage(Page::WorldWalls);
            else GoBack();
            break;
        case Page::WorldTileActions:
            if (i == 0) SelectWorldTiles(false);
            else if (i == 1) SelectWorldTiles(true);
            else if (i == 2) DeselectWorldTiles(false);
            else if (i == 3) DeselectWorldTiles(true);
            else if (i == 4) ToggleTileSelectionOnContact();
            else if (i == 5)
                SetWorldObjectRemoved(WorldObjectKind::Tiles,
                    !IsWorldObjectRemoved(WorldObjectKind::Tiles));
            else GoBack();
            break;
        case Page::WorldTileColours:
            if (i == 0) CycleTileColourPreset(true, +1);
            else if (i == 1) OpenSelectedTileColourMethod(true);
            else if (i == 2) ToggleRandomTileRGB();
            else if (i == 3) CycleTileColourPreset(false, +1);
            else if (i == 4) OpenSelectedTileColourMethod(false);
            else GoBack();
            break;
        case Page::WorldWalls:
            if (i == 0 || i == 2) {
                const bool selected = i == 0;
                SetWallCustomColourEnabled(selected,
                    !IsWallCustomColourEnabled(selected));
            }
            else if (i == 1 || i == 3) OpenWallColourMethod(i == 1);
            else GoBack();
            break;
        case Page::WorldIndividualSections:
            if (i >= 0 && i < kPracticeClusterCount)
                CycleSectionPlacement(i + 1, +1);
            else GoBack();
            break;
        case Page::WorldPlayer:
            if (i == 0) {
                AdjustPlayerFovDegrees(+1);
            } else if (i == 1) {
                g_player_fov_degrees = 0;
                SaveSettings();
            } else GoBack();
            break;
        case Page::MovementJump:
            if (i == 0) CycleJumpAssistMode(true, +1);
            else if (i == 1) CycleJumpAssistMode(false, +1);
            else if (i == 2) ToggleAirJump();
            else GoBack();
            break;
        case Page::Persistence:
            if (i == 0) ToggleMovementPersistence(true);
            else if (i == 1) ToggleMovementPersistence(false);
            else if (i == 2 || i == 3) ToggleWorldPersistence(i == 2);
            else GoBack();
            break;
        case Page::GameplayRecords:
            if (i == 0) ToggleRecordSaving(true);
            else if (i == 1) ToggleRecordSaving(false);
            else if (i == 2) {
                g_save_pb_ghosts = !g_save_pb_ghosts;
                SaveSettings();
            } else if (i == 3) {
                g_save_fastest_ghosts = !g_save_fastest_ghosts;
                SaveSettings();
            } else if (i == 4) CycleCheatRecordMode(+1);
            else if (i == 5) {
                g_auto_create_pb_profile = !g_auto_create_pb_profile;
                SaveSettings();
            } else if (i == 6) {
                g_auto_pb_copy_best_segments = !g_auto_pb_copy_best_segments;
                SaveSettings();
            } else GoBack();
            break;
        case Page::Ghost:
            if (i == 0) SetPBGhostEnabled(!IsPBGhostEnabled());
            else if (i == 1) SetPBGhostPassThroughEnabled(!IsPBGhostPassThroughEnabled());
            else if (i == 2) SetPBGhost2DTransparencyEnabled(!IsPBGhost2DTransparencyEnabled());
            else if (i == 3) SetPBGhost3DTransparencyEnabled(!IsPBGhost3DTransparencyEnabled());
            else if (i == 4) OpenGhostColourMethod();
            else if (i == 5) SetPBGhostButtonSyncEnabled(!IsPBGhostButtonSyncEnabled());
            else if (i == 6) SetPBGhostFastestSectionsEnabled(!IsPBGhostFastestSectionsEnabled());
            else GoBack();
            break;
        case Page::GhostReplay:
            if (i == 0) {
                if (!StartGhostReplayViewer()) Notify(L"No saved ghost recording for this split profile");
                else Notify(IsGhostReplayRequested() ? L"Preparing native button replay" : L"Replay stopped; level will reset if buttons were pressed");
            } else if (i == 1) CycleReplayProfile(+1);
            else if (i == 2) ToggleGhostReplaySource();
            else if (i == 3) ToggleGhostReplayPause();
            else if (i == 4) RestartGhostReplay();
            else if (i == 5) SeekGhostReplaySeconds(-5);
            else if (i == 6) SeekGhostReplaySeconds(+5);
            else if (i == 7) CycleGhostReplaySpeed(+1);
            else if (i == 9) ToggleGhostReplayCameraMode();
            else if (i == 10) GoBack();
            // Playback time is informational; Source is an actual toggle.
            break;
        case Page::SaveStates:
            if (i == 0) SetPage(Page::RestartOptions);
            else if (i == 1) SetPage(Page::SavePositions);
            else if (i == 2) SetPage(Page::SavedCheckpoints);
            else if (i == 3) SetPage(Page::ButtonCheckpoints);
            else if (i == 4) SetPage(Page::Practice);
            else GoBack();
            break;
        case Page::SavePositions:
            if (i == 0) ExecuteAction(Action::SavePosition, player);
            else if (i == 1) ExecuteAction(Action::RestorePosition, player);
            else if (i == 2) {
                if (!g_teleport_target_initialized && !SeedTeleportTargetFromPlayer(player)) {
                    Notify(L"Player not ready: enter coordinates manually");
                }
                SetPage(Page::TeleportCoordinates);
            }
            else if (i == 3) ExecuteAction(Action::TeleportLastButtonGlobal, player);
            else if (i == 5) GoBack();
            break;
        case Page::TeleportCoordinates:
            if (i >= 0 && i <= 2) StartCoordinateEdit(i);
            else if (i == 3) {
                if (g_teleport_target_initialized)
                    TeleportToCoordinates(player, g_teleport_target);
                else Notify(L"Enter X, Y and Z coordinates first");
            } else if (i == 4) {
                if (!SeedTeleportTargetFromPlayer(player))
                    Notify(L"Cannot read current player position");
            } else GoBack();
            break;
        case Page::SavedCheckpoints:
            if (i == 0) ExecuteAction(Action::SaveState, player);
            else if (i == 1) ExecuteAction(Action::LoadState, player);
            else if (i == 3) GoBack();
            break;
        case Page::ButtonCheckpoints:
            if (i == 0) ExecuteAction(Action::LoadLastButton, player);
            else if (i == 1) ExecuteAction(Action::LoadSecondLastButton, player);
            else if (i == 2) {
                g_keep_last_button_momentum = !g_keep_last_button_momentum;
                SaveSettings();
            } else if (i == 5) GoBack();
            break;
        case Page::Practice:
            if (i == 0) {
                g_practice_selected_button = g_practice_selected_button % kRefunctButtonCount + 1;
                SaveSettings();
            } else if (i == 1) {
                RequestPractice(g_practice_selected_button);
            } else if (i == 3) GoBack();
            break;
        case Page::Hud:
            if (i == 0) SetPage(Page::HudGameplay);
            else if (i == 1) SetPage(Page::HudAppearance);
            else if (i == 2) {
                g_show_menu_descriptions = !g_show_menu_descriptions;
                SaveSettings();
            } else if (i == 3) RestoreHudDisplayDefaults();
            else if (i == 4) {
                s_hud_profiles_return_page = Page::Hud;
                RefreshHudProfiles();
                SetPage(Page::HudProfiles);
            }
            else GoBack();
            break;
        case Page::Splits:
            if (i == 0) SetPage(Page::SplitHud);
            else if (i == 1) SetPage(Page::HudTiming);
            else if (i == 2) SetPage(Page::HudRecordTimes);
            else if (i == 3) SetPage(Page::SplitHudAppearance);
            else GoBack();
            break;
        case Page::HudTiming:
            if (i == 0) g_show_splits = !g_show_splits;
            else if (i == 1) { SetPage(Page::SplitDisplay); break; }
            else if (i == 2) { SetPage(Page::HudComparisons); break; }
            else { GoBack(); break; }
            SaveSettings();
            break;
        case Page::HudRecordTimes:
            if (i == 0) { g_show_pb_run_hud = !g_show_pb_run_hud; SaveSettings(); }
            else if (i == 1) { g_show_sum_of_best_hud = !g_show_sum_of_best_hud; SaveSettings(); }
            else if (i == 2) { g_show_last_button_split_hud = !g_show_last_button_split_hud; SaveSettings(); }
            else if (i == 3)
                CycleRecordTimeHudMode(g_show_compare_hud, g_compare_hud_view, +1);
            else if (i == 4)
                CycleRecordTimeHudMode(g_show_best_segment_time_hud, g_best_segment_time_view, +1);
            else if (i == 5)
                CycleRecordTimeHudMode(g_show_pb_split_time_hud, g_pb_split_time_view, +1);
            else GoBack();
            break;
        case Page::HudGameplay:
        case Page::SplitHud: {
            const bool splits = g_page == Page::SplitHud;
            const size_t count = splits ? kSplitHudElements.size() : kGeneralHudElements.size();
            if (i >= 0 && static_cast<size_t>(i) < count)
                ToggleHudElement(splits ? kSplitHudElements[i] : kGeneralHudElements[i]);
            else GoBack();
            break;
        }
        case Page::HudKeyDisplay:
            if (i == 0) ToggleHudElement(HudElement::KeyDisplay);
            else if (i == 1) {
                s_hud_style_element = static_cast<size_t>(HudElement::KeyDisplay);
                s_hud_style_return_page = Page::HudKeyDisplay;
                SetPage(Page::HudAppearanceEdit);
            } else GoBack();
            break;
        case Page::HudAppearance:
        case Page::SplitHudAppearance: {
            const bool splits = g_page == Page::SplitHudAppearance;
            const size_t count = splits ? kSplitHudElements.size() : kGeneralHudElements.size();
            if (i >= 0 && static_cast<size_t>(i) < count) {
                const auto element = splits ? kSplitHudElements[i] : kGeneralHudElements[i];
                s_hud_style_element = static_cast<size_t>(element);
                s_hud_style_return_page = g_page;
                SetPage(Page::HudAppearanceEdit);
            } else if (!splits && i == static_cast<int>(count))
                CycleMenuPosition(+1);
            else GoBack();
            break;
        }
        case Page::HudAppearanceEdit: {
            const auto element = static_cast<HudElement>(s_hud_style_element);
            const int view_row = HudElementHasView(element) ? 5 : -1;
            const int reset_row = view_row == -1 ? 5 : 6;
            if (i == 0) CycleHudCorner(s_hud_style_element, +1);
            else if (i == 1) CycleHudColour(+1);
            else if (i == 2) OpenHudCustomColourMethod();
            else if (i == 3) AdjustHudTextSize(+1);
            else if (i == 4) ToggleHudHeader();
            else if (i == view_row) ToggleHudElementView(element);
            else if (i == reset_row) ResetHudElementStyle();
            else GoBack();
            break;
        }
        case Page::HudPositions:
            if (i >= 0 && static_cast<size_t>(i) < kHudElementCount)
                CycleHudCorner(static_cast<size_t>(i), +1);
            else if (i == static_cast<int>(kHudElementCount))
                CycleMenuPosition(+1);
            else GoBack();
            break;
        case Page::SplitDisplay:
            if (i == 0) g_show_split_button = !g_show_split_button;
            else if (i == 1) g_show_split_total_time = !g_show_split_total_time;
            else if (i == 2) { ChangeSplitRowMode(g_segment_row_mode, +1); break; }
            else if (i == 3) { ChangeSplitRowMode(g_pb_row_mode, +1); break; }
            else if (i == 4) { CycleFastestDeltaMode(+1); break; }
            else if (i == 5) { CycleComparePopupDisplayMode(+1); break; }
            else if (i == 6) { ChangeSectionCompareMode(g_section_vs_pb_display, +1); break; }
            else if (i == 7) { ChangeSectionCompareMode(g_section_vs_file_display, +1); break; }
            else if (i == 8) { ChangeSectionCompareMode(g_pb_gain_loss_display, +1); break; }
            else { GoBack(); break; }
            SaveSettings();
            break;
        case Page::HudComparisons:
            if (i == 0) { CycleCompareMode(); break; }
            else if (i == 1) {
                if (!CycleComparisonProfile(+1)) Notify(L"No other split profile available");
                break;
            } else if (i == 2) {
                Notify(ReloadComparisonProfile() ? L"Comparison profile reloaded" :
                       L"No comparison profile available");
                break;
            } else GoBack();
            break;
        case Page::ProfilesHub:
            if (i == 0) {
                RefreshProfiles();
                SetPage(Page::Profiles);
            } else if (i == 1) {
                s_customisation_profiles_return_page = Page::ProfilesHub;
                RefreshCustomisationProfiles();
                SetPage(Page::CustomisationProfiles);
            } else if (i == 2) {
                s_hud_profiles_return_page = Page::ProfilesHub;
                RefreshHudProfiles();
                SetPage(Page::HudProfiles);
            } else if (i == 3) {
                s_world_profiles_return_page = Page::ProfilesHub;
                RefreshWorldProfiles();
                SetPage(Page::WorldProfiles);
            } else if (i == 4) {
                s_hotkey_profiles_return_page = Page::ProfilesHub;
                RefreshHotkeyProfiles();
                SetPage(Page::HotkeyProfiles);
            } else GoBack();
            break;
        case Page::Profiles:
            if (i >= 0 && static_cast<size_t>(i) < g_profiles.size()) {
                const std::wstring chosen = g_profiles[static_cast<size_t>(i)];
                if (SelectProfile(chosen, true)) {
                    LoadPersonalBest();
                    Notify(L"Selected " + chosen + L"; restart before recording PB");
                }
            } else if (i == static_cast<int>(g_profiles.size())) {
                StartNameEdit(NameEditMode::Create);
            } else if (i == static_cast<int>(g_profiles.size()) + 1) {
                StartNameEdit(NameEditMode::Rename);
            } else if (i == static_cast<int>(g_profiles.size()) + 2) {
                RefreshProfiles();
                Notify(L"Profile list refreshed");
            } else if (i == static_cast<int>(g_profiles.size()) + 3) {
                LoadPersonalBest();
                Notify(L"Active split file reloaded");
            } else if (i == static_cast<int>(g_profiles.size()) + 4) {
                SetPage(Page::GameplayRecords);
            } else GoBack();
            break;
        case Page::Settings:
            if (i == 0) SetPage(Page::Hotkeys);
            else if (i == 1) SetPage(Page::Persistence);
            else if (i == 2) SetPage(Page::Logs);
            else GoBack();
            break;
        case Page::Logs:
            if (i == 0) {
                SetUtilLogEnabled(!g_util_log_enabled.load(std::memory_order_relaxed));
                SetPerformanceMonitorEnabled(g_util_log_enabled.load(std::memory_order_relaxed));
                SaveSettings();
                Notify(g_util_log_enabled.load(std::memory_order_relaxed)
                    ? L"Runtime log saving enabled" : L"Runtime log saving disabled");
            } else if (i == 1) {
                const bool enabled = !g_startup_log_enabled.load(std::memory_order_relaxed);
                g_startup_log_enabled.store(enabled, std::memory_order_release);
                SaveSettings();
                Notify(!enabled ? L"Startup Logs OFF" :
                    (g_util_log_enabled.load(std::memory_order_relaxed) ? L"Startup Logs ON" :
                        L"Startup Logs ON - paused until Save Logs is enabled"));
            } else if (i == 2) SetPage(Page::ConfirmClearLogs);
            else GoBack();
            break;
        case Page::ConfirmClearLogs:
            if (i == 0) {
                if (RequestClearRuntimeLogs()) {
                    g_performance_session.fetch_add(1, std::memory_order_release);
                    Notify(L"Clearing runtime logs...");
                } else Notify(L"Log clear unavailable or already in progress");
            }
            GoBack();
            break;
        case Page::Hotkeys:
            if (i >= 0 && static_cast<size_t>(i) < kHotkeyCategories.size())
                SetPage(kHotkeyCategories[static_cast<size_t>(i)].page);
            else if (i == static_cast<int>(kHotkeyCategories.size())) {
                s_hotkey_profiles_return_page = Page::Hotkeys;
                RefreshHotkeyProfiles();
                SetPage(Page::HotkeyProfiles);
            } else if (i == static_cast<int>(kHotkeyCategories.size()) + 1) {
                for (size_t n = 0; n < kActionCount; ++n) g_keys[n].store(kActions[n].default_key);
                SaveSettings();
                Notify(L"Default hotkeys restored");
            } else GoBack();
            break;
        case Page::HotkeysGeneral: case Page::HotkeysMovement:
        case Page::HotkeysPractice: case Page::HotkeysHUD:
        case Page::HotkeysWorld: case Page::HotkeysGhost: {
            const size_t action = HotkeyActionAtRow(g_page, i);
            if (action < kActionCount) {
                g_rebind_action = static_cast<int>(action);
                g_rebinding.store(true);
                Notify(L"Press a key (Esc / Backspace = None)");
            } else GoBack();
            break;
        }
    }
}

namespace {
// Left/Right settings for movement pages. Each branch returns once
// its original action has been handled.
void HandleHorizontalMovement(int key, MyCharacter* player) {
    switch (g_page) {
        case Page::MovementPhysics: {
            if (g_page == Page::MovementPhysics && g_selection == 0) {
                ExecuteAction(key == VK_RIGHT ? Action::SpeedUp : Action::SpeedDown, player);
                return;
            }
            if (g_page == Page::MovementPhysics && g_selection == 1) {
                AdjustMovementPercent(true, key == VK_RIGHT ? +1 : -1);
                return;
            }
            if (g_page == Page::MovementPhysics && g_selection == 2) {
                AdjustSwimSpeedPercent(key == VK_RIGHT ? +1 : -1);
                return;
            }
            if (g_page == Page::MovementPhysics && g_selection == 3) {
                AdjustClimbSpeedPercent(key == VK_RIGHT ? +1 : -1);
                return;
            }
            if (g_page == Page::MovementPhysics && g_selection == 4) {
                AdjustSlidePercent(true, key == VK_RIGHT ? +1 : -1);
                return;
            }
            if (g_page == Page::MovementPhysics && g_selection == 5) {
                AdjustSlidePercent(false, key == VK_RIGHT ? +1 : -1);
                return;
            }
            if (g_page == Page::MovementPhysics && g_selection == 6) {
                AdjustMovementPercent(false, key == VK_RIGHT ? +1 : -1);
                return;
            }
            if (g_page == Page::MovementPhysics && g_selection == 7) {
                AdjustAccelerationPercent(key == VK_RIGHT ? +1 : -1);
                return;
            }
            if (g_page == Page::MovementPhysics && g_selection == 8) {
                AdjustGameSpeedPercent(key == VK_RIGHT ? +1 : -1);
                return;
            }
            return;
        }
        case Page::MovementModes: {
            if (g_page == Page::MovementModes && g_selection == 2) {
                TogglePreserveMomentum();
                return;
            }
            return;
        }
        case Page::MovementJump: {
            if (g_page == Page::MovementJump && g_selection == 0) {
                CycleJumpAssistMode(true, key == VK_RIGHT ? +1 : -1);
                return;
            }
            if (g_page == Page::MovementJump && g_selection == 1) {
                CycleJumpAssistMode(false, key == VK_RIGHT ? +1 : -1);
                return;
            }
            if (g_page == Page::MovementJump && g_selection == 2) {
                ToggleAirJump();
                return;
            }
            return;
        }
        default: return;
    }
}

// Left/Right settings for world pages. Each branch returns once
// its original action has been handled.
void HandleHorizontalWorld(int key, MyCharacter* player) {
    switch (g_page) {
        case Page::World: {
            if (g_page == Page::World && g_selection == 9) {
                SetAllWorldObjectsRemoved(!AreAllWorldObjectsRemoved());
                return;
            }
            return;
        }
        case Page::WorldCubes: {
            if (g_page == Page::WorldCubes && g_selection == 5) {
                SetWorldObjectRemoved(WorldObjectKind::Cubes,
                                    !IsWorldObjectRemoved(WorldObjectKind::Cubes));
                return;
            }
            return;
        }
        case Page::WorldButtonActions: {
            if (g_page == Page::WorldButtonActions && g_selection == 9) {
                SetWorldObjectRemoved(WorldObjectKind::Buttons,
                                    !IsWorldObjectRemoved(WorldObjectKind::Buttons));
                return;
            }
            if (g_page == Page::WorldButtonActions && g_selection == 8) {
                ToggleButtonTileSelection();
                return;
            }
            return;
        }
        case Page::WorldTileActions: {
            if (g_page == Page::WorldTileActions && g_selection == 5) {
                SetWorldObjectRemoved(WorldObjectKind::Tiles,
                                    !IsWorldObjectRemoved(WorldObjectKind::Tiles));
                return;
            }
            if (g_page == Page::WorldTileActions && g_selection == 4) {
                ToggleTileSelectionOnContact();
                return;
            }
            return;
        }
        case Page::WorldTubeSpeed: {
            if (g_page == Page::WorldTubeSpeed && g_selection == 2) {
                SetWorldObjectRemoved(WorldObjectKind::Tubes,
                                    !IsWorldObjectRemoved(WorldObjectKind::Tubes));
                return;
            }
            if (g_page == Page::WorldTubeSpeed && g_selection == 0) {
                AdjustTubeTravelSpeed(key == VK_RIGHT ? +1 : -1);
                return;
            }
            return;
        }
        case Page::WorldPlatformSpeed: {
            if (g_page == Page::WorldPlatformSpeed && g_selection == 2) {
                SetWorldObjectRemoved(WorldObjectKind::Platforms,
                                    !IsWorldObjectRemoved(WorldObjectKind::Platforms));
                return;
            }
            if (g_page == Page::WorldPlatformSpeed && g_selection == 0) {
                AdjustPlatformSpeed(key == VK_RIGHT ? +1 : -1);
                return;
            }
            return;
        }
        case Page::WorldJumpPadHeight: {
            if (g_page == Page::WorldJumpPadHeight && g_selection == 2) {
                SetWorldObjectRemoved(WorldObjectKind::JumpPads,
                                    !IsWorldObjectRemoved(WorldObjectKind::JumpPads));
                return;
            }
            if (g_page == Page::WorldJumpPadHeight && g_selection == 0) {
                AdjustJumpPadHeight(key == VK_RIGHT ? +1 : -1);
                return;
            }
            return;
        }
        case Page::WorldSections: {
            if (g_page == Page::WorldSections && g_selection == 0) {
                g_animate_section_changes = !g_animate_section_changes;
                if (!g_animate_section_changes) {
                    FinishSectionPlacementAnimations();
                    g_recording_tainted = true;
                    if (!g_pb_cheats_allowed_for_run) g_run_pb_eligible = false;
                }
                SaveSettings();
                Notify(g_animate_section_changes
                    ? L"Section animation ON: normal automatic rises"
                    : L"Section animation OFF: automatic rises instant (assisted)");
                return;
            }
            if (g_page == Page::WorldSections && g_selection == 4) {
                if (SetButtonsSyncWithSections(!g_buttons_sync_with_sections))
                    SaveSettings();
                return;
            }
            return;
        }
        case Page::WorldButtonColours: {
            if (g_page == Page::WorldButtonColours &&
                     (g_selection == 0 || g_selection == 2)) {
                const bool pressed = g_selection == 2;
                SetButtonCustomColourEnabled(pressed,
                    !IsButtonCustomColourEnabled(pressed));
                return;
            }
            if (g_page == Page::WorldButtonColours &&
                     (g_selection == 4 || g_selection == 6)) {
                const bool pressed = g_selection == 6;
                SetButtonBorderCustomColourEnabled(pressed,
                    !IsButtonBorderCustomColourEnabled(pressed));
                return;
            }
            if (g_page == Page::WorldButtonColours &&
                     (g_selection == 1 || g_selection == 3 || g_selection == 5 ||
                     g_selection == 7)) {
                const int role = g_selection / 2;
                s_button_picker_method[role] = !s_button_picker_method[role];
                return;
            }
            return;
        }
        case Page::WorldButtonBeacons: {
            if (g_page == Page::WorldButtonBeacons && g_selection == 0) {
                SetButtonBeaconVisible(!IsButtonBeaconVisible());
                return;
            }
            if (g_page == Page::WorldButtonBeacons && g_selection == 1) {
                SetButtonBeaconCustomColourEnabled(!IsButtonBeaconCustomColourEnabled());
                return;
            }
            if (g_page == Page::WorldButtonBeacons && g_selection == 2) {
                s_button_picker_method[4] = !s_button_picker_method[4];
                return;
            }
            return;
        }
        case Page::WorldIndividualSections: {
            if (g_page == Page::WorldIndividualSections && g_selection >= 0 &&
                     g_selection < kPracticeClusterCount) {
                CycleSectionPlacement(g_selection + 1, key == VK_RIGHT ? +1 : -1);
                return;
            }
            return;
        }
        case Page::WorldSectionSpeed: {
            if (g_page == Page::WorldSectionSpeed && g_selection == 0) {
                AdjustSectionSpeed(key == VK_RIGHT ? +1 : -1);
                return;
            }
            return;
        }
        case Page::WorldSectionHeights: {
            if (g_page == Page::WorldSectionHeights &&
                     g_selection >= 0 && g_selection < 3) {
                AdjustAllSectionPositionOffsets(g_selection, key == VK_RIGHT ? +1 : -1);
                return;
            }
            return;
        }
        case Page::WorldSectionPositionEdit: {
            if (g_page == Page::WorldSectionPositionEdit &&
                     g_selection >= 0 && g_selection < 3) {
                AdjustSectionPositionOffset(s_position_edit_section, g_selection,
                                                            key == VK_RIGHT ? +1 : -1);
                return;
            }
            return;
        }
        case Page::WorldPlayer: {
            if (g_page == Page::WorldPlayer && g_selection == 0) {
                AdjustPlayerFovDegrees(key == VK_RIGHT ? +1 : -1);
                return;
            }
            return;
        }
        case Page::WorldWater: {
            if (g_page == Page::WorldWater && g_selection == 0) {
                SetAllWaterRemoved(!IsAllWaterRemoved());
                return;
            }
            if (g_page == Page::WorldWater && g_selection == 1) {
                SetWaterRemoved(!IsWaterRemoved());
                return;
            }
            if (g_page == Page::WorldWater && g_selection == 2) {
                SetWaterSwimmingDisabled(!IsWaterSwimmingDisabled());
                return;
            }
            if (g_page == Page::WorldWater && g_selection == 3) {
                AdjustWaterHeightOffset(key == VK_RIGHT ? +1 : -1);
                return;
            }
            return;
        }
        case Page::WorldWaterColours: {
            if (g_page == Page::WorldWaterColours && g_selection == 0) {
                SetWaterCustomColourEnabled(!IsWaterCustomColourEnabled());
                return;
            }
            if (g_page == Page::WorldWaterColours && g_selection == 1) {
                s_water_use_windows_picker = !s_water_use_windows_picker;
                return;
            }
            if (g_page == Page::WorldWaterColours && g_selection == 2) {
                AdjustWaterColourIntensity(key == VK_RIGHT ? +1 : -1);
                return;
            }
            if (g_page == Page::WorldWaterColours && g_selection == 3) {
                SetUnderwaterCustomColourEnabled(!IsUnderwaterCustomColourEnabled());
                return;
            }
            if (g_page == Page::WorldWaterColours && g_selection == 4) {
                s_underwater_use_windows_picker = !s_underwater_use_windows_picker;
                return;
            }
            if (g_page == Page::WorldWaterColours && g_selection == 5) {
                AdjustUnderwaterColourIntensity(key == VK_RIGHT ? +1 : -1);
                return;
            }
            if (g_page == Page::WorldWaterColours && g_selection == 6) {
                SetUnderwaterTintRemoved(!IsUnderwaterTintRemoved());
                return;
            }
            if (g_page == Page::WorldWaterColours && g_selection == 7) {
                SetWaterSplashesRemoved(!AreWaterSplashesRemoved());
                return;
            }
            return;
        }
        case Page::WaterReflections:
            ToggleWaterReflectionRow(g_selection);
            return;
        case Page::WorldJumpPadColours: {
            if (g_page == Page::WorldJumpPadColours && g_selection == 0) {
                SetJumpPadCustomColourEnabled(!IsJumpPadCustomColourEnabled());
                return;
            }
            if (g_page == Page::WorldJumpPadColours && g_selection == 1) {
                s_jumppad_use_windows_picker = !s_jumppad_use_windows_picker;
                return;
            }
            return;
        }
        case Page::WorldPlatformColours: {
            if (g_page == Page::WorldPlatformColours && g_selection == 0) {
                SetPlatformCustomColourEnabled(!IsPlatformCustomColourEnabled());
                return;
            }
            if (g_page == Page::WorldPlatformColours && g_selection == 1) {
                s_platform_use_windows_picker = !s_platform_use_windows_picker;
                return;
            }
            return;
        }
        case Page::WorldTubeColours: {
            if (g_page == Page::WorldTubeColours && g_selection == 0) {
                SetTubeCustomColourEnabled(!IsTubeCustomColourEnabled());
                return;
            }
            if (g_page == Page::WorldTubeColours && g_selection == 1) {
                s_tube_use_windows_picker = !s_tube_use_windows_picker;
                return;
            }
            return;
        }
        case Page::WorldTileColours: {
            if (g_page == Page::WorldTileColours &&
                     (g_selection == 0 || g_selection == 3)) {
                CycleTileColourPreset(g_selection == 0, key == VK_RIGHT ? +1 : -1);
                return;
            }
            if (g_page == Page::WorldTileColours && g_selection == 2) {
                ToggleRandomTileRGB();
                return;
            }
            if (g_page == Page::WorldTileColours &&
                     (g_selection == 1 || g_selection == 4)) {
                CycleTileColourMethod(g_selection == 1);
                return;
            }
            return;
        }
        case Page::WorldWalls: {
            if (g_page == Page::WorldWalls &&
                     (g_selection == 0 || g_selection == 2)) {
                const bool selected = g_selection == 0;
                SetWallCustomColourEnabled(selected,
                    !IsWallCustomColourEnabled(selected));
                return;
            }
            if (g_page == Page::WorldWalls &&
                     (g_selection == 1 || g_selection == 3)) {
                const int index = g_selection == 1 ? 0 : 1;
                s_wall_use_windows_picker[index] = !s_wall_use_windows_picker[index];
                return;
            }
            return;
        }
        default: return;
    }
}

// Left/Right settings for hud pages. Each branch returns once
// its original action has been handled.
void HandleHorizontalHud(int key, MyCharacter* player) {
    switch (g_page) {
        case Page::HudGameplay:
        case Page::SplitHud: {
            const bool splits = g_page == Page::SplitHud;
            const size_t count = splits ? kSplitHudElements.size() : kGeneralHudElements.size();
            if (g_selection >= 0 && static_cast<size_t>(g_selection) < count) {
                ToggleHudElement(splits ? kSplitHudElements[g_selection]
                                        : kGeneralHudElements[g_selection]);
                return;
            }
            return;
        }
        case Page::HudKeyDisplay: {
            if (g_page == Page::HudKeyDisplay && g_selection == 0) {
                ToggleHudElement(HudElement::KeyDisplay);
                return;
            }
            return;
        }
        case Page::HudRecordTimes: {
            if (g_page == Page::HudRecordTimes && g_selection == 3) {
                CycleRecordTimeHudMode(g_show_compare_hud, g_compare_hud_view,
                                                       key == VK_RIGHT ? +1 : -1);
                return;
            }
            if (g_page == Page::HudRecordTimes && g_selection == 4) {
                CycleRecordTimeHudMode(g_show_best_segment_time_hud, g_best_segment_time_view,
                                                       key == VK_RIGHT ? +1 : -1);
                return;
            }
            if (g_page == Page::HudRecordTimes && g_selection == 5) {
                CycleRecordTimeHudMode(g_show_pb_split_time_hud, g_pb_split_time_view,
                                                       key == VK_RIGHT ? +1 : -1);
                return;
            }
            return;
        }
        case Page::HudAppearanceEdit: {
            if (g_page == Page::HudAppearanceEdit && g_selection == 0) {
                CycleHudCorner(s_hud_style_element, key == VK_RIGHT ? +1 : -1);
                return;
            }
            if (g_page == Page::HudAppearanceEdit && g_selection == 1) {
                CycleHudColour(key == VK_RIGHT ? +1 : -1);
                return;
            }
            if (g_page == Page::HudAppearanceEdit && g_selection == 2) {
                CycleHudCustomColourMethod();
                return;
            }
            if (g_page == Page::HudAppearanceEdit && g_selection == 3) {
                AdjustHudTextSize(key == VK_RIGHT ? +1 : -1);
                return;
            }
            if (g_page == Page::HudAppearanceEdit && g_selection == 4) {
                ToggleHudHeader();
                return;
            }
            if (g_page == Page::HudAppearanceEdit && g_selection == 5 &&
                     HudElementHasView(static_cast<HudElement>(s_hud_style_element))) {
                ToggleHudElementView(static_cast<HudElement>(s_hud_style_element));
                return;
            }
            return;
        }
        case Page::HudAppearance: {
            if (g_selection == static_cast<int>(kGeneralHudElements.size())) {
                CycleMenuPosition(key == VK_RIGHT ? +1 : -1);
                return;
            }
            return;
        }
        case Page::HudPositions: {
            if (g_page == Page::HudPositions && g_selection >= 0 &&
                     static_cast<size_t>(g_selection) < kHudElementCount) {
                CycleHudCorner(static_cast<size_t>(g_selection), key == VK_RIGHT ? +1 : -1);
                return;
            }
            if (g_page == Page::HudPositions &&
                     g_selection == static_cast<int>(kHudElementCount)) {
                CycleMenuPosition(key == VK_RIGHT ? +1 : -1);
                return;
            }
            return;
        }
        case Page::SplitDisplay: {
            if (g_page == Page::SplitDisplay && g_selection == 2) {
                ChangeSplitRowMode(g_segment_row_mode, key == VK_RIGHT ? +1 : -1);
                return;
            }
            if (g_page == Page::SplitDisplay && g_selection == 3) {
                ChangeSplitRowMode(g_pb_row_mode, key == VK_RIGHT ? +1 : -1);
                return;
            }
            if (g_page == Page::SplitDisplay && g_selection == 4) {
                CycleFastestDeltaMode(key == VK_RIGHT ? +1 : -1);
                return;
            }
            if (g_page == Page::SplitDisplay && g_selection == 5) {
                CycleComparePopupDisplayMode(key == VK_RIGHT ? +1 : -1);
                return;
            }
            if (g_page == Page::SplitDisplay && g_selection == 6) {
                ChangeSectionCompareMode(g_section_vs_pb_display, key == VK_RIGHT ? +1 : -1);
                return;
            }
            if (g_page == Page::SplitDisplay && g_selection == 7) {
                ChangeSectionCompareMode(g_section_vs_file_display, key == VK_RIGHT ? +1 : -1);
                return;
            }
            if (g_page == Page::SplitDisplay && g_selection == 8) {
                ChangeSectionCompareMode(g_pb_gain_loss_display, key == VK_RIGHT ? +1 : -1);
                return;
            }
            return;
        }
        case Page::HudComparisons: {
            if (g_page == Page::HudComparisons && g_selection == 0) {
                CycleCompareMode();
                return;
            }
            if (g_page == Page::HudComparisons && g_selection == 1) {
                if (!CycleComparisonProfile(key == VK_RIGHT ? +1 : -1))
                    Notify(L"No other split profile available");
                return;
            }
            return;
        }
        default: return;
    }
}

// Left/Right settings for customisation pages. Each branch returns once
// its original action has been handled.
void HandleHorizontalCustomisation(int key, MyCharacter* player) {
    switch (g_page) {
        case Page::CustomisationTime: {
            if (g_page == Page::CustomisationTime && g_selection == 0) {
                SetCustomTimeOfDayEnabled(!IsCustomTimeOfDayEnabled());
                return;
            }
            if (g_page == Page::CustomisationTime && g_selection == 1) {
                AdjustCustomTimeMinutes(key == VK_RIGHT ? +1 : -1);
                return;
            }
            if (g_page == Page::CustomisationTime && g_selection == 2) {
                SetCustomTimeLocked(!IsCustomTimeLocked());
                return;
            }
            if (g_page == Page::CustomisationTime && g_selection == 3) {
                AdjustCustomTimeSpeedPercent(key == VK_RIGHT ? +1 : -1);
                return;
            }
            return;
        }
        case Page::CustomisationSkyColours: {
            if (g_page == Page::CustomisationSkyColours && g_selection == 0) {
                SetSkyColourEnabled(SkyRegion::Zenith, !IsSkyColourEnabled(SkyRegion::Zenith));
                return;
            }
            if (g_page == Page::CustomisationSkyColours && g_selection == 1) {
                s_sky_use_windows_picker[0] = !s_sky_use_windows_picker[0];
                return;
            }
            if (g_page == Page::CustomisationSkyColours && g_selection == 2) {
                SetSkyColourEnabled(SkyRegion::Horizon, !IsSkyColourEnabled(SkyRegion::Horizon));
                return;
            }
            if (g_page == Page::CustomisationSkyColours && g_selection == 3) {
                s_sky_use_windows_picker[1] = !s_sky_use_windows_picker[1];
                return;
            }
            if (g_page == Page::CustomisationSkyColours && g_selection == 4) {
                SetSkyColourEnabled(SkyRegion::Sun, !IsSkyColourEnabled(SkyRegion::Sun));
                return;
            }
            if (g_page == Page::CustomisationSkyColours && g_selection == 5) {
                s_sky_use_windows_picker[2] = !s_sky_use_windows_picker[2];
                return;
            }
            if (g_page == Page::CustomisationSkyColours && g_selection == 6) {
                SetSkyColourEnabled(SkyRegion::Clouds, !IsSkyColourEnabled(SkyRegion::Clouds));
                return;
            }
            if (g_page == Page::CustomisationSkyColours && g_selection == 7) {
                s_sky_use_windows_picker[3] = !s_sky_use_windows_picker[3];
                return;
            }
            return;
        }
        case Page::CustomisationSkyVisibility: {
            if (g_page == Page::CustomisationSkyVisibility && g_selection == 0) {
                SetSkyDiscVisible(!IsSkyDiscVisible());
                return;
            }
            if (g_page == Page::CustomisationSkyVisibility && g_selection == 1) {
                SetSkyEffectVisible(SkyEffect::Clouds, !IsSkyEffectVisible(SkyEffect::Clouds));
                return;
            }
            if (g_page == Page::CustomisationSkyVisibility && g_selection == 2) {
                SetSkyEffectVisible(SkyEffect::Stars, !IsSkyEffectVisible(SkyEffect::Stars));
                return;
            }
            return;
        }
        case Page::CustomisationFogColours: {
            if (g_page == Page::CustomisationFogColours && g_selection == 0) {
                SetFogColourEnabled(FogRegion::General, !IsFogColourEnabled(FogRegion::General));
                return;
            }
            if (g_page == Page::CustomisationFogColours && g_selection == 1) {
                s_fog_use_windows_picker[0] = !s_fog_use_windows_picker[0];
                return;
            }
            if (g_page == Page::CustomisationFogColours && g_selection == 2) {
                SetFogColourEnabled(FogRegion::Directional, !IsFogColourEnabled(FogRegion::Directional));
                return;
            }
            if (g_page == Page::CustomisationFogColours && g_selection == 3) {
                s_fog_use_windows_picker[1] = !s_fog_use_windows_picker[1];
                return;
            }
            return;
        }
        case Page::CustomisationFogVisibility: {
            if (g_page == Page::CustomisationFogVisibility && g_selection == 0) {
                SetFogVisible(FogRegion::General, !IsFogVisible(FogRegion::General));
                return;
            }
            if (g_page == Page::CustomisationFogVisibility && g_selection == 1) {
                SetFogVisible(FogRegion::Directional, !IsFogVisible(FogRegion::Directional));
                return;
            }
            return;
        }
        case Page::CustomisationShadows: {
            if (g_page == Page::CustomisationShadows && g_selection == 0) {
                SetShadowsVisible(!AreShadowsVisible());
                return;
            }
            if (g_page == Page::CustomisationShadows && g_selection == 1) {
                SetShadowAmbientColourEnabled(!IsShadowAmbientColourEnabled());
                return;
            }
            if (g_page == Page::CustomisationShadows && g_selection == 2) {
                s_shadow_use_windows_picker = !s_shadow_use_windows_picker;
                return;
            }
            return;
        }
        case Page::CustomisationPillars: {
            if (g_page == Page::CustomisationPillars && g_selection == 0) {
                SetPillarColourEnabled(!IsPillarColourEnabled());
                return;
            }
            if (g_page == Page::CustomisationPillars && g_selection == 1) {
                s_pillar_use_windows_picker = !s_pillar_use_windows_picker;
                return;
            }
            if (g_page == Page::CustomisationPillars && g_selection == 2) {
                SetPillarsRemoved(!ArePillarsRemoved());
                return;
            }
            return;
        }
        default: return;
    }
}

// Left/Right settings for recordsandsettings pages. Each branch returns once
// its original action has been handled.
void HandleHorizontalRecordsAndSettings(int key, MyCharacter* player) {
    switch (g_page) {
        case Page::ButtonCheckpoints: {
            if (g_page == Page::ButtonCheckpoints && g_selection == 2) {
                g_keep_last_button_momentum = !g_keep_last_button_momentum;
                SaveSettings();
                return;
            }
            return;
        }
        case Page::Persistence: {
            if (g_page == Page::Persistence && g_selection >= 0 &&
                     g_selection < 4) {
                if (g_selection < 2) ToggleMovementPersistence(g_selection == 0);
                else ToggleWorldPersistence(g_selection == 2);
                return;
            }
            return;
        }
        case Page::GameplayRecords: {
            if (g_selection <= 1) {
                ToggleRecordSaving(g_selection == 0);
                return;
            }
            if (g_selection == 2) {
                g_save_pb_ghosts = !g_save_pb_ghosts;
                SaveSettings();
                return;
            }
            if (g_selection == 3) {
                g_save_fastest_ghosts = !g_save_fastest_ghosts;
                SaveSettings();
                return;
            }
            if (g_selection == 4) {
                CycleCheatRecordMode(key == VK_RIGHT ? +1 : -1);
                return;
            }
            if (g_selection == 5) {
                g_auto_create_pb_profile = !g_auto_create_pb_profile;
                SaveSettings();
                return;
            }
            if (g_selection == 6) {
                g_auto_pb_copy_best_segments = !g_auto_pb_copy_best_segments;
                SaveSettings();
                return;
            }
            return;
        }
        case Page::GhostReplay: {
            if (g_selection == 1) CycleReplayProfile(key == VK_RIGHT ? +1 : -1);
            else if (g_selection == 2) ToggleGhostReplaySource();
            else if (g_selection == 7) CycleGhostReplaySpeed(key == VK_RIGHT ? +1 : -1);
            else if (g_selection == 9) ToggleGhostReplayCameraMode();
            else if (g_selection == 5) SeekGhostReplaySeconds(-5);
            else if (g_selection == 6) SeekGhostReplaySeconds(+5);
            return;
        }
        case Page::Ghost: {
            if (g_page == Page::Ghost) {
                switch (g_selection) {
                    case 0: SetPBGhostEnabled(!IsPBGhostEnabled()); return;
                    case 1: SetPBGhostPassThroughEnabled(!IsPBGhostPassThroughEnabled()); return;
                    case 2: SetPBGhost2DTransparencyEnabled(!IsPBGhost2DTransparencyEnabled()); return;
                    case 3: SetPBGhost3DTransparencyEnabled(!IsPBGhost3DTransparencyEnabled()); return;
                    case 4: s_ghost_use_windows_picker = !s_ghost_use_windows_picker; return;
                    case 5: SetPBGhostButtonSyncEnabled(!IsPBGhostButtonSyncEnabled()); return;
                    case 6: SetPBGhostFastestSectionsEnabled(!IsPBGhostFastestSectionsEnabled()); return;
                    default: break;
                }
            }
            return;
        }
        case Page::RestartOptions: {
            if (g_page == Page::RestartOptions && g_selection == 3) {
                ToggleProgressRiseAnimation();
                return;
            }
            return;
        }
        case Page::Practice: {
            if (g_page == Page::Practice && g_selection == 0) {
                g_practice_selected_button = std::clamp(g_practice_selected_button +
                    (key == VK_RIGHT ? 1 : -1), 1, kRefunctButtonCount);
                SaveSettings();
                return;
            }
            return;
        }
        default: return;
    }
}

// Keep ProcessKey focused on input routing. The handlers above are
// split by menu category to keep C++ nesting and function size bounded.
void HandleHorizontalMenuKey(int key, MyCharacter* player) {
    switch (g_page) {
        case Page::Crosshair:
            AdjustCrosshairRow(g_selection, key == VK_RIGHT ? +1 : -1);
            return;
        case Page::MovementPhysics:
        case Page::MovementModes:
        case Page::MovementJump:
            HandleHorizontalMovement(key, player);
            return;
        case Page::World:
        case Page::WorldCubes:
        case Page::WorldButtonActions:
        case Page::WorldTileActions:
        case Page::WorldTubeSpeed:
        case Page::WorldPlatformSpeed:
        case Page::WorldJumpPadHeight:
        case Page::WorldSections:
        case Page::WorldButtonColours:
        case Page::WorldButtonBeacons:
        case Page::WorldIndividualSections:
        case Page::WorldSectionSpeed:
        case Page::WorldSectionHeights:
        case Page::WorldSectionPositionEdit:
        case Page::WorldPlayer:
        case Page::WorldWater:
        case Page::WorldWaterColours:
        case Page::WaterReflections:
        case Page::WorldJumpPadColours:
        case Page::WorldPlatformColours:
        case Page::WorldTubeColours:
        case Page::WorldTileColours:
        case Page::WorldWalls:
            HandleHorizontalWorld(key, player);
            return;
        case Page::HudGameplay:
        case Page::SplitHud:
        case Page::HudKeyDisplay:
        case Page::HudRecordTimes:
        case Page::HudAppearanceEdit:
        case Page::HudAppearance:
        case Page::HudPositions:
        case Page::SplitDisplay:
        case Page::HudComparisons:
            HandleHorizontalHud(key, player);
            return;
        case Page::CustomisationTime:
        case Page::CustomisationSkyColours:
        case Page::CustomisationSkyVisibility:
        case Page::CustomisationFogColours:
        case Page::CustomisationFogVisibility:
        case Page::CustomisationShadows:
        case Page::CustomisationPillars:
            HandleHorizontalCustomisation(key, player);
            return;
        case Page::ButtonCheckpoints:
        case Page::Persistence:
        case Page::GameplayRecords:
        case Page::Ghost:
        case Page::GhostReplay:
        case Page::RestartOptions:
        case Page::Practice:
            HandleHorizontalRecordsAndSettings(key, player);
            return;
        default: return;
    }
}

} // namespace

void ProcessKey(int key, MyCharacter* player) {
    if (s_button_hex_role >= 0) {
        HandleButtonHexKey(key); // HEX letters must never trigger hotkeys.
        return;
    }
    if (s_tile_hex_axis >= 0) {
        ProcessTileHexKey(key); // HEX letters must not trigger normal hotkeys.
        return;
    }
    if (g_coordinate_editing.load(std::memory_order_relaxed)) {
        if (key == Hotkey(Action::Menu) ||
            (key == VK_F5 && Hotkey(Action::Menu) == 0)) {
            FinishCoordinateEdit(false);
            ExecuteAction(Action::Menu, player);
        } else {
            ProcessCoordinateEditKey(key);
        }
        return;
    }
    if (g_name_editing.load()) {
        ProcessProfileNameKey(key);
        return;
    }
    // During rebinding, the next key sets the new hotkey, not an action.
    if (g_rebinding.load()) {
        if (key == VK_BACK || key == VK_ESCAPE) {
            // Escape deliberately clears the current binding (not cancel).
            if (g_rebind_action >= 0 && static_cast<size_t>(g_rebind_action) < kActionCount) {
                if (!CanClearHotkey(static_cast<size_t>(g_rebind_action))) return;
                g_keys[g_rebind_action].store(0);
                SaveSettings();
                Notify(L"Hotkey cleared: None");
            }
            g_rebind_action = -1;
            g_rebinding.store(false);
            return;
        }
        // An unbound menu retains F5 as a recovery key. Do not steal it for
        // another action until a working menu key has been assigned.
        if (key == VK_F5 && Hotkey(Action::Menu) == 0 &&
            g_rebind_action != static_cast<int>(Action::Menu)) {
            Notify(L"F5 is reserved while the menu hotkey is None");
            return;
        }
        if (!IsBindable(key)) { Notify(L"That key is reserved"); return; }
        for (size_t i = 0; i < kActionCount; ++i) {
            if (g_keys[i].load() == key && static_cast<int>(i) != g_rebind_action) {
                Notify(L"That key is already assigned");
                return;
            }
        }
        if (g_rebind_action >= 0 && static_cast<size_t>(g_rebind_action) < kActionCount) {
            g_keys[g_rebind_action].store(key);
            SaveSettings();
            Notify(L"Hotkey saved: " + KeyName(key));
        }
        g_rebind_action = -1;
        g_rebinding.store(false);
        return;
    }

    // Search mode owns character keys before any gameplay/mod hotkeys.
    // The menu key still closes/reopens the overlay. Search results only
    // navigate to the real menu row; a second Enter is required to use it.
    if (g_menu_open.load() && g_page == Page::Search) {
        if (key == Hotkey(Action::Menu) ||
            (key == VK_F5 && Hotkey(Action::Menu) == 0)) {
            ExecuteAction(Action::Menu, player); return;
        }
        if (key == VK_ESCAPE) { GoBack(); return; }
        if (key == VK_RETURN) { ActivateSelected(player); return; }
        if (key == VK_UP || key == VK_DOWN) {
            const int count = static_cast<int>(Entries().size());
            if (count > 0) g_selection = (g_selection + count +
                (key == VK_DOWN ? 1 : -1)) % count;
            return;
        }
        if (key == VK_BACK) {
            if (!s_menu_search_query.empty()) {
                s_menu_search_query.pop_back();
                RefreshMenuSearch();
                g_selection = s_menu_search_matches.empty() ? 0 : 1;
            }
            return;
        }
        // Don't turn Ctrl/Alt shortcuts into search characters.
        if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) ||
            (GetAsyncKeyState(VK_MENU) & 0x8000)) return;
        wchar_t ch = 0;
        if (key >= 'A' && key <= 'Z') ch = static_cast<wchar_t>(key);
        else if (key >= '0' && key <= '9') ch = static_cast<wchar_t>(key);
        else if (key >= VK_NUMPAD0 && key <= VK_NUMPAD9)
            ch = static_cast<wchar_t>(L'0' + key - VK_NUMPAD0);
        else if (key == VK_SPACE) ch = L' ';
        else if (key == VK_OEM_MINUS || key == VK_SUBTRACT) ch = L'-';
        else if (key == VK_OEM_PERIOD || key == VK_DECIMAL) ch = L'.';
        if (ch && s_menu_search_query.size() < kMaxMenuSearchLength) {
            s_menu_search_query.push_back(ch);
            RefreshMenuSearch();
            g_selection = s_menu_search_matches.empty() ? 0 : 1;
        }
        return;
    }
    // Press / while browsing any normal submenu to start a fresh search.
    if (g_menu_open.load() && key == VK_OEM_2 &&
        !(GetAsyncKeyState(VK_CONTROL) & 0x8000) &&
        !(GetAsyncKeyState(VK_MENU) & 0x8000)) {
        OpenMenuSearch();
        return;
    }

    // Backspace also clears the highlighted binding without entering capture.
    // Use the category row mapping, not the action's original numeric index.
    if (g_menu_open.load() && IsHotkeyCategoryPage(g_page) && key == VK_BACK) {
        const size_t action = HotkeyActionAtRow(g_page, g_selection);
        if (action < kActionCount) {
            if (!CanClearHotkey(action)) return;
            g_keys[action].store(0);
            SaveSettings();
            Notify(L"Hotkey cleared: None");
        }
        return;
    }
    // F5 remains a recovery key if Menu has deliberately been set to None.
    if (key == VK_F5 && Hotkey(Action::Menu) == 0) {
        ExecuteAction(Action::Menu, player);
        return;
    }
    // Zero is the unassigned sentinel, never a dispatchable key.
    for (size_t i = 0; i < kActionCount; ++i) {
        if (key != 0 && key == g_keys[i].load()) {
            ExecuteAction(static_cast<Action>(i), player);
            return;
        }
    }
    if (!g_menu_open.load()) return;
    const int count = static_cast<int>(Entries().size());
    if (count == 0) return;
    switch (key) {
        case VK_UP: g_selection = (g_selection + count - 1) % count; break;
        case VK_DOWN: g_selection = (g_selection + 1) % count; break;
        case VK_LEFT:
        case VK_RIGHT:
            HandleHorizontalMenuKey(key, player);
            break;
        case VK_RETURN: ActivateSelected(player); break;
        case VK_ESCAPE: GoBack(); break;
        default: break;
    }
}


void QueueMenuSearchKey(int key) {
    // Fixed-capacity FIFO. Keyboard events are infrequent and this lock is
    // held for just an integer write, never while processing a menu action.
    AcquireSRWLockExclusive(&s_menu_search_key_lock);
    if (s_menu_search_key_count < kMenuSearchKeyQueueLimit)
        s_menu_search_key_queue[s_menu_search_key_count++] = key;
    ReleaseSRWLockExclusive(&s_menu_search_key_lock);
}

void DrainMenuSearchKeys(MyCharacter* player) {
    // Take a local ordered snapshot while holding the lock; never process
    // menu actions while locked (a key could change the active menu page).
    std::array<int, kMenuSearchKeyQueueLimit> queued{};
    size_t queued_count = 0;
    AcquireSRWLockExclusive(&s_menu_search_key_lock);
    queued_count = s_menu_search_key_count;
    for (size_t i = 0; i < queued_count; ++i)
        queued[i] = s_menu_search_key_queue[i];
    s_menu_search_key_count = 0;
    ReleaseSRWLockExclusive(&s_menu_search_key_lock);

    for (size_t i = 0; i < queued_count; ++i) {
        // A result can navigate away from Search. Never reinterpret remaining
        // text as hotkeys after that transition or after the menu was closed.
        if (!g_menu_open.load(std::memory_order_relaxed) ||
            g_page != Page::Search) break;
        ProcessKey(queued[i], player);
    }
}
bool IsMenuSearchTypingKey(int key) {
    if (!g_menu_open.load(std::memory_order_relaxed) ||
        !s_menu_search_capture.load(std::memory_order_acquire))
        return false;
    return (key >= 'A' && key <= 'Z') ||
           (key >= '0' && key <= '9') ||
           (key >= VK_NUMPAD0 && key <= VK_NUMPAD9) ||
           key == VK_SPACE || key == VK_BACK || key == VK_RETURN ||
           key == VK_UP || key == VK_DOWN || key == VK_ESCAPE ||
           key == VK_SHIFT || key == VK_LSHIFT || key == VK_RSHIFT ||
           key == VK_OEM_MINUS || key == VK_SUBTRACT ||
           key == VK_OEM_PERIOD || key == VK_DECIMAL;
}
} // namespace refunct

#include "refunct/crosshair.hpp"
#include "refunct/internal_api.hpp"
#include "refunct/features/button_colours.hpp"
#include "refunct/ghost_data_paths.hpp"
#include <new>

namespace refunct {
namespace {
// This is a single game-thread cache of actual native button contacts. It is
// intentionally never serialized as a PB. A missing event must stay missing.
std::array<NativeButtonObservation, kRefunctButtonCount> s_native_button_observations{};

void ClearNativeButtonObservations() {
    s_native_button_observations.fill(NativeButtonObservation{});
}

std::wstring SectionPBKey(int section) {
    return L"Section" + std::to_wstring(section);
}

// Capture the historical individual segment records BEFORE UpdateBestSegment
// changes any of them. Earlier buttons in this same run already have their
// pre-update record stored in their ButtonSplit entry. A missed native edge
// did not update a gold, so its loaded record can still be read as-is.
double EarlierBestSectionReference(SectionBoundary boundary, int last_button,
                                   double last_previous_best) {
    if (boundary.section == 0) return -1.0;
    auto prior = g_pb.best_segment_seconds;
    for (int button = boundary.previous_end_button + 1;
         button <= boundary.end_button; ++button) {
        if (button == last_button) {
            prior[static_cast<size_t>(button - 1)] = last_previous_best;
            continue;
        }
        const auto observed = std::find_if(g_button_splits.rbegin(), g_button_splits.rend(),
            [button](const ButtonSplit& item) { return item.button_number == button; });
        if (observed != g_button_splits.rend())
            prior[static_cast<size_t>(button - 1)] =
                observed->previous_recorded_gold_seconds;
    }
    return SectionDurationFromSegments(prior, boundary);
}
} // namespace

// This bitset is deliberately only 64 bits and contains NO per-frame UObject
// discovery. All queried accessors read already-resolved settings/state.
// It drives PB eligibility, the cheats list, and the change popup together.
namespace {
enum class CheatBit : unsigned char {
    Fly, Noclip, Walk, Swim, Climb, SlideSpeed, SlideDistance, Jump,
    Acceleration, GameSpeed, Momentum, WheelJump, HoldJump, CameraFov,
    SectionSpeed, InstantRise, SectionPlacement, SectionPosition,
    ButtonSync, ButtonRepair, ButtonTileGuard, TileTouchGuard,
    TileSelectedColour, TileUnselectedColour, TileRandomColour,
    WallSelectedColour, WallUnselectedColour,
    ButtonSelectedColour, ButtonPressedColour,
    BorderSelectedColour, BorderPressedColour, BeaconHidden, BeaconColour,
    TubeColour, TubeSpeed, CheckpointMode, PreloadedRestart,
    ProgressRestart, FastRestart,
    UsedPosition, UsedButtons, UsedTiles, UsedSections, UsedCheckpoint,
    FlySpeed, FasterRestarts, LastButtonMomentum,
    PlatformColour, PlatformSpeed, JumpPadColour, JumpPadHeight, WaterColour, UsedCubes,
    AirJump, TimeOfDay, CustomMap, WaterRemoved, WaterSwimmingOff, WaterHeight,
    PillarColour, PillarRemoved, PillarVisibility, // legacy bit ID remains reserved
    WorldObjectRemoval
};
static_assert(static_cast<unsigned>(CheatBit::WorldObjectRemoval) < 64);
constexpr uint64_t CheatMask(CheatBit bit) {
    return uint64_t{1} << static_cast<unsigned>(bit);
}
uint64_t s_used_actions = 0;
uint64_t s_last_cheat_mask = 0;

// Customisation-classified settings must not invalidate PBs or appear as cheats.
// Camera FOV and pillar removal are intentionally classified as customisations
// by user preference, although they can affect perception or collision.
// Pillar removal is intentionally included by user preference even though it
// also disables collision; other gameplay modifiers remain outside this mask.
// Keep the registry and PB gate aligned with the Cosmetics HUD.
constexpr uint64_t kCustomisationMask =
    CheatMask(CheatBit::CameraFov) |
    CheatMask(CheatBit::TileSelectedColour) |
    CheatMask(CheatBit::TileUnselectedColour) |
    CheatMask(CheatBit::TileRandomColour) |
    CheatMask(CheatBit::WallSelectedColour) |
    CheatMask(CheatBit::WallUnselectedColour) |
    CheatMask(CheatBit::ButtonSelectedColour) |
    CheatMask(CheatBit::ButtonPressedColour) |
    CheatMask(CheatBit::BorderSelectedColour) |
    CheatMask(CheatBit::BorderPressedColour) |
    CheatMask(CheatBit::BeaconHidden) |
    CheatMask(CheatBit::BeaconColour) |
    CheatMask(CheatBit::TubeColour) |
    CheatMask(CheatBit::PlatformColour) |
    CheatMask(CheatBit::PillarColour) |
    CheatMask(CheatBit::PillarRemoved) |
    CheatMask(CheatBit::JumpPadColour) |
    CheatMask(CheatBit::WaterColour) |
    CheatMask(CheatBit::TimeOfDay);
static_assert((kCustomisationMask & (CheatMask(CheatBit::TubeSpeed) |
                                     CheatMask(CheatBit::PlatformSpeed) |
                                     CheatMask(CheatBit::JumpPadHeight) |
                                     CheatMask(CheatBit::SectionSpeed))) == 0);

uint64_t AllSettingMask() {
    uint64_t active = 0;
    const auto set = [&](CheatBit bit, bool value) {
        if (value) active |= CheatMask(bit);
    };
    set(CheatBit::Fly, g_flying || g_restore_flying_pending);
    set(CheatBit::Noclip, g_noclip || g_restore_noclip_pending);
    set(CheatBit::Walk, g_walk_speed_percent != 100);
    set(CheatBit::Swim, g_swim_speed_percent != 100);
    set(CheatBit::Climb, g_climb_speed_percent != 100);
    set(CheatBit::SlideSpeed, g_slide_speed_percent != 100);
    set(CheatBit::SlideDistance, g_slide_distance_percent != 100);
    set(CheatBit::Jump, g_jump_height_percent != 100);
    set(CheatBit::Acceleration, g_acceleration_percent != 100);
    set(CheatBit::GameSpeed, g_game_speed_percent != 100);
    set(CheatBit::Momentum, g_preserve_momentum);
    set(CheatBit::WheelJump, g_scroll_to_jump.load(std::memory_order_relaxed));
    set(CheatBit::HoldJump, g_hold_space_to_jump);
    set(CheatBit::AirJump, g_air_jump_enabled);
    set(CheatBit::CameraFov, g_player_fov_degrees != 0);
    set(CheatBit::SectionSpeed, IsSectionSpeedModified());
    set(CheatBit::InstantRise, !g_animate_section_changes);
    bool edited_sections = false;
    bool moved_sections = false;
    for (int i = 1; i <= kPracticeClusterCount; ++i) {
        if (GetSectionPlacement(i) != SectionPlacement::Auto)
            edited_sections = true;
        if (GetSectionPositionOffset(i, 0) != 0.0f ||
            GetSectionPositionOffset(i, 1) != 0.0f ||
            GetSectionPositionOffset(i, 2) != 0.0f)
            moved_sections = true;
    }
    set(CheatBit::SectionPlacement, edited_sections);
    set(CheatBit::SectionPosition, moved_sections);
    set(CheatBit::ButtonSync, g_buttons_sync_with_sections);
    // The read-only ghost monitor is not a cheat. A genuine automatic gate
    // repair marks ButtonRepair; a manual button edit marks UsedButtons.
    set(CheatBit::ButtonTileGuard, IsButtonTileSelectionDisabled());
    set(CheatBit::TileTouchGuard, IsTileSelectionOnContactDisabled());
    set(CheatBit::TileSelectedColour,
        IsTileCustomColourEnabled(true) || GetTileColourPreset(true) != 0);
    set(CheatBit::TileUnselectedColour,
        IsTileCustomColourEnabled(false) || GetTileColourPreset(false) != 0);
    set(CheatBit::TileRandomColour, IsRandomTileRGBEnabled());
    set(CheatBit::WallSelectedColour, IsWallCustomColourEnabled(true));
    set(CheatBit::WallUnselectedColour, IsWallCustomColourEnabled(false));
    set(CheatBit::ButtonSelectedColour, IsButtonCustomColourEnabled(false));
    set(CheatBit::ButtonPressedColour, IsButtonCustomColourEnabled(true));
    set(CheatBit::BorderSelectedColour, IsButtonBorderCustomColourEnabled(false));
    set(CheatBit::BorderPressedColour, IsButtonBorderCustomColourEnabled(true));
    set(CheatBit::BeaconHidden, !IsButtonBeaconVisible());
    set(CheatBit::BeaconColour, IsButtonBeaconCustomColourEnabled());
    set(CheatBit::TubeColour, IsTubeCustomColourEnabled());
    set(CheatBit::TubeSpeed, IsTubeTravelSpeedModified());
    set(CheatBit::PlatformColour, IsPlatformCustomColourEnabled());
    set(CheatBit::PillarColour, IsPillarColourEnabled());
    set(CheatBit::PillarRemoved, ArePillarsRemoved());
    set(CheatBit::PlatformSpeed, IsPlatformSpeedModified());
    set(CheatBit::JumpPadColour, IsJumpPadCustomColourEnabled());
    set(CheatBit::WaterColour,
        IsWaterCustomColourEnabled() || IsUnderwaterCustomColourEnabled());
    set(CheatBit::TimeOfDay, IsCustomTimeOfDayEnabled());
    set(CheatBit::CustomMap, CustomMapActive());
    bool removed_world_objects = false;
    for (unsigned i = 0; i < static_cast<unsigned>(WorldObjectKind::Count); ++i)
        removed_world_objects |= IsWorldObjectRemoved(static_cast<WorldObjectKind>(i));
    set(CheatBit::WorldObjectRemoval, removed_world_objects);
    set(CheatBit::WaterRemoved, IsWaterRemoved() || IsAllWaterRemoved());
    set(CheatBit::WaterSwimmingOff, IsWaterSwimmingDisabled() || IsAllWaterRemoved());
    set(CheatBit::WaterHeight, GetWaterHeightOffset() != 0);
    set(CheatBit::JumpPadHeight, IsJumpPadHeightModified());
    set(CheatBit::FlySpeed, g_fly_speed != 1200.0f);
    set(CheatBit::FasterRestarts, !g_progress_rise_animation_enabled);
    set(CheatBit::LastButtonMomentum, g_keep_last_button_momentum);
    return active;
}

uint64_t SettingCheatMask() { return AllSettingMask() & ~kCustomisationMask; }
uint64_t ActiveCustomisationMask() { return AllSettingMask() & kCustomisationMask; }

uint64_t ActiveCheatMask() {
    uint64_t active = SettingCheatMask() | s_used_actions;
    if (g_practice_active && g_practice_origin != PracticeOrigin::ProgressRestart)
        active |= CheatMask(CheatBit::CheckpointMode);
    if (g_preloaded_restart_active) active |= CheatMask(CheatBit::PreloadedRestart);
    if (g_progress_restart_active) active |= CheatMask(CheatBit::ProgressRestart);
    if (g_normal_fast_restart_assisted) active |= CheatMask(CheatBit::FastRestart);
    return active;
}

std::vector<std::wstring> LabelsForMask(uint64_t active) {
    std::vector<std::wstring> names;
    names.reserve(24);
    // Avoid allocating 40+ strings on every HUD frame: only construct
    // labels for enabled bits, and only format percentages when needed.
    const auto add = [&](CheatBit bit, const wchar_t* label) {
        if (active & CheatMask(bit)) names.emplace_back(label);
    };
    const auto add_percent = [&](CheatBit bit, const wchar_t* label, int value) {
        if (active & CheatMask(bit))
            names.emplace_back(std::wstring(label) + L": " + std::to_wstring(value) + L"%");
    };
    add(CheatBit::Fly, L"Flying");
    add(CheatBit::Noclip, L"Noclip");
    add_percent(CheatBit::Walk, L"Movement speed", g_walk_speed_percent);
    add_percent(CheatBit::Swim, L"Swim speed", g_swim_speed_percent);
    add_percent(CheatBit::Climb, L"Climb speed", g_climb_speed_percent);
    add_percent(CheatBit::SlideSpeed, L"Slide speed", g_slide_speed_percent);
    add_percent(CheatBit::SlideDistance, L"Slide distance", g_slide_distance_percent);
    add_percent(CheatBit::Jump, L"Jump height", g_jump_height_percent);
    add_percent(CheatBit::Acceleration, L"Acceleration", g_acceleration_percent);
    add_percent(CheatBit::GameSpeed, L"Game speed", g_game_speed_percent);
    add(CheatBit::Momentum, L"Preserve momentum");
    add(CheatBit::WheelJump, L"Scroll jump");
    add(CheatBit::HoldJump, L"Hold Space jump");
    add(CheatBit::AirJump, L"Air jump");
    if (active & CheatMask(CheatBit::CameraFov))
        names.emplace_back(L"Camera FOV: " + std::to_wstring(g_player_fov_degrees));
    add_percent(CheatBit::SectionSpeed, L"Section speed", g_section_speed_percent);
    add(CheatBit::InstantRise, L"Instant section rises");
    add(CheatBit::SectionPlacement, L"Section height/mode overrides");
    add(CheatBit::SectionPosition, L"Section XYZ offsets");
    add(CheatBit::ButtonSync, L"Buttons synced to sections");
    add(CheatBit::ButtonRepair, L"Automatic button repair (used this run)");
    add(CheatBit::ButtonTileGuard, L"Button tile selection blocked");
    add(CheatBit::TileTouchGuard, L"Tile selection on touch blocked");
    add(CheatBit::TileSelectedColour, L"Selected tile colours");
    add(CheatBit::TileUnselectedColour, L"Unselected tile colours");
    add(CheatBit::TileRandomColour, L"Random tile colours");
    add(CheatBit::WallSelectedColour, L"Selected wall colours");
    add(CheatBit::WallUnselectedColour, L"Unselected wall colours");
    add(CheatBit::ButtonSelectedColour, L"Unpressed button colours");
    add(CheatBit::ButtonPressedColour, L"Pressed button colours");
    add(CheatBit::BorderSelectedColour, L"Unpressed button borders");
    add(CheatBit::BorderPressedColour, L"Pressed button borders");
    add(CheatBit::BeaconHidden, L"Button beacons hidden");
    add(CheatBit::BeaconColour, L"Button beacon colours");
    add(CheatBit::TubeColour, L"Tube colours");
    add_percent(CheatBit::TubeSpeed, L"Tube speed", GetTubeTravelSpeedPercent());
    add(CheatBit::PlatformColour, L"Platform colours");
    add(CheatBit::PillarColour, L"Pillar colours");
    add(CheatBit::PillarRemoved, L"Pillars removed");
    add_percent(CheatBit::PlatformSpeed, L"Platform speed", GetPlatformSpeedPercent());
    add(CheatBit::JumpPadColour, L"Jump pad colours");
    add(CheatBit::WaterColour, L"Water colours");
    add(CheatBit::TimeOfDay, L"Time of day");
    add(CheatBit::CustomMap, L"Custom map loaded");
    add(CheatBit::WorldObjectRemoval, L"World objects removed");
    add(CheatBit::WaterRemoved, L"Ocean surface hidden");
    add(CheatBit::WaterSwimmingOff, L"Swimming disabled");
    if (active & CheatMask(CheatBit::WaterHeight))
        add(CheatBit::WaterHeight, L"Water height modified");
    add_percent(CheatBit::JumpPadHeight, L"Jump pad launch height", GetJumpPadHeightPercent());
    add(CheatBit::CheckpointMode, L"Section/checkpoint practice");
    add(CheatBit::PreloadedRestart, L"Preloaded level restart");
    add(CheatBit::ProgressRestart, L"Restart with progress");
    add(CheatBit::FastRestart, L"Fast-rise restart");
    add(CheatBit::UsedPosition, L"Used this run: teleport/restore position");
    add(CheatBit::UsedButtons, L"Used this run: button modifications");
    add(CheatBit::UsedTiles, L"Used this run: tile modifications");
    add(CheatBit::UsedSections, L"Used this run: section modifications");
    add(CheatBit::UsedCheckpoint, L"Used this run: checkpoint load");
    add(CheatBit::UsedCubes, L"Used this run: cube modifications");
    if (active & CheatMask(CheatBit::FlySpeed))
        names.emplace_back(L"Fly speed: " + std::to_wstring(static_cast<int>(g_fly_speed)));
    add(CheatBit::FasterRestarts, L"Fast restart animation");
    add(CheatBit::LastButtonMomentum, L"Keep last-button momentum");
    return names;
}
} // namespace

bool GameplayTuningActive() { return SettingCheatMask() != 0; }
bool CheatsCurrentlyActive() { return ActiveCheatMask() != 0; }
std::vector<std::wstring> ActiveCheatLabels() {
    auto labels = LabelsForMask(ActiveCheatMask() & ~CheatMask(CheatBit::WorldObjectRemoval));
    constexpr const wchar_t* kWorldLabels[] = {
        L"Tubes removed", L"Buttons removed", L"Jump pads removed",
        L"Cubes removed", L"Moving platforms removed", L"Tiles removed"
    };
    for (unsigned i = 0; i < static_cast<unsigned>(WorldObjectKind::Count); ++i)
        if (IsWorldObjectRemoved(static_cast<WorldObjectKind>(i)))
            labels.emplace_back(kWorldLabels[i]);
    return labels;
}
std::vector<std::wstring> ActiveCustomisationLabels() {
    // Keep the original 64-bit registry as the single source of PB eligibility.
    // Newer visual options are display-only: no new CheatBit values, object
    // scans or gameplay-taint paths are needed just to show them on the HUD.
    // WaterColour combines two independent settings in the mask, so replace
    // its generic label with one entry per active surface/underwater setting.
    auto names = LabelsForMask(ActiveCustomisationMask() &
                               ~CheatMask(CheatBit::WaterColour));
    const auto add = [&](bool enabled, const wchar_t* label) {
        if (enabled) names.emplace_back(label);
    };
    const auto add_intensity = [&](bool enabled, const wchar_t* label, int percent) {
        if (!enabled) return;
        if (percent == 100) names.emplace_back(label);
        else names.emplace_back(std::wstring(label) + L": " +
                                std::to_wstring(percent) + L"% intensity");
    };

    add(g_crosshair.style != 0, L"Custom crosshair");
    add_intensity(IsWaterCustomColourEnabled(), L"Water surface colour",
                  GetWaterColourIntensityPercent());
    add_intensity(IsUnderwaterCustomColourEnabled(), L"Underwater colour",
                  GetUnderwaterColourIntensityPercent());
    add(IsUnderwaterTintRemoved(), L"Underwater tint removed");
    add(AreWaterSplashesRemoved(), L"Water splashes removed");

    add(IsSkyColourEnabled(SkyRegion::Zenith), L"Sky zenith colour");
    add(IsSkyColourEnabled(SkyRegion::Horizon), L"Sky horizon colour");
    add(IsSkyColourEnabled(SkyRegion::Sun), L"Sky sun colour");
    add(IsSkyColourEnabled(SkyRegion::Clouds), L"Sky cloud colour");
    add(!IsSkyDiscVisible(), L"Moon/Sun disc hidden");
    add(!IsSkyEffectVisible(SkyEffect::Clouds), L"Clouds hidden");
    add(!IsSkyEffectVisible(SkyEffect::Stars), L"Stars hidden");
    add(IsFogColourEnabled(FogRegion::General), L"Fog haze colour");
    add(IsFogColourEnabled(FogRegion::Directional), L"Directional fog colour");
    add(!IsFogVisible(FogRegion::General), L"Fog hidden");
    add(!IsFogVisible(FogRegion::Directional), L"Directional fog hidden");
    add(!AreShadowsVisible(), L"Directional shadows hidden");
    add(IsShadowAmbientColourEnabled(), L"Shadow ambient colour");

    // Time-of-day is already in the visual mask. Show its independent
    // lock/speed modifiers only while custom time is actually enabled.
    if (IsCustomTimeOfDayEnabled()) {
        add(IsCustomTimeLocked(), L"Time of day locked");
        if (GetCustomTimeSpeedPercent() != 100)
            names.emplace_back(L"Time of day speed: " +
                               std::to_wstring(GetCustomTimeSpeedPercent()) + L"%");
    }
    return names;
}

void MarkCheatAction(CheatAction action) {
    const auto id = static_cast<unsigned>(action);
    if (id > static_cast<unsigned>(CheatAction::Cubes)) return;
    const auto bit = action == CheatAction::Cubes ? CheatBit::UsedCubes
        : static_cast<CheatBit>(static_cast<unsigned>(CheatBit::UsedPosition) + id);
    const uint64_t mask = CheatMask(bit);
    if (s_used_actions & mask) return;  // Do not spam repeated actions.
    s_used_actions |= mask;
    g_recording_tainted = true;
    if (!g_pb_cheats_allowed_for_run) g_run_pb_eligible = false;
    const auto labels = LabelsForMask(mask);
    if (!labels.empty()) {
        g_cheat_popup_text = L"CHEAT USED: " + labels.front();
        g_cheat_popup_expires = GetTickCount64() + 3200;
    }
    s_last_cheat_mask |= mask;
}

// Automatic native-gate recovery is different from an explicit button edit.
// It still counts as run assistance, but never labels a passive checker or
// a healthy button as "used this run: button modifications".
void MarkAutomaticButtonRepair(int one_based_button) {
    const uint64_t mask = CheatMask(CheatBit::ButtonRepair);
    if (s_used_actions & mask) return;
    s_used_actions |= mask;
    g_recording_tainted = true;
    if (!g_pb_cheats_allowed_for_run) g_run_pb_eligible = false;
    g_cheat_popup_text = L"CHEAT USED: Automatic button repair (button " +
        std::to_wstring(one_based_button) + L")";
    g_cheat_popup_expires = GetTickCount64() + 3200;
    s_last_cheat_mask |= mask;
}

void UpdateCheatNotifications() {
    const uint64_t current = ActiveCheatMask();
    const uint64_t added = current & ~s_last_cheat_mask;
    s_last_cheat_mask = current;
    if (!added) return;
    // Only gameplay-changing bits reach this notification path. Visual
    // customisations are masked out, so they cannot taint a valid run.
    g_recording_tainted = true;
    if (!g_pb_cheats_allowed_for_run) g_run_pb_eligible = false;
    const auto labels = LabelsForMask(added);
    if (!labels.empty()) {
        g_cheat_popup_text = L"CHEAT ACTIVE: " + labels.front();
        if (labels.size() > 1)
            g_cheat_popup_text += L" (+" + std::to_wstring(labels.size() - 1) + L" more)";
        g_cheat_popup_expires = GetTickCount64() + 3200;
    }
}

bool RecordsAllowedNow() {
    return !g_preloaded_restart_active && !g_progress_restart_active &&
           !g_normal_fast_restart_assisted &&
           !g_recording_tainted && !CheatsCurrentlyActive();
}

namespace {
bool NoArtificialRunProgress() {
    return !g_preloaded_restart_active && !g_progress_restart_active &&
           !g_normal_fast_restart_assisted;
}
}

bool RecordsAllowedForPB() {
    return NoArtificialRunProgress() &&
           ((g_pb_cheats_allowed_for_run && CheatRecordsPB(g_cheat_record_mode)) ||
            RecordsAllowedNow());
}

bool RecordsAllowedForSegments() {
    return NoArtificialRunProgress() &&
           (CheatRecordsSegments(g_cheat_record_mode) || RecordsAllowedNow());
}

void ClearTaintOnFreshRun(int buttons) {
    // A checkpoint load triggers a real RestartGame and visits button zero.
    // Do not mistake that internal restart for a new unassisted player run.
    if (buttons == 0 && !g_practice_active &&
        !g_preloaded_restart_active && !g_progress_restart_active) {
        // A *new* run has no historical one-shot actions, even when a
        // persistent setting keeps that new run assisted. Checkpoint loads
        // and artificial progress never clear this history.
        s_last_cheat_mask &= ~s_used_actions;
        s_used_actions = 0;
        g_cheat_popup_expires = 0;
        if (!g_normal_fast_restart_assisted &&
            !GameplayTuningActive() && !g_flying && !g_noclip &&
            !g_restore_flying_pending && !g_restore_noclip_pending)
            g_recording_tainted = false;
    }
}
std::wstring ButtonLabel(int internal_button) {
    if (internal_button >= 1 && internal_button <= kRefunctButtonCount)
        return kButtonNames[static_cast<size_t>(internal_button - 1)];
    // Used only for diagnostics, not for choosing a checkpoint.
    return std::to_wstring(internal_button);
}
// Shared low-level writer: writes a complete snapshot atomically, creates a
// backup and flushes it to disk. May run on the dedicated gold-save worker.
// No game-thread globals are read from this function.
bool WritePersonalBestSnapshot(const std::wstring& path, const PersonalBest& best,
                               bool replace_existing = true) {
    if (path.empty() || (!replace_existing && IsRegularFile(path))) return false;
    const auto profile = ghost_data_paths::ProfileName(path);
    if (profile.empty() || !EnsureProfileDirectories(profile)) return false;
    std::wstring content =
        L"; Refunct Utility recorded splits (furthest run until first completed PB)\r\n"
        L"; Edit values manually. Use mm:ss.mmm or seconds, leave blank if unknown.\r\n"
        L"; Button times are elapsed since RUN START, not individual segments.\r\n"
        L"; Before the first PB, the furthest eligible run supplies the splits.\r\n"
        L"; After the first completed PB, incomplete runs cannot replace it.\r\n"
        L"; Each file is an independent PB profile (Any%, 100%, etc.).\r\n"
        L"; Optional RunRules minimums validate category progress at finish.\r\n"
        L"; SectionSplits come from the same recorded run (partial or PB).\r\n"
        L"; BestSegments are independent individual records, NOT PB sections.\r\n"
        L"; Fastest segments may come from an eligible Run or Section Practice.\r\n"
        L"\r\n[PersonalBest]\r\nRunTime=";
    if (best.total_seconds >= 0.0) content += FormatSplitTime(best.total_seconds);
    // Explicit metadata differentiates automatically captured partial splits
    // from manual split entries with no RunTime. Both use this SAME INI file.
    const int incomplete_button = best.total_seconds < 0.0 ?
        std::clamp(best.incomplete_reached, 0, kRefunctButtonCount - 1) : 0;
    content += L"\r\nFurthestButton=" + std::to_wstring(incomplete_button);
    content += L"\r\n\r\n[RunRules]\r\n";
    content += L"MinimumButtons=" + std::to_wstring(best.minimum_buttons) + L"\r\n";
    content += L"MinimumCubes=" + std::to_wstring(best.minimum_cubes) + L"\r\n";
    content += L"MinimumPlatforms=" + std::to_wstring(best.minimum_platforms) + L"\r\n";
    content += L"\r\n[ButtonSplits]\r\n";
    for (int button = 1; button <= kRefunctButtonCount; ++button) {
        content += PBKey(button) + L"=";
        const double time = best.button_seconds[button - 1];
        if (time >= 0.0) content += FormatSplitTime(time);
        content += L"\r\n";
    }
    content += L"\r\n[SectionSplits]\r\n";
    for (int section = 1; section <= static_cast<int>(kSectionEndButtons.size()); ++section) {
        content += SectionPBKey(section) + L"=";
        const double seconds = best.section_seconds[static_cast<size_t>(section - 1)];
        if (std::isfinite(seconds) && seconds >= 0.0) content += FormatSplitTime(seconds);
        content += L"\r\n";
    }
    content += L"\r\n[BestSegments]\r\n";
    for (int button = 1; button <= kRefunctButtonCount; ++button) {
        content += PBKey(button) + L"=";
        const double time = best.best_segment_seconds[button - 1];
        if (time >= 0.0) content += FormatSplitTime(time);
        content += L"\r\n";
    }
    content += L"\r\n[BestSegmentSources]\r\n";
    for (int button = 1; button <= kRefunctButtonCount; ++button) {
        content += PBKey(button) + L"=";
        const auto source = best.best_segment_sources[button - 1];
        if (best.best_segment_seconds[button - 1] >= 0.0) {
            if (source == SegmentSource::Run) content += L"Run";
            else if (source == SegmentSource::Practice) content += L"Practice";
        }
        content += L"\r\n";
    }
    // All characters above are ASCII. The conversion also rejects accidental
    // non-ASCII values instead of silently writing a corrupt INI.
    std::string ascii;
    ascii.reserve(content.size());
    for (wchar_t ch : content) {
        if (ch > 127) return false;
        ascii.push_back(static_cast<char>(ch));
    }
    const std::wstring temp = path + L".tmp";
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, ascii.data(), static_cast<DWORD>(ascii.size()),
                              &written, nullptr) && written == ascii.size() &&
                    FlushFileBuffers(file);
    CloseHandle(file);
    // Preserve the previous user-editable reference before replacing it.
    if (ok && replace_existing && IsRegularFile(path))
        CopyFileW(path.c_str(), (path + L".bak").c_str(), FALSE);
    const DWORD flags = MOVEFILE_WRITE_THROUGH |
                        (replace_existing ? MOVEFILE_REPLACE_EXISTING : 0);
    if (!ok || !MoveFileExW(temp.c_str(), path.c_str(), flags)) {
        DeleteFileW(temp.c_str());
        return false;
    }
    return true;
}

// A new fastest segment used to run the expensive atomic file replace,
// FlushFileBuffers, CopyFile and MOVEFILE_WRITE_THROUGH in the character tick.
// The worker runs these operations off the game thread. Queueing overwrites
// only a PENDING snapshot, never a snapshot currently being written, so a
// rapid sequence of golds is coalesced without losing the latest record.
namespace {
struct GoldFileWriter {
    SRWLOCK lock = SRWLOCK_INIT;
    HANDLE wake = nullptr;  // automatic reset
    HANDLE idle = nullptr;  // manual reset, signalled when all writes finish
    bool pending = false;
    std::wstring path;
    PersonalBest best{};
    std::atomic_bool error{false};
};
// Lifetime is intentionally process-long: a shipping ASI is never hot-unloaded.
// No waiting for a worker inside DllMain/loader lock.
GoldFileWriter* s_gold_writer = nullptr;

DWORD WINAPI GoldFileWriterMain(LPVOID parameter) {
    auto* writer = static_cast<GoldFileWriter*>(parameter);
    for (;;) {
        if (WaitForSingleObject(writer->wake, INFINITE) != WAIT_OBJECT_0)
            return 1;
        for (;;) {
            AcquireSRWLockExclusive(&writer->lock);
            if (!writer->pending) {
                SetEvent(writer->idle);
                ReleaseSRWLockExclusive(&writer->lock);
                break;
            }
            std::wstring path = writer->path;
            PersonalBest snapshot = writer->best;
            writer->pending = false;
            ReleaseSRWLockExclusive(&writer->lock);
            bool saved = false;
            // Short-lived file locks (e.g. antivirus or a text editor) should
            // not immediately discard a legitimately measured segment.
            for (int attempt = 0; attempt < 3 && !saved; ++attempt) {
                if (attempt != 0) Sleep(100); // Worker thread only.
                saved = WritePersonalBestSnapshot(path, snapshot);
            }
            if (!saved) {
                writer->error.store(true, std::memory_order_release);
                Log(L"ERROR: background split-record file save failed; see profile directory");
            }
        }
    }
}

void StartGoldFileWriter() {
    if (s_gold_writer) return;
    auto* writer = new (std::nothrow) GoldFileWriter;
    if (!writer) return;  // Keep the synchronous fallback on allocation failure.
    writer->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    writer->idle = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    if (!writer->wake || !writer->idle) {
        if (writer->wake) CloseHandle(writer->wake);
        if (writer->idle) CloseHandle(writer->idle);
        delete writer;
        return;
    }
    HANDLE thread = CreateThread(nullptr, 0, GoldFileWriterMain, writer, 0, nullptr);
    if (!thread) {
        CloseHandle(writer->wake);
        CloseHandle(writer->idle);
        delete writer;
        return;
    }
    CloseHandle(thread);  // The worker/event lifetime is the game's lifetime.
    s_gold_writer = writer;
}

bool DrainGoldFileWriter() {
    if (!s_gold_writer) return true;
    // Never hang the game thread forever if the background worker crashes or
    // becomes stuck. On timeout, REFUSE the dependent file/profile operation:
    // allowing it to continue risks an older queued write overwriting it.
    constexpr DWORD kDrainTimeoutMs = 10000;
    const DWORD result = WaitForSingleObject(s_gold_writer->idle, kDrainTimeoutMs);
    if (result == WAIT_OBJECT_0) return true;
    Log(result == WAIT_TIMEOUT
        ? L"ERROR: fastest-segment writer did not finish within 10 seconds; profile operation cancelled"
        : L"ERROR: fastest-segment writer wait failed; profile operation cancelled");
    return false;
}

bool QueueGoldFileWrite(const PersonalBest& best) {
    if (!s_gold_writer || g_pb_path.empty()) return false;
    auto* writer = s_gold_writer;
    AcquireSRWLockExclusive(&writer->lock);
    writer->best = best;
    writer->path = g_pb_path; // Snapshot path before a profile change/rename.
    writer->pending = true;
    ResetEvent(writer->idle);
    ReleaseSRWLockExclusive(&writer->lock);
    // The worker always takes the latest queued complete PB snapshot.
    if (SetEvent(writer->wake)) return true;
    // An event failure must not leave idle unsignalled forever.
    AcquireSRWLockExclusive(&writer->lock);
    writer->pending = false;
    SetEvent(writer->idle);
    ReleaseSRWLockExclusive(&writer->lock);
    return false;
}

void PollGoldFileWriteError() {
    if (s_gold_writer &&
        s_gold_writer->error.exchange(false, std::memory_order_acq_rel)) {
        Notify(L"Split records could not be saved; see RefunctUtility.log");
    }
}
} // namespace

// All explicit PB/profile writes remain synchronous to preserve their normal
// return status. Drain older gold jobs first so they cannot overwrite a newer
// completed-run PB or a manually edited profile.
bool SavePersonalBestFile(const PersonalBest& best) {
    return DrainGoldFileWriter() && WritePersonalBestSnapshot(g_pb_path, best);
}

// Discover profiles by their actual filenames, including files renamed in Explorer.
void ScanProfiles(bool create_defaults) {
    g_profiles.clear();
    if (g_storage_dir.empty()) return;
    const auto add = [&](const std::wstring& name) {
        if (!ValidProfileName(name) || g_profiles.size() >= 64) return;
        const bool present = std::any_of(g_profiles.begin(), g_profiles.end(),
            [&](const std::wstring& entry) { return _wcsicmp(entry.c_str(), name.c_str()) == 0; });
        if (!present) g_profiles.push_back(name);
    };
    // Read actual directory names, so Explorer renames can be discovered.
    const auto profiles = ghost_data_paths::ProfilesRoot(g_storage_dir);
    WIN32_FIND_DATAW item{};
    HANDLE search = FindFirstFileW((profiles + L"\\*").c_str(), &item);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (!(item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            const std::wstring name(item.cFileName);
            if (IsRegularFile(ghost_data_paths::Records(g_storage_dir, name))) add(name);
        } while (FindNextFileW(search, &item));
        FindClose(search);
    }
    // A read-only fallback for profiles whose migration was blocked.
    if (!g_splits_dir.empty()) {
        search = FindFirstFileW((g_splits_dir + L"\\*.ini").c_str(), &item);
        if (search != INVALID_HANDLE_VALUE) {
            do {
                if (item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                std::wstring name(item.cFileName);
                if (name.size() <= 4 || _wcsicmp(name.c_str() + name.size() - 4, L".ini")) continue;
                name.resize(name.size() - 4);
                add(name);
            } while (FindNextFileW(search, &item));
            FindClose(search);
        }
    }
    if (create_defaults) {
        const std::wstring old = g_pb_path;
        for (const wchar_t* name : {L"Any%", L"100%"}) {
            if (std::any_of(g_profiles.begin(), g_profiles.end(),
                [&](const auto& entry) { return _wcsicmp(entry.c_str(), name) == 0; })) continue;
            if (!EnsureProfileDirectories(name)) {
                Log(L"ERROR: unable to create default profile directories");
                continue;
            }
            g_pb_path = ProfilePath(name);
            if (!SavePersonalBestFile(PersonalBest{}))
                Log(L"ERROR: unable to create default split profile");
            else g_profiles.emplace_back(name);
        }
        g_pb_path = old;
    }
    std::sort(g_profiles.begin(), g_profiles.end(), [](const auto& a, const auto& b) {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    });
}

// Create one numbered, non-destructive snapshot only AFTER a new PB has
// committed to the existing profile. The source category remains selected;
// never call SelectProfile here (it invalidates the just-completed run).
namespace {
void SaveAutomaticPBProfile(const PersonalBest& new_pb) {
    if (!g_auto_create_pb_profile || g_pb_path.empty() ||
        !ValidProfileName(g_active_profile)) return;

    // This runs on a completed PB only, not in the per-frame update path.
    // Refresh after external file edits so a deleted/added profile is noticed.
    ScanProfiles(false);
    if (g_profiles.size() >= 64) {
        Log(L"WARNING: auto PB profile skipped (64 profile limit); main PB already saved");
        return;
    }

    PersonalBest snapshot = new_pb; // Full PB, timestamps, sections and rules.
    if (!g_auto_pb_copy_best_segments) {
        snapshot.best_segment_seconds.fill(-1.0);
        snapshot.best_segment_sources.fill(SegmentSource::Unknown);
    }

    // Keep names valid under the existing 32-character filename limit.
    // Explicit 3-digit numbering provides chronological ordering in Explorer.
    std::wstring prefix = g_active_profile.substr(0, 25);
    while (!prefix.empty() && prefix.back() == L' ') prefix.pop_back();
    if (prefix.empty()) return;
    for (int number = 1; number <= 999; ++number) {
        std::wstring digits = std::to_wstring(number);
        digits.insert(0, 3 - digits.size(), L'0');
        const std::wstring profile = prefix + L"_PB_" + digits;
        if (!ValidProfileName(profile)) continue;
        const std::wstring destination = ProfilePath(profile);
        if (destination.empty() || IsRegularFile(destination) ||
            GetFileAttributesW(ghost_data_paths::ProfileDir(g_storage_dir, profile).c_str()) != INVALID_FILE_ATTRIBUTES)
            continue;
        if (!EnsureProfileDirectories(profile)) {
            Log(L"WARNING: auto PB profile directory could not be created");
            return;
        }
        // Do not replace a file that another program created after our check.
        if (!WritePersonalBestSnapshot(destination, snapshot, false)) {
            Log(L"WARNING: auto PB profile file could not be created; main PB remains saved");
            return;
        }
        // Ghost recording is committed first. Each PB snapshot gets its own
        // matching replay when the source ghost was successfully recorded.
        std::wstring source_ghost = ghost_data_paths::PB(g_storage_dir, g_pb_path);
        const std::wstring target_ghost = ghost_data_paths::PB(g_storage_dir, destination);
        if (!IsRegularFile(source_ghost)) {
            const auto old = ghost_data_paths::LegacyPB(g_storage_dir, g_pb_path);
            const auto inline_old = ghost_data_paths::LegacyInline(g_storage_dir, g_pb_path, L".ghost");
            if (IsRegularFile(old)) source_ghost = old;
            else if (IsRegularFile(inline_old)) source_ghost = inline_old;
        }
        if (PBGhostSavedForLatestPB() && IsRegularFile(source_ghost) &&
            !CopyFileW(source_ghost.c_str(), target_ghost.c_str(), TRUE)) {
            Log(L"WARNING: PB snapshot created without a ghost replay copy");
        }
        if (g_auto_pb_copy_best_segments)
            CopyFastestSegmentGhosts(destination);
        ScanProfiles(false);
        Log((L"Automatic PB snapshot profile created: " + profile +
             (g_auto_pb_copy_best_segments ? L" (fastest segments copied)" :
                                              L" (fastest segments omitted)")).c_str());
        Notify(L"PB snapshot saved: " + profile);
        return;
    }
    Log(L"WARNING: auto PB profile skipped (no free numbered name); main PB saved");
}
} // namespace

bool SelectProfile(const std::wstring& name, bool reset_run) {
    if (!DrainGoldFileWriter() || !DrainFastestSegmentGhostWrites()) return false;
    const auto it = std::find_if(g_profiles.begin(), g_profiles.end(), [&](const auto& value) {
        return _wcsicmp(value.c_str(), name.c_str()) == 0;
    });
    if (it == g_profiles.end()) return false;
    const bool changed = _wcsicmp(g_active_profile.c_str(), it->c_str()) != 0;
    g_active_profile = *it;
    g_pb_path = ProfilePath(g_active_profile);
    // Mid-run profile switches cannot rewrite the run's earlier button history.
    if (changed && reset_run) {
        g_practice_gold_anchor_button = 0;
        g_practice_gold_anchor_seconds = -1.0;
        g_run_pb_eligible = false;
        g_run_started_at_zero = false;
        g_button_state_initialized = false;
        ResetButtonHistory();
        g_run_finish_processed = false;
    }
    ReloadComparisonProfile();
    SaveSettings();
    return true;
}

// Keep comparisons separate from g_pb and g_pb_path. A missing or deleted
// reference profile produces N/A, never writes or creates reference records.
bool ReloadComparisonProfile() {
    g_compare_reference_loaded = false;
    g_compare_pb = PersonalBest{};
    const auto available = [&](const std::wstring& value) {
        if (!ValidProfileName(value) ||
            _wcsicmp(value.c_str(), g_active_profile.c_str()) == 0) return false;
        return std::any_of(g_profiles.begin(), g_profiles.end(), [&](const auto& entry) {
            return _wcsicmp(entry.c_str(), value.c_str()) == 0;
        });
    };
    if (!available(g_compare_profile)) {
        g_compare_profile.clear();
        for (const auto& profile : g_profiles) {
            if (available(profile)) { g_compare_profile = profile; break; }
        }
    }
    if (g_compare_profile.empty()) return false;
    const std::wstring path = ProfilePath(g_compare_profile);
    if (path.empty() || !IsRegularFile(path)) return false;

    const auto read_time = [&](const wchar_t* section, int button, double& output) {
        wchar_t buffer[96]{};
        if (!GetPrivateProfileStringW(section, PBKey(button).c_str(), L"", buffer,
                static_cast<DWORD>(std::size(buffer)), path.c_str())) return;
        if (!ParsePBTime(buffer, output)) output = -1.0;
    };
    for (int button = 1; button <= kRefunctButtonCount; ++button) {
        read_time(L"ButtonSplits", button, g_compare_pb.button_seconds[button - 1]);
        read_time(L"BestSegments", button, g_compare_pb.best_segment_seconds[button - 1]);
    }
    for (int section = 1; section <= static_cast<int>(kSectionEndButtons.size()); ++section) {
        wchar_t value[96]{};
        if (GetPrivateProfileStringW(L"SectionSplits", SectionPBKey(section).c_str(),
                L"", value, static_cast<DWORD>(std::size(value)), path.c_str()) &&
            !ParsePBTime(value, g_compare_pb.section_seconds[static_cast<size_t>(section - 1)]))
            g_compare_pb.section_seconds[static_cast<size_t>(section - 1)] = -1.0;
    }
    // A manually entered PB may only have RunTime and no final button split.
    wchar_t total_time[96]{};
    if (GetPrivateProfileStringW(L"PersonalBest", L"RunTime", L"", total_time,
            static_cast<DWORD>(std::size(total_time)), path.c_str())) {
        if (!ParsePBTime(total_time, g_compare_pb.total_seconds))
            g_compare_pb.total_seconds = -1.0;
    }
    g_compare_reference_loaded = true;
    return true;
}

bool CycleComparisonProfile(int direction) {
    if (g_profiles.empty()) return false;
    const auto found = std::find_if(g_profiles.begin(), g_profiles.end(), [&](const auto& name) {
        return _wcsicmp(name.c_str(), g_compare_profile.c_str()) == 0;
    });
    const int count = static_cast<int>(g_profiles.size());
    const int index = found == g_profiles.end()
        ? (direction < 0 ? 0 : -1)
        : static_cast<int>(found - g_profiles.begin());
    for (int step = 1; step <= count; ++step) {
        const int candidate = (index + count + step * (direction < 0 ? -1 : 1)) % count;
        if (_wcsicmp(g_profiles[candidate].c_str(), g_active_profile.c_str()) == 0) continue;
        g_compare_profile = g_profiles[candidate];
        ReloadComparisonProfile();
        SaveSettings();
        return true;
    }
    return false;
}

double ComparisonReferenceSeconds(int button) {
    if (!g_compare_reference_loaded ||
        _wcsicmp(g_compare_profile.c_str(), g_active_profile.c_str()) == 0) return -1.0;
    const double reference = ProfileReferenceTimeAtButton(g_compare_pb.button_seconds,
        g_compare_pb.best_segment_seconds, button, g_compare_mode);
    if (reference >= 0.0) return reference;
    // Only at the finish: total PB time is not a substitute for missing splits.
    if (button == kRefunctButtonCount &&
        g_compare_mode == ProfileCompareMode::PersonalBest &&
        std::isfinite(g_compare_pb.total_seconds) && g_compare_pb.total_seconds >= 0.0)
        return g_compare_pb.total_seconds;
    return -1.0;
}

void LoadPersonalBest() {
    // Prestart the I/O worker while loading the profile, not on a button press.
    StartGoldFileWriter();
    if (!DrainGoldFileWriter()) {
        Log(L"ERROR: pending fastest-segment save could not be completed");
        return;
    }
    g_pb = PersonalBest{};
    if (g_pb_path.empty()) { LoadPBGhostForActiveProfile(); return; }
    if (GetFileAttributesW(g_pb_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (!SavePersonalBestFile(g_pb)) {
            Log(L"ERROR: could not create selected split profile in Refunct folder");
        } else {
            Log(L"Created empty split profile at mod startup");
        }
        LoadPBGhostForActiveProfile();
        return;
    }
    auto read_time = [](const wchar_t* section, const std::wstring& key,
                        double& output) {
        wchar_t raw[96]{};
        if (GetPrivateProfileStringW(section, key.c_str(), L"", raw, 96,
                                     g_pb_path.c_str()) == 0) return;
        if (!ParsePBTime(raw, output)) {
            Log(L"WARNING: invalid time in selected split profile; ignoring value");
            output = -1.0;
        }
    };
    read_time(L"PersonalBest", L"RunTime", g_pb.total_seconds);
    g_pb.minimum_buttons = std::clamp<int>(GetPrivateProfileIntW(L"RunRules", L"MinimumButtons", 0, g_pb_path.c_str()), 0, 37);
    g_pb.minimum_cubes = std::clamp<int>(GetPrivateProfileIntW(L"RunRules", L"MinimumCubes", 0, g_pb_path.c_str()), 0, 10000);
    g_pb.minimum_platforms = std::clamp<int>(GetPrivateProfileIntW(L"RunRules", L"MinimumPlatforms", 0, g_pb_path.c_str()), 0, 10000);
    for (int i = 1; i <= static_cast<int>(kSectionEndButtons.size()); ++i)
        read_time(L"SectionSplits", SectionPBKey(i), g_pb.section_seconds[i - 1]);
    for (int i = 1; i <= kRefunctButtonCount; ++i) {
        read_time(L"ButtonSplits", PBKey(i), g_pb.button_seconds[i - 1]);
        read_time(L"BestSegments", PBKey(i), g_pb.best_segment_seconds[i - 1]);
        if (g_pb.best_segment_seconds[i - 1] >= 0.0) {
            wchar_t source[32]{};
            GetPrivateProfileStringW(L"BestSegmentSources", PBKey(i).c_str(), L"",
                                     source, static_cast<DWORD>(std::size(source)),
                                     g_pb_path.c_str());
            if (_wcsicmp(source, L"Run") == 0)
                g_pb.best_segment_sources[i - 1] = SegmentSource::Run;
            else if (_wcsicmp(source, L"Practice") == 0)
                g_pb.best_segment_sources[i - 1] = SegmentSource::Practice;
        }
    }
    // A legacy profile with manually entered ButtonSplits and no RunTime
    // has no FurthestButton marker and must not be overwritten automatically.
    // Reject corrupt/inconsistent partial metadata rather than guessing.
    const int reached = GetPrivateProfileIntW(
        L"PersonalBest", L"FurthestButton", 0, g_pb_path.c_str());
    if (g_pb.total_seconds < 0.0 && reached > 0 &&
        reached < kRefunctButtonCount) {
        double prior = 0.0;
        bool valid = true;
        for (int i = 0; i < reached; ++i) {
            const double seconds = g_pb.button_seconds[static_cast<std::size_t>(i)];
            if (!std::isfinite(seconds) || seconds <= prior || seconds > 86400.0) {
                valid = false;
                break;
            }
            prior = seconds;
        }
        if (valid) g_pb.incomplete_reached = reached;
        else Log(L"WARNING: incomplete split metadata invalid; preserving profile splits as manual");
    }
    LoadPBGhostForActiveProfile();
    Log((L"Loaded split profile: " + g_active_profile).c_str());
}

// Complete Sum of Best requires a record for every physical button. A partial
// sum is still useful but is explicitly labelled partial in the menu.
SumOfBest CalculateSumOfBest() {
    SumOfBest total{};
    for (double time : g_pb.best_segment_seconds) {
        // A zero-duration best record is not a valid fastest segment. Treat
        // it as missing, consistent with cumulative best-segment references.
        if (std::isfinite(time) && time > 0.0 && time <= 86400.0) {
            total.seconds += time;
            ++total.recorded;
        }
    }
    return total;
}

GoldUpdate UpdateBestSegment(int button, double segment, SegmentSource source, double observed_elapsed) {
    GoldUpdate result{};
    // Keep existing fastest segments available for comparisons even when
    // automatic fastest-segment saving is disabled.
    if (button < 1 || button > kRefunctButtonCount) return result;
    const size_t index = static_cast<size_t>(button - 1);
    result.current_best = g_pb.best_segment_seconds[index];
    result.previous_best = result.current_best;
    // Independent from PB saving; assisted segments require explicit
    // Save With Cheats permission (checked by RecordsAllowedForSegments).
    if (!g_save_fastest_segments || !RecordsAllowedForSegments()) return result;
    // Enforce a small positive floor to avoid a false 0 ms gold from an
    // instantaneous native activation or the practice loader itself.
    if (source == SegmentSource::Unknown || !std::isfinite(segment) ||
        segment < 0.050 || segment > 24.0 * 3600.0) return result;
    constexpr double kGoldEpsilon = 0.0005;
    if (result.current_best >= 0.0 &&
        segment >= result.current_best - kGoldEpsilon) return result;

    PersonalBest next = g_pb;
    next.best_segment_seconds[index] = segment;
    next.best_segment_sources[index] = source;
    if (!QueueGoldFileWrite(next) && !SavePersonalBestFile(next)) {
        Notify(L"New fastest segment, but file save failed - see log");
        Log((L"ERROR: failed to save fastest segment for button " +
             ButtonLabel(button)).c_str());
        return result;
    }
    g_pb = next;
    result.current_best = segment;
    result.new_gold = true;
    // Record a matching movement path for each new gold. This also works when
    // the run ends at this button and never produces a full-run PB. The ghost
    // module rejects practice/synthetic paths (times alone are insufficient).
    QueueFastestSegmentGhost(button, observed_elapsed - segment,
                            observed_elapsed, source == SegmentSource::Run);
    // Keep the game thread free of synchronous logger/file writes when a gold
    // is earned. The worker logs failures; the HUD already shows new golds.
    return result;
}

// Preserve one incomplete reference inside the active profile's records.ini.
// A farther button wins; at the same button, the faster cumulative time wins.
// A completed PB and pre-existing manual splits without a RunTime are protected.
// The existing background record writer performs the disk I/O off the tick.
void RecordFurthestIncompleteSplits(int button, double elapsed_seconds) {
    if (button < 1 || button >= kRefunctButtonCount || g_pb_path.empty() ||
        !g_save_personal_bests || !g_pb_recording_enabled_for_run ||
        !g_run_started_at_zero || !g_run_pb_eligible ||
        !RecordsAllowedForPB() || g_timer_complete || IsGhostReplayRequested() ||
        !std::isfinite(elapsed_seconds) || elapsed_seconds <= 0.0 ||
        elapsed_seconds > 86400.0 || g_pb.total_seconds >= 0.0) return;

    // Existing profiles with hand-entered splits and a blank RunTime are
    // intentional user data. Only an explicit auto-partial marker permits
    // replacing those splits.
    if (g_pb.incomplete_reached == 0 &&
        std::any_of(g_pb.button_seconds.begin(), g_pb.button_seconds.end(),
                    [](double seconds) { return seconds >= 0.0; })) return;

    const int saved_button = g_pb.incomplete_reached;
    if (button < saved_button) return;
    if (button == saved_button) {
        const double prior = g_pb.button_seconds[static_cast<std::size_t>(button - 1)];
        if (std::isfinite(prior) && prior > 0.0 &&
            elapsed_seconds >= prior - 0.0005) return;
    }

    // Only a consecutive, measured button prefix is a legitimate run. A
    // skipped native event cannot become a fabricated split timestamp.
    if (g_button_splits.size() < static_cast<std::size_t>(button)) return;
    double previous = 0.0;
    PersonalBest next = g_pb;
    next.button_seconds.fill(-1.0);
    next.section_seconds.fill(-1.0);
    for (int i = 0; i < button; ++i) {
        const auto& split = g_button_splits[static_cast<std::size_t>(i)];
        if (split.button_number != i + 1 ||
            !std::isfinite(split.run_seconds) ||
            split.run_seconds <= previous || split.run_seconds > 86400.0) return;
        next.button_seconds[static_cast<std::size_t>(i)] = split.run_seconds;
        previous = split.run_seconds;
    }
    if (std::abs(previous - elapsed_seconds) > 0.001) return;
    for (std::size_t i = 0; i < kSectionEndButtons.size(); ++i) {
        if (kSectionEndButtons[i] > button) break;
        const SectionBoundary boundary = SectionEndingAtButton(kSectionEndButtons[i]);
        next.section_seconds[i] = SectionDurationFromPB(next.button_seconds, boundary);
    }
    next.incomplete_reached = button;
    next.total_seconds = -1.0;

    // Reuse the coalescing background writer, which also serialises gold saves.
    if (!QueueGoldFileWrite(next) && !SavePersonalBestFile(next)) {
        Log(L"ERROR: furthest incomplete split recording could not be saved");
        return;
    }
    g_pb = next;
}

void FinishPersonalBestIfNeeded() {
    if (!g_engine_timer_valid || !g_timer_complete || g_run_finish_processed) return;
    // The loader can activate Refunct's finish button before it has actually
    // finished setting up the checkpoint. Never finalize PB eligibility mid-
    // replay; wait until the state machine is ready or the run is restarted.
    if (g_practice_active && g_practice_phase != PracticePhase::Ready) return;

    void* instance = GetGameInstance();
    if (!instance) return;  // A transient invalid pointer must not discard the PB.
    const auto* state = reinterpret_cast<const LevelState*>(
        reinterpret_cast<const uint8_t*>(instance) + 0x9C);
    if (!IsReadable(state, sizeof(*state))) return;
    const int count = state->buttons;
    if (count < 1 || count > kRefunctButtonCount) {
        Log(L"WARNING: completed run has an unexpected button count; PB not saved");
        g_run_finish_processed = true;
        return;
    }

    g_run_finish_processed = true;
    // A PB is eligible only if saving stayed ON since the start of this run.
    // This does not affect fastest-segment recording or existing PB displays.
    if (!g_save_personal_bests || !g_pb_recording_enabled_for_run) {
        Log(L"INFO: automatic PB saving disabled for this run");
        return;
    }
    if (state->buttons < g_pb.minimum_buttons || state->cubes < g_pb.minimum_cubes ||
        state->platforms < g_pb.minimum_platforms) {
        Log(L"INFO: finish did not meet selected split profile RunRules; PB not saved");
        return;
    }
    // Ordinarily require a complete, eligible run observed from button zero.
    // Checkpoint practice deliberately restarts/refills part of the course,
    // so its earlier PB-eligible flag is false. Treat that as a cheat instead
    // of blocking it forever: the user must have enabled PB cheat recording
    // for this run and still have it enabled when finishing.
    const bool cheat_checkpoint_pb = g_practice_active &&
        g_practice_phase == PracticePhase::Ready &&
        g_practice_origin != PracticeOrigin::ProgressRestart &&
        g_pb_cheats_allowed_for_run && CheatRecordsPB(g_cheat_record_mode);
    if (!RecordsAllowedForPB() ||
        (!cheat_checkpoint_pb && (!g_run_started_at_zero || !g_run_pb_eligible))) {
        Log(L"INFO: completed run was not eligible for PB recording");
        return;
    }
    // Synthetic button replay must never itself produce a PB. Require an
    // actual later button activation after the restored checkpoint. This also
    // prevents loading button 37 or a completed save-state from immediately
    // overwriting a real PB with an artificial finish time.
    if (cheat_checkpoint_pb) {
        const int loaded_through = std::max(g_practice_target_button - 1,
                                            g_practice_animate_button);
        if (count <= loaded_through) {
            Log(L"INFO: finish occurred during checkpoint replay; PB not saved");
            return;
        }
    }
    constexpr double epsilon = 0.0005;
    if (g_pb.total_seconds >= 0.0 &&
        g_elapsed_seconds >= g_pb.total_seconds - epsilon) return;

    // Preserve manually entered split times when no manual RunTime exists.
    if (g_pb.total_seconds < 0.0 && g_pb.incomplete_reached == 0 &&
        std::any_of(g_pb.button_seconds.begin(), g_pb.button_seconds.end(),
                    [](double time) { return time >= 0.0; })) {
        Log(L"INFO: manual PB splits exist without RunTime; will not overwrite");
        return;
    }

    PersonalBest next = g_pb;  // Keep manually configured category RunRules.
    next.button_seconds.fill(-1.0);
    next.section_seconds.fill(-1.0);
    next.total_seconds = g_elapsed_seconds;
    next.incomplete_reached = 0;  // Completed PB supersedes the partial run.
    int split_count = 0;
    for (const auto& split : g_button_splits) {
        if (split.button_number >= 1 && split.button_number <= count &&
            std::isfinite(split.run_seconds) && split.run_seconds >= 0.0 &&
            split.run_seconds <= next.total_seconds) {
            const size_t index = static_cast<size_t>(split.button_number - 1);
            if (next.button_seconds[index] < 0.0) ++split_count;
            next.button_seconds[index] = split.run_seconds;
        }
    }
    if (split_count < count) {
        Log(L"WARNING: some button timestamps were missed; saving completed "
            L"run PB with those split entries left blank");
    }
    for (size_t i = 0; i < kSectionEndButtons.size(); ++i) {
        const auto boundary = SectionEndingAtButton(kSectionEndButtons[i]);
        next.section_seconds[i] = SectionDurationFromPB(next.button_seconds, boundary);
    }
    if (!SavePersonalBestFile(next)) {
        Log(L"ERROR: new personal best was NOT saved to selected split profile");
        g_pb_popup_text = L"NEW PB (file save failed)";
    } else {
        g_pb = next;
        CommitPBGhostOnNewPB(next.total_seconds);
        SaveAutomaticPBProfile(next);
        g_pb_popup_text = L"NEW PERSONAL BEST  " + FormatSplitTime(next.total_seconds);
        Log((L"New run PB saved to profile: " + g_active_profile).c_str());
    }
    g_pb_popup_expires = GetTickCount64() + 7500;
}

void ResetButtonHistory() {
    g_button_splits.clear();
    g_split_popup_expires = 0;
    ClearNativeButtonObservations();
}

bool RefreshProfiles() {
    ScanProfiles(true);
    if (g_profiles.empty()) {
        Notify(L"No split profiles found (check write permissions)");
        return false;
    }
    const auto current = std::find_if(g_profiles.begin(), g_profiles.end(), [&](const auto& name) {
        return _wcsicmp(name.c_str(), g_active_profile.c_str()) == 0;
    });
    const std::wstring selection = (current != g_profiles.end()) ? *current : g_profiles.front();
    const bool changed = _wcsicmp(g_active_profile.c_str(), selection.c_str()) != 0;
    if (!SelectProfile(selection, changed)) return false;
    LoadPersonalBest();
    ReloadComparisonProfile();
    if (changed) Notify(L"Previous profile missing; switched to " + selection);
    return true;
}

bool CreateProfile(const std::wstring& name) {
    if (!ValidProfileName(name) || IsRegularFile(ProfilePath(name)) ||
        GetFileAttributesW(ghost_data_paths::ProfileDir(g_storage_dir, name).c_str()) != INVALID_FILE_ATTRIBUTES) {
        Notify(L"Invalid or existing profile name");
        return false;
    }
    if (!EnsureProfileDirectories(name)) {
        Notify(L"Could not create profile folders");
        return false;
    }
    const std::wstring old_path = g_pb_path;
    g_pb_path = ProfilePath(name);
    const bool ok = SavePersonalBestFile(PersonalBest{});
    g_pb_path = old_path;
    if (!ok) {
        Notify(L"Could not create profile; see log");
        Log(L"ERROR: unable to create split profile file");
        return false;
    }
    ScanProfiles(false);
    SelectProfile(name, true);
    LoadPersonalBest();
    Notify(L"Profile created: " + name + L" (restart before timing PB)");
    return true;
}

bool RenameActiveProfile(const std::wstring& name) {
    if (!DrainGoldFileWriter() || !DrainFastestSegmentGhostWrites()) return false;
    if (!ValidProfileName(name)) { Notify(L"Invalid profile name"); return false; }
    if (_wcsicmp(name.c_str(), g_active_profile.c_str()) == 0) return true;
    const auto old_name = g_active_profile;
    const auto from = ProfilePath(old_name);
    const auto old_dir = ghost_data_paths::ProfileDir(g_storage_dir, old_name);
    const auto new_dir = ghost_data_paths::ProfileDir(g_storage_dir, name);
    const auto new_record = ghost_data_paths::Records(g_storage_dir, name);
    // Do not merge two folders. An incomplete migration still owns its name.
    if (GetFileAttributesW(new_dir.c_str()) != INVALID_FILE_ATTRIBUTES ||
        IsRegularFile(ghost_data_paths::LegacySplit(g_storage_dir, name))) {
        Notify(L"Profile name already exists");
        return false;
    }
    if (!EnsureProfileDirectories(old_name)) {
        Notify(L"Rename failed: profile folder inaccessible");
        return false;
    }
    const auto canonical_old = ghost_data_paths::Records(g_storage_dir, old_name);
    // An unresolved collision means two INIs claim one profile. Renaming only
    // one would appear to resurrect the old profile on the next scan.
    if (IsRegularFile(canonical_old) &&
        IsRegularFile(ghost_data_paths::LegacySplit(g_storage_dir, old_name))) {
        Notify(L"Rename blocked: duplicate old profile record; check log");
        Log(L"WARNING: old and new profile records conflict; rename refused without deleting either");
        return false;
    }
    // If an earlier locked migration kept the INI in Splits, finish moving it
    // into the old profile directory before renaming that entire directory.
    if (!IsRegularFile(canonical_old)) {
        if (!IsRegularFile(from) || !MoveFileW(from.c_str(), canonical_old.c_str())) {
            Notify(L"Rename failed; original records preserved");
            return false;
        }
        const auto old_backup = from + L".bak";
        if (IsRegularFile(old_backup))
            MoveFileW(old_backup.c_str(), (canonical_old + L".bak").c_str());
    }
    if (!MoveFileW(old_dir.c_str(), new_dir.c_str())) {
        Notify(L"Rename failed; see log");
        Log(L"ERROR: unable to rename entire profile directory");
        return false;
    }
    g_active_profile = name;
    g_pb_path = new_record;
    // Also pick up any unmigrated older ghost files left in the shared roots.
    RenamePBGhostProfile(from, new_record);
    ScanProfiles(false);
    ReloadComparisonProfile();
    SaveSettings();
    Notify(L"Profile renamed to " + name);
    return true;
}

void StartNameEdit(NameEditMode mode) {
    g_name_edit_mode = mode;
    g_name_buffer = mode == NameEditMode::Rename ? g_active_profile :
                    mode == NameEditMode::CustomRename ? ActiveCustomisationProfile() :
                    mode == NameEditMode::HudRename ? ActiveHudProfile() :
                    mode == NameEditMode::WorldRename ? ActiveWorldProfile() :
                    mode == NameEditMode::HotkeyRename ? ActiveHotkeyProfile() : L"";
    g_name_editing.store(true);
}

void ProcessProfileNameKey(int key) {
    if (key == VK_ESCAPE) {
        g_name_edit_mode = NameEditMode::None;
        g_name_editing.store(false);
        Notify(L"Profile editing cancelled");
        return;
    }
    if (key == VK_RETURN) {
        if (g_name_buffer.empty()) { Notify(L"Enter a profile name"); return; }
        bool done = false;
        if (g_name_edit_mode == NameEditMode::Create) done = CreateProfile(g_name_buffer);
        else if (g_name_edit_mode == NameEditMode::Rename) done = RenameActiveProfile(g_name_buffer);
        else if (g_name_edit_mode == NameEditMode::CustomCreate)
            done = CreateCustomisationProfile(g_name_buffer);
        else if (g_name_edit_mode == NameEditMode::CustomRename)
            done = RenameActiveCustomisationProfile(g_name_buffer);
        else if (g_name_edit_mode == NameEditMode::HudCreate)
            done = CreateHudProfile(g_name_buffer);
        else if (g_name_edit_mode == NameEditMode::HudRename)
            done = RenameActiveHudProfile(g_name_buffer);
        else if (g_name_edit_mode == NameEditMode::WorldCreate)
            done = CreateWorldProfile(g_name_buffer);
        else if (g_name_edit_mode == NameEditMode::WorldRename)
            done = RenameActiveWorldProfile(g_name_buffer);
        else if (g_name_edit_mode == NameEditMode::HotkeyCreate)
            done = CreateHotkeyProfile(g_name_buffer);
        else if (g_name_edit_mode == NameEditMode::HotkeyRename)
            done = RenameActiveHotkeyProfile(g_name_buffer);
        if (done) {
            g_name_editing.store(false);
            g_name_edit_mode = NameEditMode::None;
            g_name_buffer.clear();
        }
        return;
    }
    if (key == VK_BACK) {
        if (!g_name_buffer.empty()) g_name_buffer.pop_back();
        return;
    }
    if (g_name_buffer.size() >= 32) return;
    const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    wchar_t ch = 0;
    if (key >= 'A' && key <= 'Z') ch = shift ? static_cast<wchar_t>(key) :
                                          static_cast<wchar_t>(key + (L'a' - L'A'));
    else if (key >= '0' && key <= '9') ch = shift && key == '5' ? L'%' : static_cast<wchar_t>(key);
    else if (key == VK_SPACE) ch = L' ';
    else if (key == VK_OEM_MINUS) ch = shift ? L'_' : L'-';
    if (ch) g_name_buffer.push_back(ch);
}

void CaptureNativeButtonSplit(MyCharacter* player, int before_buttons, int after_buttons, void* actor) {
    if (!player || !IsReadable(player, sizeof(MyCharacter)) ||
        !g_button_state_initialized || !g_engine_timer_valid ||
        g_practice_active || IsSyntheticPracticeButtonContact() ||
        !g_get_accurate_time || before_buttons < 0 ||
        after_buttons != before_buttons + 1 ||
        after_buttons > kRefunctButtonCount ||
        after_buttons <= g_last_button_count) return;
    void* instance = GetGameInstance();
    if (!instance) return;
    const auto* state = reinterpret_cast<const LevelState*>(
        reinterpret_cast<const uint8_t*>(instance) + 0x9C);
    if (!IsReadable(state, sizeof(*state)) || state->buttons != after_buttons ||
        state->start_seconds < 0 ||
        !std::isfinite(state->start_partial_seconds) ||
        state->start_partial_seconds < 0.0f ||
        state->start_partial_seconds > 1.0f) return;
    const double start = state->start_seconds +
                         static_cast<double>(state->start_partial_seconds);
    // An old pending observation must never migrate across a restart.
    if (!std::isfinite(start) || std::abs(start - g_last_run_start) > 0.001) return;
    int32_t seconds = 0;
    float partial = 0.0f;
    g_get_accurate_time(player, &seconds, &partial);
    if (seconds < 0 || !std::isfinite(partial) || partial < 0.0f || partial > 1.0f)
        return;
    double now = static_cast<double>(seconds) + partial;
    if (state->level == 31 && state->end_seconds >= 0 &&
        std::isfinite(state->end_partial_seconds) &&
        state->end_partial_seconds >= 0.0f && state->end_partial_seconds <= 1.0f)
        now = state->end_seconds + static_cast<double>(state->end_partial_seconds);
    const double elapsed = now - start;
    if (!std::isfinite(elapsed) || elapsed < 0.0 || elapsed > 24.0 * 3600.0)
        return;
    s_native_button_observations[after_buttons - 1] = {start, elapsed, before_buttons};
    RecordPBGhostButtonContact(player, actor, after_buttons, start, elapsed);
}

// Snapshot section timing as the button is pressed. Drawing it later must
// never depend on mutable practice/load state or on the newest button.
double MeasuredSectionSeconds(SectionBoundary section, SectionBoundary boundary,
                              double completed_seconds) {
    if (section.section == 0 || boundary.section != section.section ||
        !std::isfinite(completed_seconds) || completed_seconds < 0.0) return -1.0;
    double start = 0.0;
    if (g_practice_active) {
        if (g_practice_phase != PracticePhase::Ready ||
            g_practice_origin != PracticeOrigin::Section ||
            !std::isfinite(g_practice_section_anchor_seconds) ||
            g_practice_section_anchor_seconds < 0.0 ||
            g_practice_section_anchor_button >= boundary.end_button)
            return -1.0;
        if (boundary.previous_end_button == g_practice_section_anchor_button) {
            start = g_practice_section_anchor_seconds;
        } else {
            const auto prior = std::find_if(g_button_splits.rbegin(),
                g_button_splits.rend(), [&](const ButtonSplit& split) {
                    return split.button_number == boundary.previous_end_button &&
                           std::isfinite(split.run_seconds) &&
                           split.run_seconds >= g_practice_section_anchor_seconds;
                });
            if (prior == g_button_splits.rend()) return -1.0;
            start = prior->run_seconds;
        }
    } else if (boundary.previous_end_button == 0) {
        // Only a run tracked from button zero has a measured start timestamp.
        if (!g_run_started_at_zero) return -1.0;
    } else {
        const auto prior = std::find_if(g_button_splits.rbegin(),
            g_button_splits.rend(), [&](const ButtonSplit& split) {
                return split.button_number == boundary.previous_end_button &&
                       std::isfinite(split.run_seconds) && split.run_seconds >= 0.0;
            });
        if (prior == g_button_splits.rend()) return -1.0;
        start = prior->run_seconds;
    }
    return DisplayedDurationSeconds(start, completed_seconds);
}

// Real elapsed time for an intermediate button such as 18.1. During
// Section Practice, exclude all time spent auto-loading previous buttons.
// Require a genuine Ready anchor or an already-timed previous button.
double MeasuredIntermediateButtonSeconds(int button, double completed_seconds) {
    if (button <= 1 || button > kRefunctButtonCount ||
        !std::isfinite(completed_seconds) || completed_seconds < 0.0)
        return -1.0;
    double start = -1.0;
    if (g_practice_active) {
        if (g_practice_phase != PracticePhase::Ready ||
            g_practice_origin != PracticeOrigin::Section ||
            !std::isfinite(g_practice_section_anchor_seconds) ||
            g_practice_section_anchor_seconds < 0.0 ||
            g_practice_section_anchor_button >= button) return -1.0;
        if (g_practice_section_anchor_button == button - 1) {
            start = g_practice_section_anchor_seconds;
        } else if (!g_button_splits.empty()) {
            const auto& previous = g_button_splits.back();
            if (previous.button_number == button - 1 &&
                previous.section_popup_expires != 0 &&
                previous.run_seconds >= g_practice_section_anchor_seconds)
                start = previous.run_seconds;
        }
    } else if (!g_button_splits.empty()) {
        const auto& previous = g_button_splits.back();
        if (previous.button_number == button - 1 &&
            std::isfinite(previous.run_seconds) && previous.run_seconds >= 0.0)
            start = previous.run_seconds;
    }
    return DisplayedDurationSeconds(start, completed_seconds);
}

void UpdateButtonSplits() {
    PollGoldFileWriteError();
    if (!g_engine_timer_valid) return;
    void* instance = GetGameInstance();
    if (!instance) return;
    const auto* state = reinterpret_cast<const LevelState*>(
        reinterpret_cast<const uint8_t*>(instance) + 0x9C);
    if (!IsReadable(state, sizeof(*state))) return;
    const int button_count = state->buttons;
    if (button_count < 0 || button_count > kRefunctButtonCount) return;
    const double start = state->start_seconds +
                         static_cast<double>(state->start_partial_seconds);
    if (!std::isfinite(start) || !std::isfinite(g_elapsed_seconds)) return;

    if (!g_button_state_initialized) {
        if (g_restart_pending_clean_taint && button_count == 0) {
            ClearTaintOnFreshRun(button_count);
            g_restart_pending_clean_taint = false;
        }
        g_last_button_count = button_count;
        g_last_run_start = start;
        g_last_run_time = g_elapsed_seconds;
        g_button_state_initialized = true;
        g_run_started_at_zero = button_count == 0;
        g_pb_cheats_allowed_for_run = CheatRecordsPB(g_cheat_record_mode);
        g_run_pb_eligible = g_run_started_at_zero && RecordsAllowedForPB();
        g_pb_recording_enabled_for_run = g_save_personal_bests;
        g_run_finish_processed = false;
        if (!g_run_started_at_zero) {
            Log(L"INFO: split tracking began after the first button; "
                L"restart the run to enable PB recording");
        }
        return;
    }

    const bool new_run = std::abs(start - g_last_run_start) > 0.001 ||
                         g_elapsed_seconds + 0.25 < g_last_run_time ||
                         button_count < g_last_button_count;
    if (new_run) {
        ClearTaintOnFreshRun(button_count);
        if (button_count == 0) g_restart_pending_clean_taint = false;
        ResetButtonHistory();
        g_practice_gold_anchor_button = 0;
        g_practice_gold_anchor_seconds = -1.0;
        g_run_started_at_zero = button_count == 0;
        g_pb_cheats_allowed_for_run = CheatRecordsPB(g_cheat_record_mode);
        g_run_pb_eligible = g_run_started_at_zero && RecordsAllowedForPB();
        g_pb_recording_enabled_for_run = g_save_personal_bests;
        g_run_finish_processed = false;
    }

    if (!new_run && button_count > g_last_button_count) {
        const int previous_button = g_last_button_count;
        // A two- or three-button section can advance more than once inside
        // one game tick. Only split those events if the native contact hook
        // observed the *entire* chain with real elapsed timestamps.
        const bool complete_native_sequence =
            button_count > previous_button + 1 &&
            CompleteNativeButtonSequence(
                s_native_button_observations, previous_button, button_count,
                start, g_button_splits.empty() ? 0.0 : g_button_splits.back().run_seconds,
                g_elapsed_seconds);
        const auto record_button = [&](int observed_button, double observed_elapsed,
                                       int previous_button) {
            const bool consecutive = observed_button == previous_button + 1;
            const bool has_split_predecessor = observed_button == 1
                ? previous_button == 0 && g_run_started_at_zero
                : !g_button_splits.empty() &&
                  g_button_splits.back().button_number == observed_button - 1;
            // During practice the loader's target button was not timed. The
            // Ready anchor, not the last synthetic split or run start, is the
            // actual beginning of the first manually played segment. This
            // anchor is valid for DISPLAY even if gold saving is disabled.
            const bool has_practice_anchor = consecutive && g_practice_active &&
                g_practice_phase == PracticePhase::Ready &&
                g_practice_origin != PracticeOrigin::ProgressRestart &&
                g_practice_gold_anchor_button == observed_button - 1 &&
                std::isfinite(g_practice_gold_anchor_seconds) &&
                g_practice_gold_anchor_seconds >= 0.0 &&
                g_practice_gold_anchor_seconds <= observed_elapsed;
            const bool has_previous = has_split_predecessor || has_practice_anchor;
            if (!consecutive || !has_previous) g_run_pb_eligible = false;

            const double previous = g_button_splits.empty() ? 0.0 :
                                    g_button_splits.back().run_seconds;
            // An observed button-count jump isn't the duration of one button:
            // showing the whole gap as a segment would be a fabricated split.
            double segment = -1.0;
            if (has_practice_anchor) {
                segment = DisplayedDurationSeconds(
                    g_practice_gold_anchor_seconds, observed_elapsed);
            } else if (consecutive && has_split_predecessor &&
                       std::isfinite(previous) && previous >= 0.0 &&
                       observed_elapsed >= previous) {
                segment = DisplayedDurationSeconds(previous, observed_elapsed);
            }
            SegmentSource source = SegmentSource::Unknown;
            if (consecutive && has_previous && std::isfinite(segment) &&
                segment >= 0.0 && RecordsAllowedForSegments()) {
                if (has_practice_anchor) {
                    source = SegmentSource::Practice;
                } else if (!g_practice_active && g_run_started_at_zero &&
                           (g_run_pb_eligible || CheatRecordsSegments(g_cheat_record_mode))) {
                    source = SegmentSource::Run;
                }
            }
            if (g_practice_active && g_practice_phase == PracticePhase::Ready &&
                g_practice_origin != PracticeOrigin::ProgressRestart &&
                consecutive && has_previous && std::isfinite(segment) && segment >= 0.0) {
                // Advance timing even with gold saving OFF; comparison displays
                // should never depend on whether records can be written.
                g_practice_gold_anchor_button = observed_button;
                g_practice_gold_anchor_seconds = observed_elapsed;
            } else if (g_practice_active && g_practice_gold_anchor_button > 0 &&
                       (!consecutive || !has_previous)) {
                g_practice_gold_anchor_button = 0;
                g_practice_gold_anchor_seconds = -1.0;
            }

            // Read a historical comparison *before* a possible best-segment save.
            // Older split profiles can have cumulative PB splits but no separate
            // [BestSegments] entries. Their per-button PB duration is still a
            // measured historical segment and is usable as a display-only fallback.
            // Never write this fallback into the gold-record table.
            const size_t segment_index = static_cast<size_t>(observed_button - 1);
            const double previous_gold = g_pb.best_segment_seconds[segment_index];
            double comparison_reference = previous_gold;
            if (!std::isfinite(comparison_reference) || comparison_reference < 0.0) {
                const double pb_current = g_pb.button_seconds[segment_index];
                const double pb_previous = observed_button == 1
                    ? 0.0 : g_pb.button_seconds[segment_index - 1];
                if (std::isfinite(pb_current) && std::isfinite(pb_previous) &&
                    pb_previous >= 0.0 && pb_current - pb_previous >= 0.050) {
                    comparison_reference = pb_current - pb_previous;
                } else {
                    comparison_reference = -1.0;
                }
            }

            // Always retrieve an existing reference, including on tainted/practice
            // runs and when auto-save is OFF. UpdateBestSegment itself prohibits
            // all writes unless the run is eligible and saving is enabled.
            const GoldUpdate gold = UpdateBestSegment(observed_button, segment, source, observed_elapsed);
            ButtonSplit split{};
            split.button_number = observed_button;
            split.run_seconds = observed_elapsed;
            split.segment_seconds = segment;
            split.comparison_best_seconds = comparison_reference;
            split.previous_recorded_gold_seconds = previous_gold;
            split.first_best_without_reference = gold.new_gold &&
                                                 comparison_reference < 0.0;
            split.source_is_practice = source == SegmentSource::Practice;
            // The run PB may be replaced in FinishPersonalBestIfNeeded() on
            // this same tick. All comparison rows must use the old snapshot.
            split.cumulative_pb_reference_seconds = g_pb.button_seconds[segment_index];
            if (observed_button == kRefunctButtonCount &&
                (!std::isfinite(split.cumulative_pb_reference_seconds) ||
                 split.cumulative_pb_reference_seconds < 0.0))
                split.cumulative_pb_reference_seconds = g_pb.total_seconds;
            if (!g_practice_active && g_run_started_at_zero &&
                consecutive && has_split_predecessor &&
                std::isfinite(split.cumulative_pb_reference_seconds) &&
                split.cumulative_pb_reference_seconds >= 0.0) {
                const double prior_run = observed_button == 1 ? 0.0 : previous;
                const double prior_pb = observed_button == 1 ? 0.0 :
                    g_pb.button_seconds[segment_index - 1];
                split.pb_gain_loss_seconds = PBLeadChangeSeconds(
                    prior_run, observed_elapsed, prior_pb,
                    split.cumulative_pb_reference_seconds);
            }
            const SectionBoundary completed_section = SectionEndingAtButton(observed_button);
            const bool intermediate_button = IsIntermediateSectionButton(observed_button);
            // Native buttons replayed by the practice loader are not timed
            // gameplay. Their independent popup lifetimes must not survive
            // the loader and later reappear as a misleading N/A.
            const bool timed_section = !g_practice_active ||
                g_practice_phase == PracticePhase::Ready;
            if (completed_section.section != 0 && timed_section) {
                const bool practice_section = g_practice_active &&
                    g_practice_phase == PracticePhase::Ready &&
                    g_practice_origin == PracticeOrigin::Section;
                const SectionBoundary practice_boundary = practice_section
                    ? PracticeSectionComparisonBoundary(
                        completed_section, g_practice_section_anchor_button)
                    : SectionBoundary{};
                // Don't treat a synthetic loaded button as a completed
                // section; for real Section Practice, compare the measured
                // remainder against exactly the same PB button range.
                const SectionBoundary reference_boundary = practice_boundary.section != 0
                    ? practice_boundary : completed_section;
                split.section_comparison_boundary = reference_boundary;
                split.section_actual_seconds = MeasuredSectionSeconds(
                    completed_section, reference_boundary, observed_elapsed);
                split.section_pb_reference_seconds = SectionDurationFromRecordedPB(
                    g_pb.section_seconds, g_pb.button_seconds, reference_boundary);
                split.section_best_reference_seconds = EarlierBestSectionReference(
                    reference_boundary, observed_button, previous_gold);
                if (split.section_pb_reference_seconds < 0.0 &&
                    split.section_best_reference_seconds < 0.0) {
                    Log((L"Section compare: missing PB and full sum-of-best for section " +
                         std::to_wstring(completed_section.section)).c_str());
                }
            } else if (intermediate_button && timed_section) {
                // 18.1, 7.1, 26.1 etc. have no completed section yet.
                // Still show a PB comparison for the individual button.
                split.section_is_partial_button = true;
                split.section_actual_seconds = MeasuredIntermediateButtonSeconds(
                    observed_button, observed_elapsed);
                split.section_pb_reference_seconds = PBButtonSegmentDuration(
                    g_pb.button_seconds, observed_button);
                // Only a genuine previously recorded gold is a 'best' fallback.
                split.section_best_reference_seconds = previous_gold;
                if (split.section_actual_seconds < 0.0) {
                    Log((L"Button compare " + ButtonLabel(observed_button) +
                         L": no measured segment start (check practice Ready anchor / previous button)").c_str());
                }
                if (split.section_pb_reference_seconds < 0.0 &&
                    split.section_best_reference_seconds < 0.0) {
                    Log((L"Button compare " + ButtonLabel(observed_button) +
                         L": PB ButtonSplits missing current/previous timestamp; no BestSegments fallback").c_str());
                }
            }
            const ULONGLONG popup_expires = GetTickCount64() + 4000;
            if ((completed_section.section != 0 || intermediate_button) && timed_section)
                split.section_popup_expires = popup_expires;
            g_button_splits.push_back(split);
            if (g_button_splits.size() > 128) g_button_splits.erase(g_button_splits.begin());
            g_split_popup_expires = popup_expires;
            RecordFurthestIncompleteSplits(observed_button, observed_elapsed);

        };
        if (complete_native_sequence) {
            for (int button = previous_button + 1; button <= button_count; ++button) {
                const double elapsed = button == button_count ? g_elapsed_seconds :
                    s_native_button_observations[button - 1].elapsed;
                record_button(button, elapsed, button - 1);
            }
        } else {
            // Preserve conservative behaviour if a native contact was not
            // observed. Log a skipped edge once so a missing reference can
            // be diagnosed from RefunctUtility.log.
            if (button_count > previous_button + 1) {
                Log((L"Split capture: button count jumped " +
                     std::to_wstring(previous_button) + L" -> " +
                     std::to_wstring(button_count) +
                     L"; intermediate native timestamp unavailable").c_str());
            }
            record_button(button_count, g_elapsed_seconds, previous_button);
        }
    }
    g_last_button_count = button_count;
    g_last_run_start = start;
    g_last_run_time = g_elapsed_seconds;
}


} // namespace refunct

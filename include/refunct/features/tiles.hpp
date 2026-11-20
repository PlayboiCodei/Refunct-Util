#pragma once
// World -> Tiles. Native BP_IslandChunk_C contacts; no counter editing.
#include "refunct/state.hpp"
namespace refunct {
// Disable automatic, physical tile activation without affecting manual Select All.
// Requires the existing native ALiftBase contact hook. Saved to settings.ini.
bool IsTileSelectionOnContactDisabled();
bool TileSelectionOnContactDisabledPreference();
bool ToggleTileSelectionOnContact();
bool ShouldBlockPhysicalTileContact(void* actor, MyCharacter* player);
// Called after the original native contact; queues visual work for the game
// tick (never creates a material from the hook/physics callback).
void NotifyNativeTileContactCompleted(void* actor, MyCharacter* player);
// Independent World > Buttons control: a native button press should not
// permanently colour nearby tiles. Requires the same optional contact hook.
bool IsButtonTileSelectionDisabled();
bool ButtonTileSelectionDisabledPreference();
bool ToggleButtonTileSelection();
// Pair these around the original native button contact; resolve any visual
// side-effects later on the game tick after Blueprint progression has settled.
bool BeginButtonTileSelectionGuard(void* actor, MyCharacter* player);
void EndButtonTileSelectionGuard();
void UpdateButtonTileSelectionGuard(MyCharacter* player);
// Replay-only simulated native footsteps. The geometry cache is built in
// bounded chunks before playback starts; unavailable reflection fails open.
enum class GhostReplayTileStatus { Pending, Ready, Unavailable };
GhostReplayTileStatus PrepareGhostReplayTiles(MyCharacter* player);
void ResetGhostReplayTiles();
bool GhostReplayTilesChanged();
void TouchGhostReplayTiles(MyCharacter* player, const Vec3& old_pose,
                          const Vec3& new_pose, bool continuous);
// Select (colour) all tile actors in raised sections, or all 251 map tiles.
bool SelectWorldTiles(bool all_tiles);
// Restore each tile's verified native state/material and the matching progress count.
bool DeselectWorldTiles(bool all_tiles);
// Continue the batch on the game thread, limited to one tile per frame.
void UpdateWorldTileSelection(MyCharacter* player);
// Native 2-slot material palette, separately for selected/unselected tiles.
// 0=game default, 1..8=original Refunct activated-material palettes.
bool IsTileCustomColourEnabled(bool selected);
int GetTileColourPreset(bool selected);
std::wstring TileColourPresetLabel(bool selected);
void CycleTileColourPreset(bool selected, int direction);
// Restore both native tile palettes and disable Random RGB without touching selection.
void RestoreDefaultTileColours();
// Profile-only, atomic state restore. No changes to tile selection/progression.
void ApplyTileColourProfileState(int selected_preset, int unselected_preset,
    bool selected_custom, bool unselected_custom,
    uint32_t selected_rgb, uint32_t unselected_rgb, bool random_selected_rgb);
void UpdateTileColourPresets(MyCharacter* player);
// Visual-only random RGB assigned once on each 0 -> selected transition.
bool IsRandomTileRGBEnabled();
void ToggleRandomTileRGB();
// Custom RGB override for either tile state; visual-only and persisted in settings.ini.
void SetTileHexColour(bool selected, uint32_t rgb);
std::wstring GetTileHexColour(bool selected);
// Opens the native Windows colour dialog on a separate UI thread.
void OpenTileColourPicker(bool selected);
// Independent selected/unselected visual tints for the concrete wall faces.
// Does not touch the BP_IslandChunk_C Color field or LevelState platforms.
bool IsWallCustomColourEnabled(bool selected);
std::wstring WallColourLabel(bool selected);
std::wstring GetWallHexColour(bool selected);
void SetWallCustomColourEnabled(bool selected, bool enabled);
void SetWallHexColour(bool selected, uint32_t rgb);
void OpenWallColourPicker(bool selected);
} // namespace refunct

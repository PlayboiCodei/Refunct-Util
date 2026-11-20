#pragma once
#include "refunct/state.hpp"
namespace refunct {
// Gameplay-thread recording; never modifies the player's native state.
void UpdatePBGhostRecording(const MyCharacter* player);
void RecordPBGhostButtonContact(const MyCharacter* player, const void* actor,
                                int split, double run_start, double elapsed);
void ResetPBGhostCapture();
// Called only after the existing PB file has been saved successfully.
void CommitPBGhostOnNewPB(double run_seconds);
// Only true if a new ghost was written for the most recent PB transaction.
bool PBGhostSavedForLatestPB();
// Called when the active split profile and its RunTime have been loaded.
void LoadPBGhostForActiveProfile();
void RenamePBGhostProfile(const std::wstring& old_pb_path,
                          const std::wstring& new_pb_path);
bool IsPBGhostEnabled();
void SetPBGhostEnabled(bool enabled);
// ON: true world-space ghost; OFF: original 2D HUD silhouette.
bool IsPBGhostPassThroughEnabled();
void SetPBGhostPassThroughEnabled(bool enabled);
// One persistent RRGGBB colour for both 2D HUD and 3D native material ghosts.
uint32_t GetPBGhostColourRgb();
void SetPBGhostColourRgb(uint32_t rgb);
// Independent persistent appearance options, default ON.
bool IsPBGhost2DTransparencyEnabled();
void SetPBGhost2DTransparencyEnabled(bool enabled);
bool IsPBGhost3DTransparencyEnabled();
void SetPBGhost3DTransparencyEnabled(bool enabled);
// ON: play each PB segment from the moment its preceding native button is pressed.
bool IsPBGhostButtonSyncEnabled();
void SetPBGhostButtonSyncEnabled(bool enabled);
// ON: use independent fastest section movement tracks. OFF: use the full
// PB run, falling back to the saved furthest clean run when necessary.
bool IsPBGhostFastestSectionsEnabled();
void SetPBGhostFastestSectionsEnabled(bool enabled);
// Called only after a legitimate, persisted new fastest segment. Only clean
// native run captures are stored as segment movement; practice and synthetic
// segments cannot be fabricated from timing data alone.
void QueueFastestSegmentGhost(int button, double started_at, double ended_at,
                              bool native_run);
// Wait for previously queued ghost files before switching/renaming profiles.
bool DrainFastestSegmentGhostWrites();
void CopyFastestSegmentGhosts(const std::wstring& destination_pb_path);
std::wstring PBGhostRecordingStatus();
// Spectator replay advances Refunct through real native button contacts.
// A replay starts from a clean level and resets the level after stop.
bool IsGhostReplayRequested();
bool IsGhostReplayPaused();
std::wstring GhostReplaySourceLabel();
// Independent Replay Viewer source: PB or saved best segments.
void ToggleGhostReplaySource();
std::wstring GhostReplayStatus();
std::wstring GhostReplaySpeedLabel();
// Camera preference persists across replay restarts and profile loads.
bool IsGhostReplayFirstPerson();
// True only for the current player while the first-person replay is active.
bool IsGhostReplayFirstPersonActive(const MyCharacter* player);
// True only while the spectator camera is live in Free Cam mode.
bool IsGhostReplayFreeCameraActive();
// True while the follow-orbit camera receives mouse-wheel zoom.
bool IsGhostReplayOrbitCameraActive();
// Called only by the same-process Win32 mouse observer; no UE work on hook thread.
void QueueGhostReplayOrbitWheel(short delta);
std::wstring GhostReplayCameraLabel();
void ToggleGhostReplayCameraMode();
bool StartGhostReplayViewer();     // Also stops a currently running replay.
void StopGhostReplayViewer();
void ToggleGhostReplayPause();
void SeekGhostReplaySeconds(int seconds);
void RestartGhostReplay();
void CycleGhostReplaySpeed(int direction);
void UpdateGhostReplayViewer(MyCharacter* player);
bool HasPBGhostForActiveProfile();
void DrawPBGhost(void* hud, int32_t viewport_width, int32_t viewport_height);
void DrawWorldPBGhost(MyCharacter* player);
} // namespace refunct

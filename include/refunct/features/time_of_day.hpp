#pragma once

namespace refunct {
struct MyCharacter;

// Visual-only control of Refunct's native day/night clock. Independent of
// the run timer, global game speed, and speedrun record eligibility.
bool IsCustomTimeOfDayEnabled();
void SetCustomTimeOfDayEnabled(bool enabled);
int GetCustomTimeMinutes();          // [0, 1439]; displays live time when running
// Returns -1 when the native clock is not yet discovered. Never changes the clock.
int GetLiveGameTimeMinutes();
void AdjustCustomTimeMinutes(int direction); // 15-minute increments
void SetCustomTimeMinutes(int minutes);
bool IsCustomTimeLocked();
void SetCustomTimeLocked(bool locked);
int GetCustomTimeSpeedPercent();       // 25..1000, 100 = native progression
void SetCustomTimeSpeedPercent(int percent);
void AdjustCustomTimeSpeedPercent(int direction); // +/-25%
void UpdateCustomTimeOfDay(MyCharacter* player);
}

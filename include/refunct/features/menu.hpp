#pragma once
// Internal functions owned by this feature's .cpp file.
#include "refunct/state.hpp"
namespace refunct {
void SetPage(Page page);
// World gameplay reset used by launch/restart persistence policies.
void ResetWorldGameplayForPersistence();
void ExecuteAction(Action action, MyCharacter* player);
std::vector<std::wstring> Entries();
const wchar_t* MenuBreadcrumb(Page page);
std::wstring MenuSelectionDescription(Page page, int selected,
                                      const std::vector<std::wstring>& entries);
void GoBack();
void ToggleProgressRiseAnimation();
void ActivateSelected(MyCharacter* player);
void ProcessKey(int key, MyCharacter* player);
bool IsMenuSearchTypingKey(int key); // Input hook consumes typed search characters.
void QueueMenuSearchKey(int key); // Preserve typing order from Slate callback.
void DrainMenuSearchKeys(MyCharacter* player); // Run on game tick.
void PollHudColourPicker(); // Commits native dialog results on game-tick thread.
} // namespace refunct

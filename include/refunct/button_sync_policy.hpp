#pragma once
// Pure button gate rules for Refunct v9.30. Indexes are zero-based and
// finished_buttons is the game's current sequential button count.
// Does not alter game progression or assume any native actor layout.
namespace refunct::button_sync_policy {

constexpr bool DesiredIsPressed(int button_index, int finished_buttons,
                                bool section_is_raised) noexcept {
    // A completed button must not be reactivated; otherwise the native
    // IsPressed gate is only cleared if the parent section is fully raised.
    return button_index < finished_buttons || !section_is_raised;
}

constexpr bool VanillaIsPressed(int button_index, int finished_buttons) noexcept {
    // In the diagnostic, the game unlocked the next unpressed button after
    // each native contact. Future and completed buttons remained locked.
    return button_index != finished_buttons;
}

} // namespace refunct::button_sync_policy

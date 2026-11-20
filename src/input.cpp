#include "refunct/internal_api.hpp"

namespace refunct {
bool HasFocusedGameWindow() {
    const HWND hwnd = GetForegroundWindow();
    DWORD pid = 0;
    if (hwnd) GetWindowThreadProcessId(hwnd, &pid);
    return pid == GetCurrentProcessId();
}

// This hook only observes messages from Refunct's own UI thread. It does not
// call UE or modify input messages. Data is queued for the character tick.
LRESULT CALLBACK MouseWheelInputHook(int code, WPARAM message, LPARAM details) {
    // The F5 menu is an overlay, not a pause screen: continue accepting
    // scroll-jump requests (either wheel direction) while it is displayed. Name editing and hotkey
    // capture still suppress gameplay input to avoid accidental jumps.
    if (code == HC_ACTION && message == WM_MOUSEWHEEL && details &&
        HasFocusedGameWindow() &&
        !g_rebinding.load(std::memory_order_relaxed) &&
        !g_name_editing.load(std::memory_order_relaxed) &&
        !g_coordinate_editing.load(std::memory_order_relaxed)) {
        const auto* data = reinterpret_cast<const MOUSEHOOKSTRUCTEX*>(details);
        const short delta = static_cast<short>(HIWORD(data->mouseData));
        if (delta != 0 && IsGhostReplayOrbitCameraActive()) {
            // A replay orbit wheel event is zoom, never scroll-jump. Keep
            // Unreal from also delivering it to the hidden player's input.
            QueueGhostReplayOrbitWheel(delta);
            return 1;
        }
        if (delta != 0 && g_scroll_to_jump) {
            // Both wheel directions request a jump during ordinary gameplay.
            const int old = g_wheel_jump_requests.load(std::memory_order_relaxed);
            if (old < 8) g_wheel_jump_requests.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return CallNextHookEx(nullptr, code, message, details);
}

void UpdateMouseWheelHook() {
    if (!g_scroll_to_jump && !IsGhostReplayOrbitCameraActive()) {
        g_wheel_jump_requests.store(0, std::memory_order_relaxed);
        if (g_mouse_wheel_hook) {
            UnhookWindowsHookEx(g_mouse_wheel_hook);
            g_mouse_wheel_hook = nullptr;
            g_mouse_wheel_thread = 0;
        }
        return;
    }
    const HWND foreground = GetForegroundWindow();
    DWORD pid = 0;
    const DWORD thread = foreground ? GetWindowThreadProcessId(foreground, &pid) : 0;
    if (pid != GetCurrentProcessId() || thread == 0) return;
    if (g_mouse_wheel_hook && g_mouse_wheel_thread == thread) return;
    if (g_mouse_wheel_hook) {
        UnhookWindowsHookEx(g_mouse_wheel_hook);
        g_mouse_wheel_hook = nullptr;
    }
    const ULONGLONG now = GetTickCount64();
    if (now < g_mouse_wheel_retry_after) return;
    // Same-process, thread-specific mouse hook. No global hooks or UE detours.
    HHOOK hook = SetWindowsHookExW(WH_MOUSE, MouseWheelInputHook, nullptr, thread);
    if (!hook) {
        g_mouse_wheel_retry_after = now + 5000;
        if (!g_mouse_wheel_warning_logged) {
            g_mouse_wheel_warning_logged = true;
            Log(L"WARNING: cannot install Win32 scroll jump hook; feature unavailable");
            Notify(L"Scroll jump unavailable: check RefunctUtility.log");
        }
        return;
    }
    g_mouse_wheel_hook = hook;
    g_mouse_wheel_thread = thread;
    g_mouse_wheel_warning_logged = false;
    Log(L"Win32 thread-specific bidirectional scroll jump hook installed");
}

namespace {
// A key held before entering Free Cam may already be down in native UE4.
// Remember those forwarded presses so its eventual key-up is still delivered,
// even when Free Cam suppresses new key-down events.
std::array<std::atomic_bool, 256> s_native_camera_key_down{};
bool IsFreeCameraMovementKey(int key) {
    return key == 'W' || key == 'A' || key == 'S' || key == 'D' ||
           key == VK_SPACE || key == VK_SHIFT ||
           key == VK_LSHIFT || key == VK_RSHIFT;
}
bool IsFreeCameraLookKey(int key) {
    return key == VK_LEFT || key == VK_RIGHT ||
           key == VK_UP || key == VK_DOWN;
}
bool IsFreeCameraKey(int key) {
    return IsFreeCameraMovementKey(key) || IsFreeCameraLookKey(key);
}
} // namespace

bool IsBound(int key) {
    // Keep an emergency menu key if the user explicitly unbinds Menu.
    // Otherwise a closed overlay could become impossible to reopen.
    if (key == VK_F5 && Hotkey(Action::Menu) == 0) return true;
    for (const auto& binding : g_keys) {
        if (key != 0 && binding.load(std::memory_order_relaxed) == key) return true;
    }
    return false;
}

void __fastcall KeyDownHook(void* app, void* /*edx*/, int key, uint32_t code, bool repeat) {
    // Never intercept Windows' Alt+F4 close shortcut, even though F4 is our
    // default Save State hotkey. This also protects a user-rebound F4 action.
    if (key == VK_F4 && (GetAsyncKeyState(VK_MENU) & 0x8000)) {
        if (g_original_keydown) g_original_keydown(app, key, code, repeat);
        return;
    }
    bool consumed = false;
    if (key >= 0 && key < 256 && HasFocusedGameWindow()) {
        const bool menu = g_menu_open.load(std::memory_order_relaxed);
        const bool rebinding = g_rebinding.load(std::memory_order_relaxed);
        const bool menu_search_key = IsMenuSearchTypingKey(key);
        if (menu_search_key && (key != VK_SHIFT && key != VK_LSHIFT && key != VK_RSHIFT))
            QueueMenuSearchKey(key);
        // Free Cam flight controls work while F5 is open, without moving the
        // hidden player. Arrow keys still navigate the menu when it is open.
        // Text entry and rebinding have priority over spectator movement.
        const bool camera_input_allowed = !rebinding &&
            !g_name_editing.load(std::memory_order_relaxed) &&
            !g_coordinate_editing.load(std::memory_order_relaxed) &&
            !menu_search_key && !(menu && IsMenuSearchTypingKey('W'));
        const bool freecam_key = camera_input_allowed && IsGhostReplayFreeCameraActive() &&
            (IsFreeCameraMovementKey(key) || (!menu && IsFreeCameraLookKey(key)));
        if (freecam_key) {
            // Keep spectator movement out of native player input.
        } else if (!repeat) {
            g_pending[key].store(1, std::memory_order_relaxed);
            g_pending_any.store(true, std::memory_order_release);
        }
        consumed = (freecam_key || rebinding || g_name_editing.load(std::memory_order_relaxed) ||
                    g_coordinate_editing.load(std::memory_order_relaxed) ||
                    menu_search_key || IsBound(key) ||
                    // Backspace is reserved for clearing a hotkey while the
                    // overlay is open; never forward it to the game.
                    (menu && (key == VK_OEM_2 || key == VK_UP || key == VK_DOWN || key == VK_LEFT ||
                              key == VK_RIGHT || key == VK_RETURN || key == VK_ESCAPE ||
                              key == VK_BACK)));
        if (consumed) g_suppress_keyup[key].store(1, std::memory_order_relaxed);
    }
    if (!consumed && g_original_keydown) {
        g_original_keydown(app, key, code, repeat);
        if (key >= 0 && key < 256 && IsFreeCameraKey(key))
            s_native_camera_key_down[key].store(true, std::memory_order_relaxed);
    }
}

void __fastcall KeyUpHook(void* app, void* /*edx*/, int key, uint32_t code, bool repeat) {
    const bool valid = key >= 0 && key < 256;
    const bool consumed = valid &&
        g_suppress_keyup[key].exchange(0, std::memory_order_relaxed) != 0;
    const bool native_was_down = valid && IsFreeCameraKey(key) &&
        s_native_camera_key_down[key].exchange(false, std::memory_order_relaxed);
    // Prevent stuck movement if camera mode changed while a key was held.
    if ((!consumed || native_was_down) && g_original_keyup)
        g_original_keyup(app, key, code, repeat);
}


} // namespace refunct

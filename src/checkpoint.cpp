#include "refunct/internal_api.hpp"

namespace refunct {
bool ValidCheckpoint(const Checkpoint& saved) {
    const auto coordinate = [](float value) {
        return std::isfinite(value) && std::abs(value) <= 100000.0f;
    };
    const auto angle = [](float value) {
        return std::isfinite(value) && std::abs(value) <= 100000.0f;
    };
    return saved.buttons >= 0 && saved.buttons <= kRefunctButtonCount &&
        coordinate(saved.location.x) && coordinate(saved.location.y) &&
        coordinate(saved.location.z) && angle(saved.rotation.pitch) &&
        angle(saved.rotation.yaw) && angle(saved.rotation.roll);
}

bool CaptureCheckpoint(MyCharacter* player, Checkpoint& result) {
    if (!player || !IsReadable(player, sizeof(*player)) ||
        !IsReadable(player->root_component, sizeof(SceneComponent)) ||
        !IsReadable(player->controller, sizeof(PlayerController))) return false;
    const auto* state = PracticeLevelState();
    if (!state) return false;
    Checkpoint snapshot{};
    snapshot.buttons = state->buttons;
    snapshot.location = player->root_component->location;
    snapshot.rotation = player->controller->control_rotation;
    if (!ValidCheckpoint(snapshot)) return false;
    result = snapshot;
    return true;
}

std::wstring CheckpointFloat(float number) {
    wchar_t buffer[64]{};
    swprintf_s(buffer, L"%.9g", static_cast<double>(number));
    return buffer;
}

bool PersistCheckpoint(const Checkpoint& snapshot) {
    if (g_checkpoint_path.empty() || !ValidCheckpoint(snapshot)) return false;
    const auto write = [&](const wchar_t* name, const std::wstring& value) {
        return WritePrivateProfileStringW(L"Checkpoint", name, value.c_str(),
                                          g_checkpoint_path.c_str()) != 0;
    };
    // Readers ignore the file until every field has been written.
    if (!write(L"Valid", L"0")) return false;
    bool success = write(L"Version", L"1");
    success = write(L"Buttons", std::to_wstring(snapshot.buttons)) && success;
    success = write(L"X", CheckpointFloat(snapshot.location.x)) && success;
    success = write(L"Y", CheckpointFloat(snapshot.location.y)) && success;
    success = write(L"Z", CheckpointFloat(snapshot.location.z)) && success;
    success = write(L"Pitch", CheckpointFloat(snapshot.rotation.pitch)) && success;
    success = write(L"Yaw", CheckpointFloat(snapshot.rotation.yaw)) && success;
    success = write(L"Roll", CheckpointFloat(snapshot.rotation.roll)) && success;
    if (!success || !write(L"Valid", L"1")) return false;
    return true;
}

bool LoadPersistedCheckpoint() {
    if (g_checkpoint_path.empty() || !IsRegularFile(g_checkpoint_path)) return false;
    if (GetPrivateProfileIntW(L"Checkpoint", L"Valid", 0, g_checkpoint_path.c_str()) != 1 ||
        GetPrivateProfileIntW(L"Checkpoint", L"Version", 0, g_checkpoint_path.c_str()) != 1)
        return false;
    Checkpoint snapshot{};
    snapshot.buttons = GetPrivateProfileIntW(L"Checkpoint", L"Buttons", -1,
                                              g_checkpoint_path.c_str());
    const auto read_float = [&](const wchar_t* name, float& result) {
        wchar_t value[80]{};
        GetPrivateProfileStringW(L"Checkpoint", name, L"", value,
                                 static_cast<DWORD>(std::size(value)), g_checkpoint_path.c_str());
        if (!value[0]) return false;
        wchar_t* end = nullptr;
        errno = 0;
        const float parsed = std::wcstof(value, &end);
        if (end == value || *end != L'\0' || errno == ERANGE || !std::isfinite(parsed))
            return false;
        result = parsed;
        return true;
    };
    if (!read_float(L"X", snapshot.location.x) ||
        !read_float(L"Y", snapshot.location.y) ||
        !read_float(L"Z", snapshot.location.z) ||
        !read_float(L"Pitch", snapshot.rotation.pitch) ||
        !read_float(L"Yaw", snapshot.rotation.yaw) ||
        !read_float(L"Roll", snapshot.rotation.roll) ||
        !ValidCheckpoint(snapshot)) {
        Log(L"Save state: corrupt or unsupported save-state.ini ignored");
        return false;
    }
    g_saved_checkpoint = snapshot;
    g_saved_checkpoint_valid = true;
    Log((L"Save state: restored checkpoint file with " +
         std::to_wstring(snapshot.buttons) + L" buttons").c_str());
    return true;
}


} // namespace refunct

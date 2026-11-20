#include "refunct/internal_api.hpp"

namespace refunct {
void UpdateTimer(const MyCharacter* player) {
    g_engine_timer_valid = false;
    g_timer_complete = false;
    void* instance = GetGameInstance();
    if (!instance || !g_get_accurate_time) return;
    auto* level = reinterpret_cast<const LevelState*>(reinterpret_cast<const uint8_t*>(instance) + 0x9C);
    if (!IsReadable(level, sizeof(LevelState))) return;
    // End time can be unset during an active run; validate it only at finish.
    if (level->level < 0 || level->level > 31 || level->start_seconds < 0 ||
        !std::isfinite(level->start_partial_seconds) ||
        level->start_partial_seconds < 0.0f || level->start_partial_seconds > 1.0f) return;
    if (level->level == 31 &&
        (level->end_seconds < 0 || !std::isfinite(level->end_partial_seconds) ||
         level->end_partial_seconds < 0.0f || level->end_partial_seconds > 1.0f)) return;
    int32_t seconds = 0;
    float partial = 0.0f;
    g_get_accurate_time(player, &seconds, &partial);
    if (seconds < 0 || !std::isfinite(partial) || partial < 0 || partial > 1.0f) return;
    const double start = level->start_seconds + static_cast<double>(level->start_partial_seconds);
    const double now = seconds + static_cast<double>(partial);
    const double end = level->end_seconds + static_cast<double>(level->end_partial_seconds);
    const bool finished = level->level == 31 && end >= start;
    const double elapsed = (finished ? end : now) - start;
    if (elapsed < 0.0 || elapsed > 24.0 * 3600.0) return;
    g_elapsed_seconds = elapsed;
    g_engine_timer_valid = true;
    g_timer_complete = finished;
}

std::wstring TimerText() {
    double elapsed = g_elapsed_seconds;
    if (!g_engine_timer_valid && g_frequency.QuadPart > 0) {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        elapsed = std::max(0.0, static_cast<double>(now.QuadPart - g_session_start.QuadPart) /
                                         static_cast<double>(g_frequency.QuadPart));
    }
    // Render from exactly the same millisecond conversion used by the PB
    // split popup and the persisted profile. Avoid a displayed 1ms mismatch
    // between the live timer and the recorded split.
    const std::wstring time = FormatSplitTime(elapsed);
    return std::wstring(g_engine_timer_valid ? L"RUN " : L"SESSION ") +
        (time == L"--:--.---" ? L"N/A" : time) +
        (g_timer_complete ? L"  FINISHED" : L"");
}


} // namespace refunct

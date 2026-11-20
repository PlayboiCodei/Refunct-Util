#pragma once
// Immutable button contact schedule for the spectator ghost. Separate from
// gameplay state; callers must check native progression after each contact.
#include <array>
#include <cstdint>
#include <cstddef>

namespace refunct::ghost_replay_progress {
template <std::size_t N>
struct Timeline {
    std::array<std::uint32_t, N> at_ms{};
    std::size_t count = 0;
    std::uint32_t duration_ms = 0;
    bool Add(std::uint32_t time_ms) {
        if (count >= N || !time_ms || time_ms > duration_ms ||
            (count && time_ms <= at_ms[count-1])) return false;
        at_ms[count++] = time_ms;
        return true;
    }
    bool Due(std::size_t next, double elapsed_ms) const {
        return next < count && elapsed_ms >= at_ms[next];
    }
};
} // namespace refunct::ghost_replay_progress

#pragma once
#include <atomic>
#include <cstdint>

namespace refunct {
// Default OFF: no timing calls and no per-frame allocations when disabled.
inline std::atomic_bool g_performance_monitor_enabled{false};
// A new session must never include samples gathered before an OFF/ON cycle.
inline std::atomic_uint64_t g_performance_session{0};
inline void SetPerformanceMonitorEnabled(bool enabled) noexcept {
    if (g_performance_monitor_enabled.exchange(enabled, std::memory_order_relaxed) != enabled)
        g_performance_session.fetch_add(1, std::memory_order_release);
}
enum class PerfArea : unsigned {
    Tick, NativeTick, Hud, Movement, Progress, Records, Ghost, Practice,
    WorldRemoval, Cubes, Tubes, Platforms, Pillars, JumpPads, Water,
    TimeSkyFogShadows, Tiles, TimeOfDay, Sky, Fog, Shadows, WaterSwimming, Count
};
std::int64_t PerformanceBegin(PerfArea area) noexcept;
void PerformanceEnd(PerfArea area, std::int64_t start) noexcept;
class PerformanceScope final {
public:
    explicit PerformanceScope(PerfArea area) noexcept : area_(area) {
        if (g_performance_monitor_enabled.load(std::memory_order_relaxed))
            start_ = PerformanceBegin(area);
    }
    ~PerformanceScope() noexcept { if (start_) PerformanceEnd(area_, start_); }
    PerformanceScope(const PerformanceScope&) = delete;
    PerformanceScope& operator=(const PerformanceScope&) = delete;
private:
    PerfArea area_;
    std::int64_t start_ = 0;
};
} // namespace refunct

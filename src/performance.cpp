#include "refunct/internal_api.hpp"
#include "refunct/performance.hpp"

namespace refunct {
namespace {
constexpr size_t kAreas = static_cast<size_t>(PerfArea::Count);
constexpr const wchar_t* kNames[kAreas] = {
    L"Character tick (incl native)", L"Native tick", L"Utility HUD",
    L"Movement", L"Progress/timer", L"Records", L"Ghost", L"Practice/maps",
    L"World removal", L"Cubes", L"Tubes", L"Platforms", L"Pillars",
    L"Jump pads", L"Water", L"Time/sky/fog/shadows", L"Tiles",
    L"Time of day", L"Sky", L"Fog", L"Shadows", L"Water swimming (pre/post tick)"
};
struct Timing { double sum = 0, peak = 0; uint64_t calls = 0; };
struct Frames {
    // Bounded histogram (0.25 ms buckets). No sorting or allocating each frame.
    std::array<uint64_t, 512> count{};
    std::array<double, 512> sum{};
    double total = 0, peak = 0;
    uint64_t samples = 0;
    void Add(double ms) noexcept {
        if (!std::isfinite(ms) || ms <= 0) return;
        const auto bucket = static_cast<size_t>((std::min)(ms * 4.0, 511.0));
        ++count[bucket]; sum[bucket] += ms; ++samples; total += ms;
        peak = (std::max)(peak, ms);
    }
    double Low(double fraction) const noexcept {
        if (!samples) return 0;
        const auto wanted = static_cast<uint64_t>(std::ceil(samples * fraction));
        uint64_t remaining = wanted;
        double slow = 0;
        for (size_t i = count.size(); i-- > 0 && remaining;) {
            if (!count[i]) continue;
            const auto take = (std::min)(remaining, count[i]);
            slow += sum[i] * static_cast<double>(take) / static_cast<double>(count[i]);
            remaining -= take;
        }
        return slow > 0 ? 1000.0 * static_cast<double>(wanted) / slow : 0;
    }
};
struct Window {
    std::array<Timing, kAreas> areas{};
    Frames tick_intervals{}, hud_intervals{};
    int64_t start = 0, last_tick = 0, last_hud = 0;
    unsigned depth = 0;
    uint64_t session = 0;
};
thread_local Window s_window{};
int64_t Frequency() noexcept {
    static const auto value = [] { LARGE_INTEGER f{}; return QueryPerformanceFrequency(&f) ? f.QuadPart : 0LL; }();
    return value;
}
int64_t Clock() noexcept {
    LARGE_INTEGER t{};
    return QueryPerformanceCounter(&t) ? t.QuadPart : 0;
}
void Report(const Window& w) {
    if (!g_util_log_enabled.load(std::memory_order_relaxed)) return;
    wchar_t buffer[512]{};
    auto report_frames = [&](const wchar_t* label, const Frames& f) {
        if (!f.samples) return;
        std::swprintf(buffer, std::size(buffer),
            L"Performance %ls intervals: samples=%llu avg FPS=%.1f estimated 1%% low=%.1f estimated 0.1%% low=%.1f worst=%.3f ms",
            label, static_cast<unsigned long long>(f.samples), 1000.0 * f.samples / f.total,
            f.Low(0.01), f.Low(0.001), f.peak);
        Log(buffer);
    };
    report_frames(L"HUD callback", w.hud_intervals);
    report_frames(L"character tick", w.tick_intervals);
    for (size_t i = 0; i < kAreas; ++i) {
        const auto& a = w.areas[i];
        if (!a.calls) continue;
        std::swprintf(buffer, std::size(buffer),
            L"Performance %ls: calls=%llu avg=%.4f ms peak=%.4f ms total=%.3f ms",
            kNames[i], static_cast<unsigned long long>(a.calls), a.sum / a.calls, a.peak, a.sum);
        Log(buffer);
    }
}
} // namespace
int64_t PerformanceBegin(PerfArea area) noexcept {
    if (!g_util_log_enabled.load(std::memory_order_relaxed)) return 0;
    const auto now = Clock(), freq = Frequency();
    if (!now || !freq) return 0;
    auto& w = s_window;
    const auto session = g_performance_session.load(std::memory_order_acquire);
    // Menu input may toggle the monitor inside an active character tick.
    // Do not mix the old scope with a new session, even if nested scopes run.
    if (w.depth && w.session != session) return 0;
    if (!w.start || (!w.depth && (w.session != session || now - w.start > freq * 20))) {
        w = {}; w.start = now; w.session = session;
    }
    ++w.depth;
    auto track = [&](int64_t& last, Frames& f) {
        if (last && now > last) f.Add(1000.0 * (now - last) / freq);
        last = now;
    };
    if (area == PerfArea::Tick) track(w.last_tick, w.tick_intervals);
    if (area == PerfArea::Hud) track(w.last_hud, w.hud_intervals);
    return now;
}
void PerformanceEnd(PerfArea area, int64_t start) noexcept {
    auto& w = s_window;
    if (w.depth) --w.depth;
    if (w.session != g_performance_session.load(std::memory_order_acquire) ||
        !g_performance_monitor_enabled.load(std::memory_order_relaxed)) return;
    const auto now = Clock(), freq = Frequency();
    if (!freq || !now || now < start) return;
    auto& a = w.areas[static_cast<size_t>(area)];
    const double ms = 1000.0 * (now - start) / freq;
    a.sum += ms; a.peak = (std::max)(a.peak, ms); ++a.calls;
    if (!w.depth && (area == PerfArea::Tick || area == PerfArea::Hud) && now - w.start >= freq * 5) {
        // Reports use the existing bounded async log writer. No synchronous IO.
        try { Report(w); } catch (...) {} // Instrumentation must not disrupt gameplay.
        const auto last_tick = w.last_tick, last_hud = w.last_hud;
        const auto session = w.session;
        w = {}; w.start = now; w.last_tick = last_tick; w.last_hud = last_hud;
        w.session = session;
    }
}
} // namespace refunct

#pragma once
// The individual material/actor work queues are already bounded. Their old
// independent deadlines could nevertheless add up to several milliseconds in
// ONE game frame. This header shares a small CPU-time allowance between the
// existing cosmetic queues, without changing native gameplay tick frequency.
#include <windows.h>
#include <algorithm>
#include <cmath>

namespace refunct {
namespace colour_work_detail {
struct FrameAllowance {
    bool active = false;
    LONGLONG remaining_ticks = 0;
};
// Cosmetic work and its owning character tick run on the UE game thread.
// Keep the allowance thread-local so off-thread use of a standalone budget
// cannot consume the game thread's allowance.
inline thread_local FrameAllowance frame{};

inline LONGLONG Frequency() noexcept {
    static const LONGLONG frequency = []() noexcept {
        LARGE_INTEGER value{};
        return QueryPerformanceFrequency(&value) && value.QuadPart > 0
            ? value.QuadPart : 0LL;
    }();
    return frequency;
}
} // namespace colour_work_detail

// Establish a fresh aggregate allowance for each character tick. Only work
// performed INSIDE ColourWorkBudget scopes is charged: native physics, ghost
// movement, button contacts and unrelated gameplay work stay unrestricted.
class ScopedColourFrameBudget final {
public:
    explicit ScopedColourFrameBudget(float dt) noexcept {
        auto& frame = colour_work_detail::frame;
        previous_ = frame;
        const LONGLONG frequency = colour_work_detail::Frequency();
        if (!frequency) { frame = {}; return; }
        // At high FPS the frame is shorter, so spend less on background work.
        // A game hitch must NOT increase the following frame's allowance.
        const float seconds = std::isfinite(dt) && dt > 0.0f
            ? std::clamp(dt, 1.0f / 500.0f, 1.0f / 60.0f)
            : 1.0f / 60.0f;
        const int microseconds = std::clamp(
            static_cast<int>(seconds * 80000.0f), 400, 1400);
        frame.active = true;
        frame.remaining_ticks = (frequency * microseconds) / 1000000LL;
        if (frame.remaining_ticks < 1) frame.remaining_ticks = 1;
    }
    ~ScopedColourFrameBudget() noexcept { colour_work_detail::frame = previous_; }
    ScopedColourFrameBudget(const ScopedColourFrameBudget&) = delete;
    ScopedColourFrameBudget& operator=(const ScopedColourFrameBudget&) = delete;
private:
    colour_work_detail::FrameAllowance previous_{};
};

class ColourWorkBudget final {
public:
    explicit ColourWorkBudget(int microseconds) noexcept {
        const LONGLONG frequency = colour_work_detail::Frequency();
        LARGE_INTEGER start{};
        if (!frequency || !QueryPerformanceCounter(&start)) return;
        start_ = start.QuadPart;
        measured_ = true;
        deadline_ = start_ +
            (frequency * static_cast<LONGLONG>(std::max(0, microseconds))) / 1000000LL;
        const auto& frame = colour_work_detail::frame;
        if (frame.active) {
            shared_ = true;
            // The remaining shared allowance is charged on destruction,
            // excluding native work between separate colour work scopes.
            const LONGLONG shared_deadline = start_ +
                std::max<LONGLONG>(0, frame.remaining_ticks);
            deadline_ = (std::min)(deadline_, shared_deadline);
        }
    }
    ~ColourWorkBudget() noexcept {
        if (!measured_ || !shared_ || !colour_work_detail::frame.active) return;
        LARGE_INTEGER end{};
        if (QueryPerformanceCounter(&end) && end.QuadPart > start_)
            colour_work_detail::frame.remaining_ticks -= end.QuadPart - start_;
    }
    ColourWorkBudget(const ColourWorkBudget&) = delete;
    ColourWorkBudget& operator=(const ColourWorkBudget&) = delete;

    [[nodiscard]] bool expired() const noexcept {
        if (!measured_) return false; // The bounded item limit still applies.
        LARGE_INTEGER current{};
        return QueryPerformanceCounter(&current) && current.QuadPart >= deadline_;
    }
private:
    LONGLONG start_ = 0;
    LONGLONG deadline_ = 0;
    bool measured_ = false;
    bool shared_ = false;
};
} // namespace refunct

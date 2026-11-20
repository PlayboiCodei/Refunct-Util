#pragma once
#include <cstdint>

namespace refunct {
// The native object array is already the authoritative registry. Each feature
// keeps its own validated targets, but can inspect only newly appended slots
// instead of repeating a full search. Reused holes are caught by invalidation
// or the existing feature-specific safety sweep. This class holds no UObject
// references and never reads game memory itself.
class DiscoveryWatermark final {
public:
    int32_t Resume(const void* storage, int32_t count, uint64_t now,
                   uint64_t full_due) noexcept {
        tail_ = false;
        if (!storage || count < 0) return -1;
        if (!storage_ || storage_ != storage || count < count_ || now >= full_due)
            return 0;
        // Newly created MIDs and other engine objects commonly grow the array
        // by a few slots; batch them rather than launching a scan each frame.
        if (count > count_ && now >= next_tail_) {
            tail_ = true;
            return count_;
        }
        return -1;
    }
    template<class Deadline>
    void Complete(const void* storage, int32_t count, uint64_t now,
                  uint64_t interval, Deadline& full_due) noexcept {
        if (!tail_ || !full_due || storage_ != storage || count < count_)
            full_due = now + interval;
        // An appended-slot pass never postpones the safety full scan.
        storage_ = storage;
        count_ = count;
        next_tail_ = now + 1000;
        tail_ = false;
    }
private:
    const void* storage_ = nullptr;
    int32_t count_ = 0;
    uint64_t next_tail_ = 0;
    bool tail_ = false;
};
} // namespace refunct

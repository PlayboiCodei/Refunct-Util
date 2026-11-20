#pragma once
// Portable, versioned PB-ghost track format. No Win32 or Unreal dependencies.
// The 20-Hz track stores immutable transform samples and discrete contact IDs; never game pointers.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace refunct::ghost_data {
constexpr std::uint32_t kLegacyVersion = 1;
constexpr std::uint32_t kPitchVersion = 2;
constexpr std::uint32_t kVersion = 3;
constexpr std::size_t kHeaderSize = 24;
constexpr std::size_t kLegacySampleSize = 20;
constexpr std::size_t kPitchSampleSize = 24;
constexpr std::size_t kSampleSize = 28;
constexpr std::size_t kMaxSamples = 144000; // 2 hours at 20 Hz, <3.9 MiB on disk.
constexpr std::uint32_t kSamplingIntervalMs = 50;

struct Sample {
    std::uint32_t time_ms = 0;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float yaw = 0.0f;
    float pitch = 0.0f; // Vertical look angle; zero for legacy recordings.
    std::uint32_t contact = 0; // High byte: split ordinal; low byte: physical actor (both 1-based).
};

inline std::uint32_t ButtonContactTag(int split, int actor) {
    return split >= 1 && split <= 37 && actor >= 1 && actor <= 37
        ? (static_cast<std::uint32_t>(split) << 8) | static_cast<std::uint32_t>(actor) : 0u;
}
inline bool ValidContactTag(std::uint32_t tag) {
    return !tag || ButtonContactTag(static_cast<int>(tag >> 8),
        static_cast<int>(tag & 255u)) == tag;
}
inline bool Valid(const Sample& value) {
    constexpr float kWorldLimit = 10000000.f;
    return ValidContactTag(value.contact) && std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z) && std::isfinite(value.yaw) &&
           std::isfinite(value.pitch) && std::abs(value.pitch) <= 180.f &&
           std::abs(value.x) < kWorldLimit && std::abs(value.y) < kWorldLimit &&
           std::abs(value.z) < kWorldLimit && std::abs(value.yaw) < kWorldLimit;
}

inline std::uint32_t Hash(const std::uint8_t* data, std::size_t size) {
    std::uint32_t result = 2166136261u; // FNV-1a
    for (std::size_t i = 0; i < size; ++i)
        result = (result ^ data[i]) * 16777619u;
    return result;
}
inline void Put32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
}
inline std::uint32_t Get32(const std::uint8_t* ptr) {
    return std::uint32_t{ptr[0]} | (std::uint32_t{ptr[1]} << 8) |
           (std::uint32_t{ptr[2]} << 16) | (std::uint32_t{ptr[3]} << 24);
}
inline void PutFloat(std::vector<std::uint8_t>& out, float value) {
    static_assert(sizeof(float) == sizeof(std::uint32_t));
    std::uint32_t bits{};
    std::memcpy(&bits, &value, sizeof(bits));
    Put32(out, bits);
}
inline float GetFloat(const std::uint8_t* ptr) {
    const std::uint32_t bits = Get32(ptr);
    float value{};
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}
inline bool ValidTrack(const std::vector<Sample>& samples, std::uint32_t pb_ms) {
    if (samples.size() < 2 || samples.size() > kMaxSamples ||
        pb_ms == 0 || pb_ms > 86400000u || samples[0].time_ms > 500u)
        return false;
    std::uint32_t previous = 0;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const Sample& sample = samples[i];
        if (!Valid(sample) || sample.time_ms > pb_ms + 100u ||
            (i != 0 && sample.time_ms <= previous)) return false;
        previous = sample.time_ms;
    }
    // Do not mistake an incomplete capture for a full run.
    return samples.back().time_ms + 200u >= pb_ms;
}
inline bool Serialize(const std::vector<Sample>& samples, std::uint32_t pb_ms,
                      std::vector<std::uint8_t>& out) {
    if (!ValidTrack(samples, pb_ms)) return false;
    std::vector<std::uint8_t> data;
    data.reserve(kHeaderSize + samples.size() * kSampleSize);
    for (std::uint8_t c : {'R','F','U','G','H','O','S','T'}) data.push_back(c);
    Put32(data, kVersion);
    Put32(data, pb_ms);
    Put32(data, static_cast<std::uint32_t>(samples.size()));
    Put32(data, 0); // Reserved checksum slot.
    for (const Sample& sample : samples) {
        Put32(data, sample.time_ms);
        PutFloat(data, sample.x); PutFloat(data, sample.y);
        PutFloat(data, sample.z); PutFloat(data, sample.yaw);
        PutFloat(data, sample.pitch);
        Put32(data, sample.contact);
    }
    const std::uint32_t hash = Hash(data.data() + kHeaderSize,
                                     data.size() - kHeaderSize);
    for (int i = 0; i < 4; ++i)
        data[20 + i] = static_cast<std::uint8_t>(hash >> (i * 8));
    out = std::move(data);
    return true;
}
inline bool Deserialize(const std::vector<std::uint8_t>& data,
                        std::uint32_t expected_pb_ms, std::vector<Sample>& out) {
    out.clear();
    static constexpr std::uint8_t magic[] = {'R','F','U','G','H','O','S','T'};
    if (data.size() < kHeaderSize ||
        std::memcmp(data.data(), magic, sizeof(magic)) != 0) return false;
    const auto version = Get32(data.data() + 8);
    if (version != kVersion && version != kPitchVersion && version != kLegacyVersion) return false;
    const std::size_t stride = version == kLegacyVersion ? kLegacySampleSize :
        version == kPitchVersion ? kPitchSampleSize : kSampleSize;
    const std::uint32_t stored_pb = Get32(data.data() + 12);
    const std::uint32_t count = Get32(data.data() + 16);
    if (count < 2 || count > kMaxSamples ||
        (expected_pb_ms > stored_pb ? expected_pb_ms - stored_pb : stored_pb - expected_pb_ms) > 2u ||
        data.size() != kHeaderSize + static_cast<std::size_t>(count) * stride ||
        Get32(data.data() + 20) != Hash(data.data() + kHeaderSize,
                                          data.size() - kHeaderSize)) return false;
    std::vector<Sample> samples;
    samples.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint8_t* row = data.data() + kHeaderSize + i * stride;
        samples.push_back({Get32(row), GetFloat(row + 4), GetFloat(row + 8),
                           GetFloat(row + 12), GetFloat(row + 16),
                           version == kLegacyVersion ? 0.f : GetFloat(row + 20),
                           version == kVersion ? Get32(row + 24) : 0u});
    }
    if (!ValidTrack(samples, stored_pb)) return false;
    out = std::move(samples);
    return true;
}
// Map the player's time since a native button contact onto the corresponding
// interval of the recorded PB. The ghost stops at the next PB button until the
// player presses it. This uses existing PB splits, not new ghost-file metadata.
inline bool ButtonSegmentPlaybackTime(double run_elapsed_seconds,
                                      double pressed_run_seconds,
                                      double pb_start_seconds,
                                      double pb_end_seconds,
                                      std::uint32_t pb_total_ms,
                                      std::uint32_t& playback_ms) {
    if (!std::isfinite(run_elapsed_seconds) || !std::isfinite(pressed_run_seconds) ||
        !std::isfinite(pb_start_seconds) || !std::isfinite(pb_end_seconds) ||
        run_elapsed_seconds < 0.0 || run_elapsed_seconds > 86400.0 ||
        pressed_run_seconds < 0.0 || pressed_run_seconds > run_elapsed_seconds + 0.050 ||
        pb_start_seconds < 0.0 || pb_end_seconds <= pb_start_seconds ||
        pb_total_ms == 0 || pb_total_ms > 86400000u ||
        pb_end_seconds * 1000.0 > static_cast<double>(pb_total_ms) + 100.0)
        return false;
    const auto start_ms = static_cast<std::uint32_t>(std::llround(pb_start_seconds * 1000.0));
    const auto end_ms = static_cast<std::uint32_t>(std::llround(pb_end_seconds * 1000.0));
    if (end_ms <= start_ms || end_ms > pb_total_ms + 100u) return false;
    // Clock jitter at the contact can be a few milliseconds; never travel
    // backwards before the segment's start.
    const double since_contact_ms = std::max(0.0, run_elapsed_seconds - pressed_run_seconds) * 1000.0;
    const auto elapsed_ms = static_cast<std::uint32_t>(std::llround(since_contact_ms));
    playback_ms = static_cast<std::uint32_t>(std::min<std::uint64_t>(
        end_ms, static_cast<std::uint64_t>(start_ms) + elapsed_ms));
    return true;
}

inline bool PositionAt(const std::vector<Sample>& samples, std::uint32_t elapsed_ms,
                       Sample& out) {
    if (samples.empty()) return false;
    const auto it = std::lower_bound(samples.begin(), samples.end(), elapsed_ms,
        [](const Sample& sample, std::uint32_t time) { return sample.time_ms < time; });
    if (it == samples.begin()) { out = *it; return true; }
    if (it == samples.end()) { out = samples.back(); return true; }
    if (it->time_ms == elapsed_ms) { out = *it; return true; }
    const Sample& after = *it;
    const Sample& before = *(it - 1);
    const float dt = static_cast<float>(after.time_ms - before.time_ms);
    if (dt <= 0.0f) return false;
    const float dx = after.x - before.x, dy = after.y - before.y, dz = after.z - before.z;
    // Teleport/checkpoint jumps are discontinuities, not a flight path.
    if (static_cast<double>(dx)*dx + static_cast<double>(dy)*dy +
        static_cast<double>(dz)*dz > 2500.0 * 2500.0) {
        out = elapsed_ms < after.time_ms ? before : after;
        return true;
    }
    const float t = static_cast<float>(elapsed_ms - before.time_ms) / dt;
    const float yaw_delta = std::remainder(after.yaw - before.yaw, 360.0f);
    const float pitch_delta = std::remainder(after.pitch - before.pitch, 360.f);
    out = {elapsed_ms, before.x + dx*t, before.y + dy*t, before.z + dz*t,
           before.yaw + yaw_delta*t,
           std::remainder(before.pitch + pitch_delta*t, 360.f)};
    return true;
}
// Contacts are discrete events, never inferred from interpolated transforms.
// Split ordinal disambiguates neighbouring contacts. Small timestamp tolerance
// accommodates rounding of split/gold durations when loaded from records.ini.
inline std::uint32_t ContactTagAt(const std::vector<Sample>& track,
                                 std::uint32_t at_ms, int split) {
    constexpr std::uint32_t tolerance = 100;
    const auto low = at_ms > tolerance ? at_ms - tolerance : 0u;
    const auto it = std::lower_bound(track.begin(), track.end(), low,
        [](const Sample& point, std::uint32_t time) { return point.time_ms < time; });
    for (auto p = it; p != track.end() && p->time_ms <= at_ms + tolerance; ++p)
        if (p->contact && ValidContactTag(p->contact) &&
            static_cast<int>(p->contact >> 8) == split) return p->contact;
    return 0;
}
// Create an independent replay from a measured button interval. A partial
// run is sufficient: the source need not reach the finish line. Preserve
// exact contact times at both ends by interpolating the 20-Hz capture.
inline bool ExtractSegment(const std::vector<Sample>& source,
                           std::uint32_t start_ms, std::uint32_t end_ms,
                           std::vector<Sample>& output) {
    output.clear();
    if (source.size() < 2 || end_ms <= start_ms + 49u ||
        end_ms > 86400000u || source.size() > kMaxSamples) return false;
    // A missing boundary is not evidence of a recorded movement path. Avoid
    // extrapolation across a late capture or a dropped/paused recorder.
    if (source.front().time_ms > start_ms + 75u ||
        source.back().time_ms + 75u < end_ms) return false;
    const auto lower = std::lower_bound(source.begin(), source.end(), start_ms,
        [](const Sample& point, std::uint32_t when) { return point.time_ms < when; });
    if (lower == source.end()) return false;
    const auto upper = std::lower_bound(source.begin(), source.end(), end_ms,
        [](const Sample& point, std::uint32_t when) { return point.time_ms < when; });
    if (upper == source.end() && source.back().time_ms + 75u < end_ms) return false;
    Sample first{}, last{};
    if (!PositionAt(source, start_ms, first) ||
        !PositionAt(source, end_ms, last)) return false;
    const std::uint32_t duration = end_ms - start_ms;
    output.reserve(static_cast<std::size_t>(upper - lower) + 2u);
    first.contact = 0; // A prior segment contact is not part of this segment.
    first.time_ms = 0;
    output.push_back(first);
    for (auto it = lower; it != upper; ++it) {
        if (it->time_ms > start_ms && it->time_ms < end_ms) {
            Sample sample = *it;
            sample.time_ms -= start_ms;
            output.push_back(sample);
        }
    }
    if (last.time_ms != end_ms) last.contact = 0;
    last.time_ms = duration;
    output.push_back(last);
    if (!ValidTrack(output, duration)) {
        output.clear();
        return false;
    }
    return true;
}
} // namespace refunct::ghost_data

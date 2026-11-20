#pragma once
// Independent, versioned furthest-run replay with native button contact times.
// Only completed contact boundaries are saved; an interrupted run is valid.
#include "refunct/ghost_data.hpp"
#include <array>

namespace refunct::ghost_furthest_data {
template <std::size_t Buttons>
struct Run {
    std::uint32_t reached = 0;
    std::array<std::uint32_t, Buttons> button_ms{};
    std::vector<ghost_data::Sample> samples;
};
constexpr std::size_t kPrefixSize = 24;
constexpr std::uint32_t kVersion = 1;

// An attached game can deliver its first sample within the permitted first
// 500ms rather than exactly at time zero. Preserve the observed start time
// instead of rejecting the run or fabricating earlier movement.
inline bool ExtractObservedPrefix(const std::vector<ghost_data::Sample>& source,
                                  std::uint32_t end_ms,
                                  std::vector<ghost_data::Sample>& output) {
    output.clear();
    if (source.size() < 2 || source.size() > ghost_data::kMaxSamples ||
        source.front().time_ms > 500 ||
        end_ms <= source.front().time_ms + 49u ||
        source.back().time_ms + 75u < end_ms) return false;
    const auto end = std::lower_bound(source.begin(), source.end(), end_ms,
        [](const ghost_data::Sample& sample, std::uint32_t when) {
            return sample.time_ms < when;
        });
    output.assign(source.begin(), end);
    if (end != source.end() && end->time_ms == end_ms)
        output.push_back(*end);
    else {
        ghost_data::Sample at_contact{};
        if (!ghost_data::PositionAt(source, end_ms, at_contact)) return false;
        at_contact.time_ms = end_ms;
        output.push_back(at_contact);
    }
    if (!ghost_data::ValidTrack(output, end_ms)) {
        output.clear();
        return false;
    }
    return true;
}

template <std::size_t Buttons>
bool Valid(const Run<Buttons>& run) {
    if (run.reached < 1 || run.reached > Buttons) return false;
    std::uint32_t previous = 0;
    for (std::size_t i = 0; i < run.reached; ++i) {
        const auto next = run.button_ms[i];
        if (next <= previous || next > 86400000u) return false;
        previous = next;
    }
    for (std::size_t i = run.reached; i < Buttons; ++i)
        if (run.button_ms[i]) return false;
    return ghost_data::ValidTrack(run.samples, previous);
}

template <std::size_t Buttons>
bool Serialize(const Run<Buttons>& run, std::vector<std::uint8_t>& out) {
    if (!Valid(run)) return false;
    std::vector<std::uint8_t> track;
    if (!ghost_data::Serialize(run.samples, run.button_ms[run.reached - 1], track))
        return false;
    std::vector<std::uint8_t> data;
    data.reserve(kPrefixSize + Buttons * sizeof(std::uint32_t) + track.size());
    for (auto byte : {'R','F','U','F','U','R','T','H'})
        data.push_back(static_cast<std::uint8_t>(byte));
    ghost_data::Put32(data, kVersion);
    ghost_data::Put32(data, run.reached);
    ghost_data::Put32(data, static_cast<std::uint32_t>(Buttons));
    ghost_data::Put32(data, 0); // Hash over button times and embedded track.
    for (const auto ms : run.button_ms) ghost_data::Put32(data, ms);
    data.insert(data.end(), track.begin(), track.end());
    const auto hash = ghost_data::Hash(data.data() + kPrefixSize, data.size() - kPrefixSize);
    for (int shift = 0; shift < 4; ++shift)
        data[20 + shift] = static_cast<std::uint8_t>(hash >> (shift * 8));
    out = std::move(data);
    return true;
}

template <std::size_t Buttons>
bool Deserialize(const std::vector<std::uint8_t>& data, Run<Buttons>& out) {
    static constexpr std::uint8_t magic[8] = {'R','F','U','F','U','R','T','H'};
    if (data.size() < kPrefixSize + Buttons * 4 + ghost_data::kHeaderSize ||
        std::memcmp(data.data(), magic, 8) ||
        ghost_data::Get32(data.data() + 8) != kVersion ||
        ghost_data::Get32(data.data() + 12) < 1 ||
        ghost_data::Get32(data.data() + 12) > Buttons ||
        ghost_data::Get32(data.data() + 16) != Buttons ||
        ghost_data::Get32(data.data() + 20) !=
            ghost_data::Hash(data.data() + kPrefixSize, data.size() - kPrefixSize)) return false;
    Run<Buttons> decoded;
    decoded.reached = ghost_data::Get32(data.data() + 12);
    const std::size_t times_start = kPrefixSize;
    for (std::size_t i = 0; i < Buttons; ++i)
        decoded.button_ms[i] = ghost_data::Get32(data.data() + times_start + i*4);
    const std::size_t track_start = kPrefixSize + Buttons*4;
    std::vector<std::uint8_t> track(data.begin() + track_start, data.end());
    if (!ghost_data::Deserialize(track, decoded.button_ms[decoded.reached-1], decoded.samples) ||
        !Valid(decoded)) return false;
    out = std::move(decoded);
    return true;
}
} // namespace refunct::ghost_furthest_data

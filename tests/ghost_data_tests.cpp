#include "refunct/ghost_data.hpp"
#include "refunct/ghost_furthest_data.hpp"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <limits>
#include <vector>
#include <cstdint>

using namespace refunct::ghost_data;
int main() {
    const std::vector<Sample> run{{0, 0.f, 0.f, 0.f, 350.f, -30.f},
                                  {50, 50.f, 10.f, 4.f, 10.f, 10.f},
                                  {100, 100.f, 20.f, 8.f, 30.f, 40.f}};
    assert(ValidTrack(run, 100));
    std::vector<std::uint8_t> bytes;
    assert(Serialize(run, 100, bytes));
    assert(bytes.size() == kHeaderSize + 3*kSampleSize);
    std::vector<Sample> decoded;
    assert(Deserialize(bytes, 100, decoded) && decoded.size() == run.size());
    assert(decoded[1].x == 50.f && decoded[1].yaw == 10.f && decoded[1].pitch == 10.f);
    Sample current{};
    assert(PositionAt(decoded, 25, current));
    assert(current.x == 25.f && std::abs(current.yaw - 360.f) < 0.001f);
    assert(std::abs(current.pitch + 10.f) < 0.001f);
    assert(PositionAt(decoded, 0, current) && current.x == 0.f);
    assert(PositionAt(decoded, 9999, current) && current.x == 100.f);
    // v3 keeps the confirmed button identity across full and sliced tracks.
    auto tagged = run;
    tagged[1].contact = ButtonContactTag(7, 8);
    tagged[2].contact = ButtonContactTag(8, 7);
    std::vector<std::uint8_t> tagged_bytes;
    assert(Serialize(tagged, 100, tagged_bytes));
    assert(Deserialize(tagged_bytes, 100, decoded));
    assert(ContactTagAt(decoded, 50, 7) == ButtonContactTag(7, 8));
    assert(ContactTagAt(decoded, 51, 8) == ButtonContactTag(8, 7));
    assert(ContactTagAt(decoded, 50, 9) == 0);
    assert(PositionAt(decoded, 50, current) && current.contact == ButtonContactTag(7,8));
    assert(PositionAt(decoded, 25, current) && current.contact == 0);
    std::vector<Sample> tagged_segment;
    assert(ExtractSegment(decoded, 50, 100, tagged_segment));
    assert(tagged_segment.front().contact == 0);
    assert(tagged_segment.back().contact == ButtonContactTag(8,7));
    tagged[1].contact = 0xffffu;
    assert(!Serialize(tagged, 100, tagged_bytes));
    // Actual v2 track bytes: pitch but no actor identity.
    std::vector<std::uint8_t> v2(bytes.begin(), bytes.begin() + kHeaderSize);
    v2[8]=2;
    for (const auto& sample : run) {
        Put32(v2, sample.time_ms);
        PutFloat(v2, sample.x); PutFloat(v2, sample.y);
        PutFloat(v2, sample.z); PutFloat(v2, sample.yaw); PutFloat(v2, sample.pitch);
    }
    const auto v2_hash=Hash(v2.data()+kHeaderSize,v2.size()-kHeaderSize);
    for(int i=0;i<4;++i) v2[20+i]=static_cast<std::uint8_t>(v2_hash>>(8*i));
    assert(Deserialize(v2,100,decoded));
    assert(decoded[1].pitch==10.f && decoded[1].contact==0);
    // Build an actual v1 byte stream without pitch, independent of the writer.
    std::vector<std::uint8_t> legacy(bytes.begin(), bytes.begin() + kHeaderSize);
    legacy[8] = 1;
    for (const auto& sample : run) {
        Put32(legacy, sample.time_ms);
        PutFloat(legacy, sample.x); PutFloat(legacy, sample.y);
        PutFloat(legacy, sample.z); PutFloat(legacy, sample.yaw);
    }
    const auto legacy_hash = Hash(legacy.data() + kHeaderSize, legacy.size() - kHeaderSize);
    for (int i = 0; i < 4; ++i) legacy[20+i] = static_cast<std::uint8_t>(legacy_hash >> (8*i));
    assert(Deserialize(legacy, 100, decoded));
    assert(decoded[1].pitch == 0.f && decoded[1].yaw == 10.f);
    assert(PositionAt(decoded, 25, current) && current.pitch == 0.f);
    // Furthest-run files embed the same versioned track, including old v1 tracks.
    refunct::ghost_furthest_data::Run<2> furthest{1, {100, 0}, run}, restored;
    std::vector<std::uint8_t> outer;
    assert(refunct::ghost_furthest_data::Serialize(furthest, outer));
    assert(refunct::ghost_furthest_data::Deserialize(outer, restored));
    assert(restored.samples.back().pitch == 40.f);
    constexpr auto prefix = refunct::ghost_furthest_data::kPrefixSize + 2*4;
    outer.resize(prefix);
    outer.insert(outer.end(), legacy.begin(), legacy.end());
    const auto outer_hash = Hash(outer.data() + 24, outer.size() - 24);
    for (int i = 0; i < 4; ++i) outer[20+i] = static_cast<std::uint8_t>(outer_hash >> (8*i));
    assert(refunct::ghost_furthest_data::Deserialize(outer, restored));
    assert(restored.samples.back().pitch == 0.f);
    assert(!Valid({0,0,0,0,0,std::numeric_limits<float>::quiet_NaN()}));
    assert(!Valid({0,0,0,0,0,181.f}));
    assert(PositionAt({{0,0,0,0,0,170.f},{100,0,0,0,0,-170.f}}, 50, current));
    assert(std::abs(current.pitch) == 180.f);
    // Profile PB changed (or the file is manually edited): do not play stale data.
    assert(!Deserialize(bytes, 200, decoded));
    assert(decoded.empty());
    std::vector<std::uint8_t> corrupted = bytes;
    corrupted.back() ^= 0x01;
    assert(!Deserialize(corrupted, 100, decoded));
    corrupted = bytes;
    corrupted[8] = 0xff; // unknown format version
    assert(!Deserialize(corrupted, 100, decoded));
    corrupted = bytes;
    corrupted[16] = 0xff; // oversized sample count
    assert(!Deserialize(corrupted, 100, decoded));
    assert(!ValidTrack({{0, 0.f,0.f,0.f,0.f}, {0,0.f,0.f,0.f,0.f}}, 100));
    assert(!ValidTrack({{0,0.f,0.f,0.f,0.f}, {50,0.f,0.f,0.f,0.f}}, 500));
    assert(!ValidTrack({{0,0.f,0.f,0.f,0.f}, {100,std::numeric_limits<float>::infinity(),0.f,0.f,0.f}},100));
    // Teleports are discontinuous; do not draw a streak through the level.
    std::vector<Sample> teleport{{0, 0.f,0.f,0.f,0.f}, {100, 10000.f,0.f,0.f,0.f}};
    assert(PositionAt(teleport, 50, current) && current.x == 0.f);
    assert(PositionAt(teleport, 100, current) && current.x == 10000.f);
    // Native button contacts rebase each PB ghost segment independently.
    std::uint32_t t = 999;
    assert(ButtonSegmentPlaybackTime(12.0, 12.0, 8.0, 11.0, 40000, t) && t == 8000);
    assert(ButtonSegmentPlaybackTime(13.0, 12.0, 8.0, 11.0, 40000, t) && t == 9000);
    assert(ButtonSegmentPlaybackTime(40.0, 12.0, 8.0, 11.0, 40000, t) && t == 11000);
    // The following button starts exactly at its PB split time.
    assert(ButtonSegmentPlaybackTime(40.0, 40.0, 11.0, 14.0, 40000, t) && t == 11000);
    assert(ButtonSegmentPlaybackTime(40.6, 40.0, 11.0, 14.0, 40000, t) && t == 11600);
    // Start-of-run segment begins at zero; existing ghost files are unchanged.
    assert(ButtonSegmentPlaybackTime(1.25, 0.0, 0.0, 2.0, 40000, t) && t == 1250);
    assert(!ButtonSegmentPlaybackTime(2.0, -1.0, 2.0, 3.0, 40000, t));
    assert(!ButtonSegmentPlaybackTime(2.0, 1.0, -1.0, 3.0, 40000, t));
    assert(!ButtonSegmentPlaybackTime(2.0, 1.0, 4.0, 3.0, 40000, t));
    assert(!ButtonSegmentPlaybackTime(2.0, 3.0, 1.0, 2.0, 40000, t));
    assert(!ButtonSegmentPlaybackTime(2.0, 0.0, 1.0, 50.0, 40000, t));
    assert(!ButtonSegmentPlaybackTime(std::numeric_limits<double>::quiet_NaN(),
                                     0.0, 1.0, 2.0, 40000, t));
    // Independent fastest-button replays work without a completed run PB.
    std::vector<Sample> incomplete;
    for (std::uint32_t ms = 0; ms <= 6000; ms += 50)
        incomplete.push_back({ms, ms * .02f, 0.f, 20.f, 0.f, ms*.01f});
    std::vector<Sample> segment;
    assert(ExtractSegment(incomplete, 0, 2133, segment));
    assert(segment.front().time_ms == 0 && segment.back().time_ms == 2133);
    assert(std::abs(segment.back().x - 42.66f) < .1f);
    assert(std::abs(segment.back().pitch - 21.33f) < .001f);
    assert(Serialize(segment, 2133, bytes));
    assert(Deserialize(bytes, 2133, decoded));
    assert(std::abs(decoded.back().pitch - 21.33f) < .001f);
    assert(ExtractSegment(incomplete, 2133, 5081, segment));
    assert(segment.front().time_ms == 0 && segment.back().time_ms == 2948);
    assert(std::abs(segment.front().x - 42.66f) < .1f);
    assert(std::abs(segment.back().x - 101.62f) < .1f);
    // An absent source interval cannot be fabricated from split times.
    assert(!ExtractSegment(incomplete, 5900, 7000, segment));
    assert(!ExtractSegment(incomplete, 100, 100, segment));
    return 0;
}

#include "refunct/pure.hpp"
#include <algorithm>
#include <cmath>
#include <cerrno>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <iterator>
#include <utility>
namespace refunct {
bool ValidProfileName(const std::wstring& name) {
    if (name.empty() || name.size() > 32 || name.front() == L' ' || name.back() == L' ')
        return false;
    for (wchar_t ch : name) {
        if ((ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') ||
            (ch >= L'0' && ch <= L'9') || ch == L' ' || ch == L'-' ||
            ch == L'_' || ch == L'%' || ch == L'(' || ch == L')') continue;
        return false;
    }
    std::wstring upper = name;
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towupper(ch)); });
    for (const wchar_t* reserved : {L"CON", L"PRN", L"AUX", L"NUL",
                                      L"COM1", L"COM2", L"COM3", L"COM4", L"COM5",
                                       L"COM6", L"COM7", L"COM8", L"COM9", L"LPT1",
                                       L"LPT2", L"LPT3", L"LPT4", L"LPT5", L"LPT6",
                                       L"LPT7", L"LPT8", L"LPT9"}) {
        if (upper == reserved) return false;
    }
    return true;
}


namespace {
std::wstring FormatMilliseconds(std::uint64_t ms) {
    wchar_t buffer[64]{};
    std::swprintf(buffer, std::size(buffer), L"%02llu:%02llu.%03llu",
               static_cast<unsigned long long>(ms / 60000),
               static_cast<unsigned long long>((ms / 1000) % 60),
               static_cast<unsigned long long>(ms % 1000));
    return buffer;
}
} // namespace

std::wstring FormatSplitTime(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0 || seconds > 24.0 * 3600.0)
        return L"--:--.---";
    // Round at the same millisecond precision used by signed split deltas.
    // Truncation produced an off-by-1ms value for some binary floats and
    // changed saved PB references on a write/read round trip.
    return FormatMilliseconds(static_cast<std::uint64_t>(std::llround(seconds * 1000.0)));
}

SplitDeltaDisplay FormatSplitDelta(double seconds, bool include_time) {
    if (!std::isfinite(seconds) || std::abs(seconds) > 24.0 * 3600.0)
        return {L"--:--.---", DeltaDirection::Invalid};

    // Round signed differences to the precision actually printed on screen.
    const auto magnitude_ms = static_cast<std::uint64_t>(std::llround(std::abs(seconds) * 1000.0));
    if (magnitude_ms == 0)
        return {include_time ? FormatMilliseconds(0) : L"=", DeltaDirection::Even};

    const bool ahead = seconds < 0.0;
    std::wstring text = ahead ? L"-" : L"+";
    if (include_time) text += FormatMilliseconds(magnitude_ms);
    return {std::move(text), ahead ? DeltaDirection::Ahead : DeltaDirection::Behind};
}

// Never carry split history across a new run or a backwards-reset button count.
// Input accepts mm:ss.mmm or plain seconds (e.g. 32.125). Empty values mean
// "not entered". Invalid manual values are ignored, never interpreted as zero.
bool ParsePBTime(std::wstring value, double& out) {
    const auto first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return false;
    const auto last = value.find_last_not_of(L" \t\r\n");
    value = value.substr(first, last - first + 1);
    auto parse_number = [](const std::wstring& src, double& result) {
        if (src.empty() || src.front() == L'-') return false;
        wchar_t* end = nullptr;
        errno = 0;
        result = std::wcstod(src.c_str(), &end);
        return errno != ERANGE && end != src.c_str() && *end == 0 &&
               std::isfinite(result) && result >= 0.0;
    };
    const auto colon = value.find(L':');
    if (colon == std::wstring::npos) {
        if (!parse_number(value, out)) return false;
    } else {
        if (value.find(L':', colon + 1) != std::wstring::npos) return false;
        double minutes = 0.0, seconds = 0.0;
        if (!parse_number(value.substr(0, colon), minutes) ||
            !parse_number(value.substr(colon + 1), seconds) ||
            std::floor(minutes) != minutes || seconds >= 60.0) return false;
        out = minutes * 60.0 + seconds;
    }
    return out <= 24.0 * 3600.0;
}

double ProfileReferenceTimeAtButton(
    const std::array<double, kComparisonButtonCount>& pb_splits,
    const std::array<double, kComparisonButtonCount>& best_segments,
    int button, ProfileCompareMode mode) noexcept {
    if (button < 1 || button > static_cast<int>(kComparisonButtonCount)) return -1.0;
    if (mode == ProfileCompareMode::PersonalBest) {
        const double value = pb_splits[static_cast<size_t>(button - 1)];
        return std::isfinite(value) && value >= 0.0 ? value : -1.0;
    }
    if (mode != ProfileCompareMode::BestSegments) return -1.0;
    double sum = 0.0;
    for (int i = 0; i < button; ++i) {
        const double value = best_segments[static_cast<size_t>(i)];
        // Zero or missing best segment cannot establish a meaningful reference.
        if (!std::isfinite(value) || value <= 0.0) return -1.0;
        sum += value;
    }
    return std::isfinite(sum) && sum <= 86400.0 ? sum : -1.0;
}

double ProfileRecordedTimeAtButton(
    const std::array<double, kComparisonButtonCount>& pb_splits,
    const std::array<double, kComparisonButtonCount>& best_segments,
    int button, ProfileCompareMode source, RecordTimeView view) noexcept {
    if (button < 1 || button > static_cast<int>(kComparisonButtonCount)) return -1.0;
    if (view == RecordTimeView::Total)
        return ProfileReferenceTimeAtButton(pb_splits, best_segments, button, source);
    if (view != RecordTimeView::Segment) return -1.0;

    const size_t index = static_cast<size_t>(button - 1);
    if (source == ProfileCompareMode::BestSegments) {
        const double value = best_segments[index];
        return std::isfinite(value) && value > 0.0 && value <= 86400.0 ? value : -1.0;
    }
    if (source != ProfileCompareMode::PersonalBest) return -1.0;
    const double current = pb_splits[index];
    if (!std::isfinite(current) || current < 0.0 || current > 86400.0) return -1.0;
    if (button == 1) return current;
    const double previous = pb_splits[index - 1];
    if (!std::isfinite(previous) || previous < 0.0 || previous > current) return -1.0;
    return current - previous;
}

std::wstring PBKey(int button) {
    wchar_t result[32]{};
    std::swprintf(result, std::size(result), L"Button%02d", button);
    return result;
}

// A temporary file + atomic replacement avoids leaving half-written PBs if
// the game stops while saving. Only ASCII is written (compatible with INI APIs).

} // namespace refunct

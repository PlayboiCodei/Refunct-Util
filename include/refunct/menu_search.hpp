#pragma once
// Portable, case-insensitive whole-word-token search across menu paths and
// labels. Input text is short and is only reindexed on actual key presses.
#include <cwctype>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace refunct {

class MenuSearchTerms final {
public:
    explicit MenuSearchTerms(std::wstring_view query) {
        std::wstring current;
        for (const wchar_t ch : query) {
            if (std::iswspace(ch)) {
                if (!current.empty()) {
                    terms_.push_back(std::move(current));
                    current.clear();
                }
            } else {
                current.push_back(static_cast<wchar_t>(std::towlower(ch)));
            }
        }
        if (!current.empty()) terms_.push_back(std::move(current));
    }

    [[nodiscard]] bool empty() const noexcept { return terms_.empty(); }

    [[nodiscard]] bool matches(std::wstring_view path,
                               std::wstring_view label) const {
        if (terms_.empty()) return false;
        std::wstring haystack;
        haystack.reserve(path.size() + label.size() + 1);
        haystack.append(path);
        haystack.push_back(L' ');
        haystack.append(label);
        for (wchar_t& ch : haystack)
            ch = static_cast<wchar_t>(std::towlower(ch));
        for (const auto& term : terms_) {
            if (haystack.find(term) == std::wstring::npos) return false;
        }
        return true;
    }

private:
    std::vector<std::wstring> terms_;
};

} // namespace refunct

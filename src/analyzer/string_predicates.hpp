#pragma once

#include <string_view>

namespace logsearch::analyzer {

//! Returns true if the content of the string_view can be ASCII-encoded, otherwise false.
constexpr bool IsAscii(const std::string_view sv) noexcept {
    constexpr unsigned char HIGH_BIT = 0x80;
    // Uses branchless OR-reduce to allow for auto-vectorization.
    // Tradeoff: Needs to scan the entire input and cannot exit early when we find a Unicode character. But we expect
    // the input to be ASCII-only most of the time, hence worth it.
    unsigned char acc = 0;
    for (const unsigned char c : sv) {
        acc |= c;
    }
    return (acc & HIGH_BIT) == 0;
}

//! Returns true if the string_view does not contain any ASCII uppercase characters.
constexpr bool IsLowercase(const std::string_view sv) noexcept {
    // NOLINTNEXTLINE(readability-use-anyofallof) — std::all_of is not constexpr in C++17.
    for (const unsigned char c : sv) {
        if (c >= 'A' && c <= 'Z') {
            return false;
        }
    }
    return true;
}

} // namespace logsearch::analyzer

#ifndef ANALYZER_UNITY_BUILD
#error "stopword_filter.cpp must be compiled as part of the Analyzer unity build"
#endif

#include "stopword_filter.hpp"

#include <array>
#include <cstdint>
#include <string_view>

namespace logsearch::analyzer {

namespace {

//! Pack a string with size <= 8 bytes into a uint64. Distinct strings of any length <= 8 pack to distinct values.
constexpr std::uint64_t Pack(const std::string_view sv) noexcept {
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < sv.size(); ++i) {
        constexpr std::size_t BITS_PER_BYTE = 8;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
        v |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(sv[i])) << (i * BITS_PER_BYTE);
    }
    return v;
}

//! Common English stopwords, packed to uint64
constexpr std::array<std::uint64_t, 33> STOPWORDS_PACKED = {{
    Pack("a"),   Pack("an"),   Pack("and"),  Pack("are"),  Pack("as"),  Pack("at"),   Pack("be"),
    Pack("but"), Pack("by"),   Pack("for"),  Pack("had"),  Pack("has"), Pack("have"), Pack("her"),
    Pack("his"), Pack("i"),    Pack("if"),   Pack("in"),   Pack("is"),  Pack("it"),   Pack("not"),
    Pack("of"),  Pack("on"),   Pack("or"),   Pack("that"), Pack("the"), Pack("this"), Pack("to"),
    Pack("was"), Pack("were"), Pack("will"), Pack("with"), Pack("you"),
}};

template <std::size_t N>
constexpr bool HasNoDuplicates(const std::array<std::uint64_t, N>& arr) noexcept {
    for (std::size_t i = 0; i < N; i++) {
        for (std::size_t j = i + 1; j < N; j++) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index,cppcoreguidelines-pro-bounds-avoid-unchecked-container-access)
            if (arr[i] == arr[j]) {
                return false;
            }
        }
    }
    return true;
}
static_assert(HasNoDuplicates(STOPWORDS_PACKED), "STOPWORDS_PACKED contains a duplicate entry.");

} // namespace

namespace detail {

bool IsStopword(const std::string_view token) noexcept [[clang::nonblocking]] {
    if (token.size() > sizeof(std::uint64_t)) {
        // We do not support stopwords with more than 8 characters yet.
        return false;
    }

    const std::uint64_t packed = Pack(token);
    // OR-reduce over the packed array. Compiler will loop-unroll or auto-vectorize.
    bool found = false;
    for (const std::uint64_t sw : STOPWORDS_PACKED) {
        found |= (sw == packed);
    }
    return found;
}

} // namespace detail

} // namespace logsearch::analyzer

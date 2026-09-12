#ifndef ANALYZER_UNITY_BUILD
#error "lowercaser.cpp must be compiled as part of the Analyzer unity build"
#endif

#include "lowercaser.hpp"

namespace logsearch::analyzer::detail {

void LowercaseAsciiInPlace(char* data, const std::size_t length) noexcept LS_NONBLOCKING {
    for (std::size_t i = 0; i < length; ++i) {
        // Keep code inside of loop simple for auto-vectorization
        const char c = data[i];
        if (c >= 'A' && c <= 'Z') {
            // Delta between uppercase and lowercase ASCII is 0x20.
            constexpr char ASCII_CASE_DELTA = 0x20;
            data[i] += ASCII_CASE_DELTA;
        }
    }
}

} // namespace logsearch::analyzer::detail

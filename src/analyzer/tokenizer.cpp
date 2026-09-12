#ifndef ANALYZER_UNITY_BUILD
#error "tokenizer.cpp must be compiled as part of the Analyzer unity build"
#endif

#include "tokenizer.hpp"

#include "mutable_span.hpp"

#include <cstddef>
#include <optional>
#include <string_view>

namespace logsearch::analyzer {

namespace {

// The ASCII whitespace characters are the space plus the contiguous 0x09-0x0D block.
constexpr bool IsAsciiWhitespace(const unsigned char c) noexcept {
    return c == ' ' || (c >= '\t' && c <= '\r');
}

} // namespace

namespace detail {

std::optional<MutableSpan> FindNextToken(MutableSpan doc, std::size_t& pos) noexcept LS_NONBLOCKING {
    const std::string_view view = doc.view();

    std::size_t start = pos;
    while (start < view.size() && IsAsciiWhitespace(view[start])) {
        start++;
    }
    if (start >= view.size()) {
        return std::nullopt;
    }
    std::size_t end = start;
    while (end < view.size() && !IsAsciiWhitespace(view[end])) {
        end++;
    }

    // Set out parameter for the next call
    pos = end;

    return MutableSpan(doc.data() + start, end - start);
}

} // namespace detail

} // namespace logsearch::analyzer

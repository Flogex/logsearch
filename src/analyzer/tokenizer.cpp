#ifndef ANALYZER_UNITY_BUILD
#error "tokenizer.cpp must be compiled as part of the Analyzer unity build"
#endif

#include "tokenizer.hpp"

#include "mutable_span.hpp"

#include <cstddef>
#include <optional>
#include <string_view>

namespace logsearch::analyzer::detail {

std::optional<MutableSpan> FindNextToken(MutableSpan doc, std::size_t& pos) noexcept [[clang::nonblocking]] {
    constexpr std::string_view WHITESPACE = " \t\n\r\f\v";
    const std::string_view view = doc.view();

    const std::size_t start = view.find_first_not_of(WHITESPACE, pos);
    if (start == std::string_view::npos) {
        return std::nullopt;
    }
    std::size_t end = view.find_first_of(WHITESPACE, start);
    if (end == std::string_view::npos) {
        end = view.size();
    }

    // Set out parameter for the next call
    pos = end;

    return MutableSpan(doc.data() + start, end - start);
}

} // namespace logsearch::analyzer::detail

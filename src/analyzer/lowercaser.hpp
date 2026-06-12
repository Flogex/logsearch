#pragma once

#include "mutable_span.hpp"

#include <cstddef>

namespace logsearch::analyzer {

namespace detail {
void LowercaseAsciiInPlace(char* data, std::size_t length) noexcept [[clang::nonblocking]];
} // namespace detail

template <typename Downstream>
class Lowercaser final {
    Downstream& downstream_;

public:
    explicit Lowercaser(Downstream& downstream) noexcept : downstream_(downstream) {
    }

    //! Lowercase ASCII characters in `doc` in place, then forward to Downstream.
    // cppcheck-suppress passedByValue ; MutableSpan is trivially copyable, by-value is intended.
    void ProcessDocument(MutableSpan doc) {
        detail::LowercaseAsciiInPlace(doc.data(), doc.size());
        downstream_.ProcessDocument(doc);
    }
};

} // namespace logsearch::analyzer

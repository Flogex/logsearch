#pragma once

#include "mutable_span.hpp"
#include "string_predicates.hpp"

#include <cstddef>
#include <duckdb/common/assert.hpp>
#include <optional>
#include <string_view>

namespace logsearch::analyzer {

namespace detail {
// Finds the next whitespace-delimited token in `doc`, starting from `pos`.
// `pos` is an in/out cursor and gets advanced by this function to the index where the next search should start.
// Returns a MutableSpan that points to the buffer of `doc`.
std::optional<MutableSpan> FindNextToken(MutableSpan doc, std::size_t& pos) noexcept [[clang::nonblocking]];
} // namespace detail

template <typename Downstream>
class Tokenizer final {
    Downstream& downstream_;

public:
    explicit Tokenizer(Downstream& downstream) noexcept : downstream_(downstream) {
    }

    //! Split `doc` on ASCII whitespace runs, dropping empty tokens.
    //! Emits zero-headroom MutableSpans (capacity = size) — slices of `doc`'s
    //! buffer. Downstream stages that need temporary grow must copy to scratch.
    // cppcheck-suppress passedByValue ; MutableSpan is trivially copyable, by-value is intended.
    void ProcessDocument(MutableSpan doc) {
        // Asserts can throw and are therefore outside the kernel
        D_ASSERT(IsAscii(doc.view()));
        D_ASSERT(IsLowercase(doc.view()));

        std::size_t start_pos = 0; // Passed by reference and modified by FindNextToken
        while (auto tok = detail::FindNextToken(doc, start_pos)) {
            downstream_.PushToken(*tok);
        }
    }
};

} // namespace logsearch::analyzer

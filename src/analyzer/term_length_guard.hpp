#pragma once

#include "mutable_span.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>

namespace logsearch::analyzer {

//! Longest term the SSTable dictionary can describe, because `SSTableDictEntry::term_length` is a uint32.
//! Note this is 4 GiB minus one byte: a term of exactly 4 GiB does not fit either.
constexpr std::size_t MAX_TERM_LENGTH = std::numeric_limits<std::uint32_t>::max();

namespace detail {
[[noreturn]] void ThrowTermTooLong(std::size_t length);
} // namespace detail

//! Rejects a term which could not be represented by the on-disk format.
// Belongs last in the chain because other stages can change the length of terms.
template <typename Downstream>
class TermLengthGuard final {
    Downstream& downstream_;

public:
    explicit TermLengthGuard(Downstream& downstream) noexcept : downstream_(downstream) {
    }

    //! Forward `term` to Downstream unless it is too long to index.
    // cppcheck-suppress passedByValue ; MutableSpan is small enough to be copied
    void PushToken(MutableSpan term) {
        // The throw is out of line so that the kernel stays a single comparison.
        if (term.size() > MAX_TERM_LENGTH) {
            detail::ThrowTermTooLong(term.size());
        }
        downstream_.PushToken(term);
    }
};

} // namespace logsearch::analyzer

#pragma once

#include "mutable_span.hpp"
#include "string_predicates.hpp"

#include <duckdb/common/assert.hpp>
#include <string_view>

namespace logsearch::analyzer {

namespace detail {
bool IsStopword(std::string_view token) noexcept [[clang::nonblocking]];
} // namespace detail

// The StopwordFilter might not be useful for the log message workload we are optimizing for because it is different
// from English prose. Maybe we will decide to just remove this stage in the future.
template <typename Downstream>
class StopwordFilter final {
    Downstream& downstream_;

public:
    explicit StopwordFilter(Downstream& downstream) noexcept : downstream_(downstream) {
    }

    //! If and only if `token` is not a stopword, forward it to Downstream.
    // cppcheck-suppress passedByValue ; MutableSpan is small enough to be copied
    void PushToken(MutableSpan token) {
        D_ASSERT(!token.empty());
        D_ASSERT(IsAscii(token.view()));
        D_ASSERT(IsLowercase(token.view()));

        if (!detail::IsStopword(token.view())) {
            downstream_.PushToken(token);
        }
    }
};

} // namespace logsearch::analyzer

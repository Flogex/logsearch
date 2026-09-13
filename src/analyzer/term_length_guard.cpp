#include "term_length_guard.hpp"

#include <cstddef>
#include <cstdint>
#include <duckdb/common/exception.hpp>

namespace logsearch::analyzer::detail {

void ThrowTermTooLong(const std::size_t length) {
    throw duckdb::OutOfRangeException("Cannot index a token of %llu bytes, the limit is %llu bytes",
                                      static_cast<std::uint64_t>(length),
                                      static_cast<std::uint64_t>(MAX_TERM_LENGTH));
}

} // namespace logsearch::analyzer::detail

#pragma once

#include <duckdb/execution/index/index_type.hpp>

namespace logsearch {
class LogsearchIndex {
public:
    static constexpr const char* NAME = "logsearch";

    static duckdb::IndexType GetIndexType();
};
} // namespace logsearch

#pragma once

#include "duckdb/execution/index/index_type.hpp"

class LogsearchIndex {
public:
    static constexpr const char* NAME = "logsearch";

    static duckdb::IndexType GetIndexType();
};

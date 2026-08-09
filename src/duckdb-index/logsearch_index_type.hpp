#pragma once

#include <duckdb/execution/index/index_type.hpp>

namespace logsearch {

//! Registers the Logsearch index type including the callbacks for CREATE INDEX and instantiates from storage.
duckdb::IndexType CreateLogsearchIndexType();

} // namespace logsearch

#pragma once

namespace duckdb {
class ExtensionLoader;
} // namespace duckdb

namespace logsearch::duckdb_index {

//! Registers the Logsearch index type including the callbacks for CREATE INDEX and instantiating from storage.
void RegisterIndexType(duckdb::ExtensionLoader& loader);

} // namespace logsearch::duckdb_index

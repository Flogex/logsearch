#define DUCKDB_EXTENSION_MAIN

#include "logsearch_extension.hpp"

#include "duckdb-index/logsearch_index_type.hpp"

#include <duckdb/execution/index/index_type_set.hpp>
#include <duckdb/main/database.hpp>
#include <duckdb/main/extension/extension_loader.hpp>

namespace {
void load_internal(duckdb::ExtensionLoader& loader) {
    duckdb::IndexTypeSet& index_types = loader.GetDatabaseInstance().config.GetIndexTypes();
    index_types.RegisterIndexType(logsearch::CreateLogsearchIndexType());
}
} // namespace

namespace duckdb {

void LogsearchExtension::Load(ExtensionLoader& loader) {
    load_internal(loader);
}

std::string LogsearchExtension::Name() {
    return "logsearch";
}

std::string LogsearchExtension::Version() const {
    return "0.1";
}

} // namespace duckdb

extern "C" {
DUCKDB_CPP_EXTENSION_ENTRY(logsearch, ext_loader) {
    load_internal(ext_loader);
}
}

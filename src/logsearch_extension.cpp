#define DUCKDB_EXTENSION_MAIN

#include "logsearch_extension.hpp"

#include "duckdb-index/logsearch_index_type.hpp"
#include "scalars/token_predicates.hpp"

#include <duckdb/main/extension/extension_loader.hpp>

namespace {
void load_internal(duckdb::ExtensionLoader& loader) {
    logsearch::duckdb_index::RegisterIndexType(loader);
    logsearch::scalars::RegisterTokenPredicates(loader);
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

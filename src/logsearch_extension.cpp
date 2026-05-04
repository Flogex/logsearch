#define DUCKDB_EXTENSION_MAIN

#include "logsearch_extension.hpp"

#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

namespace {
constexpr const char* kLogsearchVersion = "0.1.0";

void LogsearchVersionFun(DataChunk&, ExpressionState&, Vector& result) {
    result.SetValue(0, Value(kLogsearchVersion));
    result.SetVectorType(VectorType::CONSTANT_VECTOR);
}
} // namespace

void LogsearchExtension::LoadInternal(ExtensionLoader& loader) {
    const auto version_fn = ScalarFunction("logsearch_version", {}, LogicalType::VARCHAR, LogsearchVersionFun);
    loader.RegisterFunction(version_fn);
}

void LogsearchExtension::Load(ExtensionLoader& loader) {
    LoadInternal(loader);
}

std::string LogsearchExtension::Name() {
    return "logsearch";
}

std::string LogsearchExtension::Version() const {
    return kLogsearchVersion;
}

} // namespace duckdb

extern "C" {
DUCKDB_CPP_EXTENSION_ENTRY(logsearch, ext_loader) {
    duckdb::LogsearchExtension::LoadInternal(ext_loader);
}
}

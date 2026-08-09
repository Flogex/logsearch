#ifndef DUCKDB_INDEX_UNITY_BUILD
#error "logsearch_index_builder.cpp must be compiled as part of the DuckDB-Index unity build"
#endif

#include "logsearch_index_builder.hpp"

#include "inverted-index/inverted_index.hpp"

#include <duckdb/common/assert.hpp>
#include <duckdb/common/helper.hpp>
#include <duckdb/storage/buffer_manager.hpp>
#include <duckdb/storage/table_io_manager.hpp>
#include <memory>
#include <string>
#include <utility>

namespace logsearch {

LogsearchIndexBuilder::LogsearchIndexBuilder(
    duckdb::Identifier name, std::string index_type, const duckdb::IndexConstraintType index_constraint_type,
    duckdb::vector<duckdb::column_t> column_ids, duckdb::TableIOManager& table_io_manager,
    const duckdb::vector<duckdb::unique_ptr<duckdb::Expression>>& unbound_expressions, duckdb::AttachedDatabase& db)
    : name_(std::move(name)), index_type_(std::move(index_type)), index_constraint_type_(index_constraint_type),
      column_ids_(std::move(column_ids)), table_io_manager_(table_io_manager),
      unbound_expressions_(unbound_expressions), db_(db) {
}

LogsearchIndexBuilder::~LogsearchIndexBuilder() = default;

LogsearchIndexBuilder::LogsearchIndexBuilder(LogsearchIndexBuilder&&) noexcept = default;

void LogsearchIndexBuilder::SetInvertedIndex(std::unique_ptr<inverted_index::InvertedIndex> index) {
    D_ASSERT(index);
    D_ASSERT(!index_);
    index_ = std::move(index);
}

duckdb::unique_ptr<LogsearchIndex> LogsearchIndexBuilder::Build() {
    if (!index_) {
        // No rows were inserted. Store an empty index.
        index_ = std::make_unique<inverted_index::InvertedIndex>(duckdb::BufferManager::GetBufferManager(db_),
                                                                 table_io_manager_.GetRowGroupSize());
    }
    return duckdb::make_uniq<LogsearchIndex>(name_,
                                             index_type_,
                                             index_constraint_type_,
                                             column_ids_,
                                             table_io_manager_,
                                             unbound_expressions_,
                                             db_,
                                             std::move(index_));
}

} // namespace logsearch

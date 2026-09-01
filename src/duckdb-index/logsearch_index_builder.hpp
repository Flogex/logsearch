#pragma once

#include "logsearch_index.hpp"

#include <duckdb/common/enums/index_constraint_type.hpp>
#include <duckdb/common/identifier.hpp>
#include <duckdb/common/typedefs.hpp>
#include <duckdb/common/unique_ptr.hpp>
#include <duckdb/common/vector.hpp>
#include <memory>
#include <string>

namespace duckdb {
class AttachedDatabase;
class Expression;
class TableIOManager;
} // namespace duckdb

namespace logsearch::inverted_index {
class InvertedIndex;
} // namespace logsearch::inverted_index

namespace logsearch::duckdb_index {

//! Assembles a LogsearchIndex (which inherits from duckdb::BoundIndex), split between taking the BoundIndex parameters
//! and the underlying InvertedIndex, which is only completed later in the index creation process.
// We use the builder pattern to ensure a LogsearchIndex is always fully formed.
class LogsearchIndexBuilder {
public:
    LogsearchIndexBuilder(duckdb::Identifier name, std::string index_type,
                          duckdb::IndexConstraintType index_constraint_type,
                          duckdb::vector<duckdb::column_t> column_ids, duckdb::TableIOManager& table_io_manager,
                          const duckdb::vector<duckdb::unique_ptr<duckdb::Expression>>& unbound_expressions,
                          duckdb::AttachedDatabase& db);

    // Both defined out of line so the unique_ptr member is handled where InvertedIndex is a complete type.
    ~LogsearchIndexBuilder();
    LogsearchIndexBuilder(LogsearchIndexBuilder&&) noexcept;

    // Non-copyable: the builder owns the InvertedIndex until Build hands it to the LogsearchIndex.
    LogsearchIndexBuilder(const LogsearchIndexBuilder&) = delete;
    LogsearchIndexBuilder& operator=(const LogsearchIndexBuilder&) = delete;
    // The reference members make assignment impossible.
    LogsearchIndexBuilder& operator=(LogsearchIndexBuilder&&) = delete;

    //! Hand over the InvertedIndex the build produced. `index` must not be null.
    void SetInvertedIndex(std::unique_ptr<inverted_index::InvertedIndex> index);

    //! Create the bound index. Call once.
    [[nodiscard]] duckdb::unique_ptr<LogsearchIndex> Build();

private:
    duckdb::Identifier name_;
    std::string index_type_;
    duckdb::IndexConstraintType index_constraint_type_;
    // Copied rather than referenced: IndexBuildInitGlobalStateInput dies after build_global_init call.
    duckdb::vector<duckdb::column_t> column_ids_;
    duckdb::TableIOManager& table_io_manager_;
    // Owned by PhysicalCreateIndex, which outlives the whole build.
    const duckdb::vector<duckdb::unique_ptr<duckdb::Expression>>& unbound_expressions_;
    duckdb::AttachedDatabase& db_;
    std::unique_ptr<inverted_index::InvertedIndex> index_;
};

} // namespace logsearch::duckdb_index

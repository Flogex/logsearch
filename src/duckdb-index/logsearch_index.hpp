#pragma once

// Types appearing in BoundIndex's own interface are taken transitively from its header instead of being included one by
// one: VerifyExistenceType, named in GetConstraintViolationMessage below, is only defined in the art.hpp and it looks
// odd to include this header directly.
#include <duckdb/execution/index/bound_index.hpp>
#include <memory>
#include <string>

namespace logsearch::inverted_index {
class InvertedIndex;
} // namespace logsearch::inverted_index

namespace logsearch::duckdb_index {

//! DuckDB index over one VARCHAR expression of a table, backed by the logsearch inverted index.
class LogsearchIndex : public duckdb::BoundIndex {
public:
    //! `index` is the underlying InvertedIndex data structure. LogsearchIndexBuilder assembles it
    //! during CREATE INDEX.
    LogsearchIndex(const duckdb::Identifier& name, const std::string& index_type,
                   duckdb::IndexConstraintType index_constraint_type,
                   const duckdb::vector<duckdb::column_t>& column_ids, duckdb::TableIOManager& table_io_manager,
                   const duckdb::vector<duckdb::unique_ptr<duckdb::Expression>>& unbound_expressions,
                   duckdb::AttachedDatabase& db, std::unique_ptr<inverted_index::InvertedIndex> index);

    //! Defined out of line so the unique_ptr member is destroyed where InvertedIndex is a complete type.
    ~LogsearchIndex() override;

    // Non-copyable and non-movable: DuckDB owns every index through a unique_ptr<BoundIndex>.
    LogsearchIndex(const LogsearchIndex&) = delete;
    LogsearchIndex& operator=(const LogsearchIndex&) = delete;
    LogsearchIndex(LogsearchIndex&&) = delete;
    LogsearchIndex& operator=(LogsearchIndex&&) = delete;

    //! The inverted index this index is built on. Exposed so tests can query the built index directly.
    //! ResetStorage replaces the inverted index, invalidating any reference handed out here.
    [[nodiscard]] const inverted_index::InvertedIndex& GetInvertedIndex() const;

    duckdb::ErrorData Append(duckdb::IndexLock& l, duckdb::DataChunk& chunk, duckdb::Vector& row_ids) override;
    void ResetStorage(duckdb::IndexLock& index_lock) override;
    duckdb::ErrorData Insert(duckdb::IndexLock& l, duckdb::DataChunk& chunk, duckdb::Vector& row_ids) override;
    duckdb::idx_t TryDelete(duckdb::IndexLock& state, duckdb::DataChunk& entries, duckdb::Vector& row_identifiers,
                            duckdb::optional_ptr<duckdb::SelectionVector> deleted_sel,
                            duckdb::optional_ptr<duckdb::SelectionVector> non_deleted_sel) override;
    bool MergeIndexes(duckdb::IndexLock& state, BoundIndex& other_index) override;
    void Vacuum(duckdb::IndexLock& l) override;
    duckdb::idx_t GetInMemorySize(duckdb::IndexLock& state) override;
    void Verify(duckdb::IndexLock& l) override;
    std::string ToString(duckdb::IndexLock& l, bool display_ascii) override;
    void VerifyAllocations(duckdb::IndexLock& l) override;
    void VerifyBuffers(duckdb::IndexLock& l) override;
    std::string GetConstraintViolationMessage(duckdb::VerifyExistenceType verify_type, duckdb::idx_t failed_index,
                                              duckdb::DataChunk& input) override;
    duckdb::IndexStorageInfo SerializeToDisk(duckdb::QueryContext context,
                                             const duckdb::case_insensitive_map_t<duckdb::Value>& options) override;
    duckdb::IndexStorageInfo SerializeToWAL(const duckdb::case_insensitive_map_t<duckdb::Value>& options) override;

private:
    std::unique_ptr<inverted_index::InvertedIndex> index_;
};

} // namespace logsearch::duckdb_index

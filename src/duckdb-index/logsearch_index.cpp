#include "logsearch_index.hpp"

#include "inverted-index/inverted_index.hpp"

#include <duckdb/common/assert.hpp>
#include <duckdb/common/exception.hpp>
#include <duckdb/common/query_context.hpp>
#include <duckdb/common/string_util.hpp>
#include <duckdb/storage/buffer_manager.hpp>
#include <duckdb/storage/index_storage_info.hpp>
#include <duckdb/storage/table_io_manager.hpp>
#include <memory>
#include <string>
#include <utility>

namespace logsearch::duckdb_index {

LogsearchIndex::LogsearchIndex(const duckdb::Identifier& name, const std::string& index_type,
                               const duckdb::IndexConstraintType index_constraint_type,
                               const duckdb::vector<duckdb::column_t>& column_ids,
                               duckdb::TableIOManager& table_io_manager,
                               const duckdb::vector<duckdb::unique_ptr<duckdb::Expression>>& unbound_expressions,
                               duckdb::AttachedDatabase& db, std::unique_ptr<inverted_index::InvertedIndex> index)
    : BoundIndex(name, index_type, index_constraint_type, column_ids, table_io_manager, unbound_expressions, db),
      index_(std::move(index)) {
    D_ASSERT(index_);
}

LogsearchIndex::~LogsearchIndex() = default;

const inverted_index::InvertedIndex& LogsearchIndex::GetInvertedIndex() const {
    return *index_;
}

void LogsearchIndex::ResetStorage(duckdb::IndexLock& /*index_lock*/) {
    index_ = std::make_unique<inverted_index::InvertedIndex>(duckdb::BufferManager::GetBufferManager(db),
                                                             table_io_manager.GetRowGroupSize());
}

void LogsearchIndex::Vacuum(duckdb::IndexLock& /*l*/) {
    // Sealed partitions are already compact and the Memtable arena is released when a partition is sealed.
}

void LogsearchIndex::Verify(duckdb::IndexLock& /*l*/) {
}

void LogsearchIndex::VerifyAllocations(duckdb::IndexLock& /*l*/) {
}

void LogsearchIndex::VerifyBuffers(duckdb::IndexLock& /*l*/) {
}

std::string LogsearchIndex::ToString(duckdb::IndexLock& /*l*/, bool /*display_ascii*/) {
    // Must be non-empty: PhysicalCreateIndex::Finalize asserts on it.
    return duckdb::StringUtil::Format("LogsearchIndex(%s, sealed_partitions=%llu)",
                                      name.GetIdentifierName(),
                                      static_cast<unsigned long long>(index_->NumSealedPartitions()));
}

duckdb::idx_t LogsearchIndex::GetInMemorySize(duckdb::IndexLock& /*state*/) const {
    // TODO: Account for the Memtable arena and the blocks held by the sealed SSTables.
    return 0;
}

duckdb::ErrorData LogsearchIndex::Append(duckdb::IndexLock& /*l*/, duckdb::DataChunk& /*chunk*/,
                                         duckdb::Vector& /*row_ids*/) {
    throw duckdb::NotImplementedException("A Logsearch index cannot be maintained under appends yet");
}

duckdb::ErrorData LogsearchIndex::Insert(duckdb::IndexLock& /*l*/, duckdb::DataChunk& /*chunk*/,
                                         duckdb::Vector& /*row_ids*/) {
    throw duckdb::NotImplementedException("A Logsearch index cannot be maintained under inserts yet");
}

duckdb::idx_t LogsearchIndex::TryDelete(duckdb::IndexLock& /*state*/, duckdb::DataChunk& /*entries*/,
                                        duckdb::Vector& /*row_identifiers*/,
                                        duckdb::optional_ptr<duckdb::SelectionVector> /*deleted_sel*/,
                                        duckdb::optional_ptr<duckdb::SelectionVector> /*non_deleted_sel*/) {
    // Overridden only to replace the base implementation's InternalException, which DuckDB reports as a fatal
    // assertion failure, with a regular error.
    throw duckdb::NotImplementedException("A Logsearch index cannot be maintained under deletes yet");
}

bool LogsearchIndex::MergeIndexes(duckdb::IndexLock& /*state*/, BoundIndex& /*other_index*/) {
    throw duckdb::NotImplementedException("MergeIndexes is not implemented on Logsearch indexes");
}

std::string LogsearchIndex::GetConstraintViolationMessage(duckdb::VerifyExistenceType /*verify_type*/,
                                                          duckdb::idx_t /*failed_index*/,
                                                          duckdb::DataChunk& /*input*/) const {
    throw duckdb::InternalException("Logsearch indexes never enforce a constraint");
}

duckdb::IndexStorageInfo
LogsearchIndex::SerializeToDisk(duckdb::QueryContext /*context*/,
                                const duckdb::case_insensitive_map_t<duckdb::Value>& /*options*/) {
    // Overridden only to name the index type. The base implementations throw a message that does not.
    throw duckdb::NotImplementedException("A Logsearch index cannot be written to disk yet");
}

duckdb::IndexStorageInfo
LogsearchIndex::SerializeToWAL(const duckdb::case_insensitive_map_t<duckdb::Value>& /*options*/) {
    // Overridden only to name the index type. The base implementations throw a message that does not.
    throw duckdb::NotImplementedException("A Logsearch index cannot be written to the WAL yet");
}

} // namespace logsearch::duckdb_index

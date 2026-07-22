#pragma once

#include <cstddef>
#include <duckdb/common/optional_idx.hpp>
#include <duckdb/common/query_context.hpp>
#include <duckdb/common/typedefs.hpp>
#include <duckdb/storage/storage_info.hpp>
#include <memory>
#include <string_view>
#include <vector>

namespace duckdb {
class BufferManager;
} // namespace duckdb

namespace logsearch::inverted_index {

class Memtable;
class SSTable;

//! The inverted index for one indexed column: an active in-memory Memtable holding the current partition (row
//! group) plus the immutable SSTables of the already-sealed partitions.
class InvertedIndex {
public:
    //! Constructs a new empty InvertedIndex.
    //! `buffer_manager` backs both the Memtable's arena (via its allocator) and the sealed SSTables' blocks.
    //! `row_group_size` is the partition size; it must match the table's row group size (see RowGroupOf).
    explicit InvertedIndex(duckdb::BufferManager& buffer_manager,
                           duckdb::idx_t row_group_size = DEFAULT_ROW_GROUP_SIZE);

    //! Defined out of line so the std::vector<SSTable> member is destroyed where SSTable is a complete type.
    ~InvertedIndex();

    // Non-copyable and non-movable: owns a unique_ptr<Memtable> and holds a BufferManager reference.
    InvertedIndex(const InvertedIndex&) = delete;
    InvertedIndex& operator=(const InvertedIndex&) = delete;
    InvertedIndex(InvertedIndex&&) = delete;
    InvertedIndex& operator=(InvertedIndex&&) = delete;

    //! Append `row_id` to `term`'s postings. Row IDs must arrive non-decreasing. When `row_id` is the first of a new
    //! row group, the current Memtable is sealed into an SSTable and a fresh Memtable starts.
    void Insert(std::string_view term, duckdb::row_t row_id, duckdb::QueryContext context = {});

    //! Return the row ID of every row whose indexed column contains `term`, in ascending order.
    //! Row visibility (deletes/updates) is not accounted for yet.
    [[nodiscard]] std::vector<duckdb::row_t> Lookup(std::string_view term, duckdb::QueryContext context = {}) const;

    //! Number of sealed partitions (SSTables). The active (unsealed) Memtable is not counted.
    [[nodiscard]] std::size_t NumSealedPartitions() const;

private:
    //! The row group (== index partition) that `row_id` belongs to.
    [[nodiscard]] duckdb::idx_t RowGroupOf(duckdb::row_t row_id) const;

    //! True if `row_id` lands in a different row group than the Memtable currently holds.
    //! This indicates that the current partition is complete and must be sealed.
    [[nodiscard]] bool IsNewRowGroup(duckdb::row_t row_id) const;

    duckdb::BufferManager& bm_;
    const std::unique_ptr<Memtable> memtable_;
    std::vector<SSTable> partitions_;
    const duckdb::idx_t row_group_size_;
    duckdb::optional_idx current_rowgroup_;
};

} // namespace logsearch::inverted_index

#pragma once

#include "memtable.hpp"

#include <cstddef>
#include <duckdb/common/query_context.hpp>
#include <duckdb/common/typedefs.hpp>
#include <duckdb/storage/storage_info.hpp>
#include <optional>
#include <string_view>
#include <vector>

namespace duckdb {
class BufferManager;
} // namespace duckdb

namespace logsearch::inverted_index {

class SSTable;

//! The inverted index for one indexed column: an active in-memory Memtable holding the current partition (row
//! group) plus the immutable SSTables of the already-sealed partitions. Insert appends to the memtable and, when a
//! row ID crosses into a new row group, seals the memtable into an SSTable and starts a fresh one. Lookup merges the
//! memtable with every sealed partition.
class InvertedIndex {
public:
    //! `buffer_manager` backs both the memtable's arena (via its allocator) and the sealed SSTables' blocks.
    //! `row_group_size` is the partition size; it must match the table's row group size (see RowGroupOf).
    explicit InvertedIndex(duckdb::BufferManager& buffer_manager,
                           duckdb::idx_t row_group_size = DEFAULT_ROW_GROUP_SIZE);

    //! Defined out of line so the std::vector<SSTable> member is destroyed where SSTable is a complete type.
    ~InvertedIndex();

    // Non-copyable and non-movable: owns a Memtable (arena) and holds a BufferManager reference.
    InvertedIndex(const InvertedIndex&) = delete;
    InvertedIndex& operator=(const InvertedIndex&) = delete;
    InvertedIndex(InvertedIndex&&) = delete;
    InvertedIndex& operator=(InvertedIndex&&) = delete;

    //! Append `row_id` to `term`'s postings. Row IDs must arrive non-decreasing. When `row_id` is the first of a new
    //! row group, the current memtable is sealed into an SSTable and a fresh memtable starts.
    //! `context` attributes the seal's block allocations to the running query; it is not stored.
    void Insert(std::string_view term, duckdb::row_t row_id, duckdb::QueryContext context = duckdb::QueryContext());

    //! Return every row ID whose document contains `term`, in ascending order. Sealed partitions cover disjoint,
    //! ascending row-ID ranges (oldest first) and the memtable holds the newest range, so merging is a plain
    //! concatenation -- no dedup needed. The result is a set of CANDIDATE row IDs; row visibility (deletes/updates)
    //! must be applied by the caller.
    //! `context` attributes the I/O of pinning evicted SSTable blocks to the running query; it is not stored.
    [[nodiscard]] std::vector<duckdb::row_t> Lookup(std::string_view term,
                                                    duckdb::QueryContext context = duckdb::QueryContext()) const;

    //! Number of sealed partitions (SSTables). The active (unsealed) memtable is not counted.
    [[nodiscard]] std::size_t NumSealedPartitions() const;

private:
    //! The row group (== partition) that `row_id` belongs to.
    [[nodiscard]] duckdb::idx_t RowGroupOf(duckdb::row_t row_id) const;

    //! True if `row_id` lands in a different row group than the memtable currently holds -- i.e. the current partition
    //! is complete and must be sealed. False on the first-ever insert (nothing to seal yet).
    [[nodiscard]] bool is_new_rowgroup(duckdb::row_t row_id) const;

    duckdb::BufferManager& bm_;
    Memtable memtable_;
    std::vector<SSTable> partitions_;
    duckdb::idx_t row_group_size_;
    //! Row group index the memtable currently holds. Empty until the first Insert.
    std::optional<duckdb::idx_t> current_rowgroup_;
};

} // namespace logsearch::inverted_index

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

    // Defined out of line so Memtable and SSTable only have to be complete types there.
    ~InvertedIndex();
    // Movable so an index can be built in a local variable and handed on, e.g. by a parallel index build.
    InvertedIndex(InvertedIndex&&) noexcept;

    // Non-copyable: an index owns its Memtable and the blocks behind its partitions.
    InvertedIndex(const InvertedIndex&) = delete;
    InvertedIndex& operator=(const InvertedIndex&) = delete;
    // We currently don't need move-assignment – the move constructor is enough.
    InvertedIndex& operator=(InvertedIndex&&) = delete;

    //! Append `row_id` to `term`'s postings. Row IDs must arrive non-decreasing. When `row_id` is the first of a new
    //! row group, the current Memtable is sealed into an SSTable and a fresh Memtable starts.
    void Insert(std::string_view term, duckdb::row_t row_id, duckdb::QueryContext context = {});

    //! Return the row ID of every row whose indexed column contains `term`, in ascending order.
    //! Row visibility (deletes/updates) is not accounted for yet.
    [[nodiscard]] std::vector<duckdb::row_t> Lookup(std::string_view term, duckdb::QueryContext context = {}) const;

    //! Take over everything `other` holds. `other` is consumed and must not be used again.
    //! Both indexes seal their active partition first, so the result is a single ordered run of sealed partitions.
    //! The list of partitions are merged, not their respective postings lists, so the row-ID ranges of the two indexes
    //! must be disjoint.
    void PairwiseMerge(InvertedIndex&& other, duckdb::QueryContext context = {});

    //! Number of sealed partitions (SSTables). The active (unsealed) Memtable is not counted.
    [[nodiscard]] std::size_t NumSealedPartitions() const;

    //! Number of dictionary entries summed over the active Memtable and every sealed partition. Each partition keeps
    //! its own dictionary, so a term occurring in several of them contributes once per partition. This is a size
    //! measure, not a count of distinct terms.
    [[nodiscard]] std::size_t TotalDictionarySize() const;

private:
    //! The row group (== index partition) that `row_id` belongs to.
    [[nodiscard]] duckdb::idx_t RowGroupOf(duckdb::row_t row_id) const;

    //! True if `row_id` lands in a different row group than the Memtable currently holds.
    //! This indicates that the current partition is complete and must be sealed.
    [[nodiscard]] bool IsNewRowGroup(duckdb::row_t row_id) const;

    //! Seal the active Memtable into an immutable partition and start a fresh one. No-op when Memtable is empty.
    void SealActivePartition(duckdb::QueryContext context = {});

    duckdb::BufferManager& bm_;
    // nullptr only in a moved-from index, which may just be destroyed.
    std::unique_ptr<Memtable> memtable_;
    std::vector<SSTable> partitions_;
    const duckdb::idx_t row_group_size_;
    duckdb::optional_idx current_rowgroup_;
    //! Highest row ID seen so far, or -1 while empty
    // Only read by D_ASSERTS to check global invariant.
    duckdb::row_t last_row_id_ = -1;
};

} // namespace logsearch::inverted_index

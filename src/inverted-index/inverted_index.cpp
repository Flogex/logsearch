#include "inverted_index.hpp"

#include "assertions.hpp"
#include "memtable.hpp"
#include "ss_table.hpp"

#include <algorithm>
#include <cstddef>
#include <duckdb/common/assert.hpp>
#include <duckdb/common/constants.hpp>
#include <duckdb/common/helper.hpp>
#include <duckdb/common/optional_idx.hpp>
#include <duckdb/common/query_context.hpp>
#include <duckdb/common/typedefs.hpp>
#include <duckdb/storage/buffer_manager.hpp>
#include <duckdb/storage/storage_info.hpp>
#include <iterator>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

namespace logsearch::inverted_index {

// In contrast to DefaultAllocator() or Allocator::Get(db), BufferManager::GetBufferAllocator() tracks allocated memory
// and counts it against memory_limit.
InvertedIndex::InvertedIndex(duckdb::BufferManager& buffer_manager, const duckdb::idx_t row_group_size)
    : bm_(buffer_manager), memtable_(std::make_unique<Memtable>(buffer_manager.GetBufferAllocator())),
      row_group_size_(row_group_size) {
    if (row_group_size == 0) {
        throw duckdb::InvalidInputException("row_group_size must not be zero");
    }
    if (row_group_size > duckdb::Storage::MAX_ROW_GROUP_SIZE) {
        throw duckdb::OutOfRangeException("row_group_size must not be larger than %llu",
                                          static_cast<unsigned long long>(duckdb::Storage::MAX_ROW_GROUP_SIZE));
    }
}

// Defined here rather than in the header because std::vector<SSTable> and std::unique_ptr<Memtable> need the wrapped
// classes to be complete.
InvertedIndex::~InvertedIndex() = default;
InvertedIndex::InvertedIndex(InvertedIndex&&) noexcept = default;

void InvertedIndex::Insert(const std::string_view term, const duckdb::row_t row_id,
                           const duckdb::QueryContext context) {
    D_ASSERT(memtable_);
    D_ASSERT(row_id >= last_row_id_);
    if (IsNewRowGroup(row_id)) {
        SealActivePartition(context);
    }
    current_rowgroup_ = RowGroupOf(row_id);
    last_row_id_ = row_id;
    memtable_->Insert(term, row_id);
}

// Sealed partitions cover disjoint, ascending row-ID ranges (oldest first) and the Memtable holds the newest range, so
// merging is a plain concatenation without deduplication.
std::vector<duckdb::row_t> InvertedIndex::Lookup(const std::string_view term,
                                                 const duckdb::QueryContext context) const {
    D_ASSERT(memtable_);
    std::vector<duckdb::row_t> result;
    for (const SSTable& partition : partitions_) {
        const std::vector<duckdb::row_t> hits = partition.Lookup(term, context);
        result.insert(result.end(), hits.begin(), hits.end());
    }
    {
        const std::vector<duckdb::row_t> hits = memtable_->Lookup(term);
        result.insert(result.end(), hits.begin(), hits.end());
    }
    return result;
}

namespace {

#ifdef LS_ASSERTS_ENABLED // only caller is a D_ASSERT
//! Returns true if any partition of `lhs` covers a row ID that a partition of `rhs` also covers.
bool RangesOverlap(const std::vector<SSTable>& lhs, const std::vector<SSTable>& rhs) {
    std::size_t left = 0;
    std::size_t right = 0;
    // `lhs` and `rhs` are both ordered by row ID.
    // Within one vector, the SSTables are internally disjoint.
    // `lhs` and `rhs` can be interleaved.
    while (left < lhs.size() && right < rhs.size()) {
        if (lhs[left].MaxRowId() < rhs[right].MinRowId()) {
            left++;
        } else if (rhs[right].MaxRowId() < lhs[left].MinRowId()) {
            right++;
        } else {
            // Overlapping if and only if lhs.MaxRowId() >= rhs.MinRowId() && rhs.MaxRowId() >= lhs.MinRowId()
            return true;
        }
    }
    return false;
}
#endif

} // namespace

// Called during Combine in index build
void InvertedIndex::PairwiseMerge(InvertedIndex&& other, const duckdb::QueryContext context) {
    D_ASSERT(this != &other);
    D_ASSERT(duckdb::RefersToSameObject(bm_, other.bm_));
    D_ASSERT(row_group_size_ == other.row_group_size_);

    // Actually take `other` over, rather than only emptying it: the caller is left with a moved-from index whose
    // Memtable is null, so every entry point that touches it trips its `D_ASSERT(memtable_)`.
    InvertedIndex source(std::move(other));

    SealActivePartition(context);
    source.SealActivePartition(context);

    // If the two InvertedIndexes had overlapping partitions, we would need to merge the postings lists instead of
    // simply the list of partitions. Fortunately, the index build assigns whole row groups to a task, so no two tasks
    // ever produce a partition with rows from the same row group.
    D_ASSERT(!RangesOverlap(partitions_, source.partitions_));

    // Every task of the index build is assigned arbitrary row groups, so the partitions from both indexes can be
    // interleaved. For example, thread A can build an index for row groups {0,2,5} while thread B gets {1,3,4}. Both
    // halves are already ordered, so merging them beats sorting the concatenation (std::sort).
    // std::inplace_merge would avoid the temporary vector while std::merge requires fewer move operations.
    // Neither really matters for a handful of partitions per merge.
    // Ultimately it was a stylistic choice because I didn't want to make SSTable move-assignable (would be needed for
    // std::inplace_merge).
    std::vector<SSTable> merged;
    merged.reserve(partitions_.size() + source.partitions_.size());
    std::merge(std::make_move_iterator(partitions_.begin()),
               std::make_move_iterator(partitions_.end()),
               std::make_move_iterator(source.partitions_.begin()),
               std::make_move_iterator(source.partitions_.end()),
               std::back_inserter(merged),
               [](const SSTable& lhs, const SSTable& rhs) { return lhs.MinRowId() < rhs.MinRowId(); });
    partitions_ = std::move(merged);
    last_row_id_ = std::max(last_row_id_, source.last_row_id_);
}

std::size_t InvertedIndex::NumSealedPartitions() const {
    return partitions_.size();
}

std::size_t InvertedIndex::TotalDictionarySize() const {
    D_ASSERT(memtable_);
    std::size_t total = memtable_->DictionarySize();
    for (const SSTable& partition : partitions_) {
        total += partition.NumTerms();
    }
    return total;
}

duckdb::idx_t InvertedIndex::RowGroupOf(const duckdb::row_t row_id) const {
    // Floor-dividing by the row group size assumes that row groups start at multiples of `row_group_size_`.
    // That holds for a table carrying this index: Indexed tables always top up their last row group (SUGGEST_NEW is
    // ignored) and are never vacuumed, so groups stay boundary-aligned. Still, we need to improve this calculation in
    // the future to be able to deal with concurrent checkpoints and vacuums.
    D_ASSERT(row_id >= 0 && row_id <= duckdb::MAX_ROW_ID);
    return static_cast<duckdb::idx_t>(row_id) / row_group_size_;
}

bool InvertedIndex::IsNewRowGroup(const duckdb::row_t row_id) const {
    // A jump across several row groups still seals only once, hence empty intermediate partitions are never created.
    return current_rowgroup_.IsValid() && RowGroupOf(row_id) != current_rowgroup_.GetIndex();
}

void InvertedIndex::SealActivePartition(const duckdb::QueryContext context) {
    D_ASSERT(memtable_);
    // A partition whose documents produced no terms at all leaves the Memtable empty. No need to create an empty
    // SSTable from it.
    if (memtable_->DictionarySize() != 0) {
        SSTable partition = memtable_->Seal(bm_, context);
        D_ASSERT(partitions_.empty() || partitions_.back().MaxRowId() < partition.MinRowId());
        // Insert seals on every row-group change, so a partition never straddles a boundary. `PairwiseMerge` depends on
        // it: partitions of two indexes are disjoint only because each one stays within a single row group.
        D_ASSERT(RowGroupOf(partition.MinRowId()) == RowGroupOf(partition.MaxRowId()));
        partitions_.push_back(std::move(partition));
        memtable_->Reset();
    }
    // The next Insert has to open a new partition because, once sealed, a partition in a SSTable is immutable.
    current_rowgroup_ = duckdb::optional_idx();
}

} // namespace logsearch::inverted_index

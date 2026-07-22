#ifndef INVERTED_INDEX_UNITY_BUILD
#error "inverted_index.cpp must be compiled as part of the Inverted-Index unity build"
#endif

#include "inverted_index.hpp"

#include "memtable.hpp"
#include "ss_table.hpp"

#include <cstddef>
#include <duckdb/common/assert.hpp>
#include <duckdb/common/constants.hpp>
#include <duckdb/common/optional_idx.hpp>
#include <duckdb/common/query_context.hpp>
#include <duckdb/common/typedefs.hpp>
#include <duckdb/storage/buffer_manager.hpp>
#include <duckdb/storage/storage_info.hpp>
#include <memory>
#include <string_view>
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

// We defined the destructor in the .cpp file to be able to forward-declare Memtable and SSTable.
// The destructors of std::vector<SSTable> and std::unique_ptr<Memtable> need the wrapped classes to be defined.
InvertedIndex::~InvertedIndex() = default;

void InvertedIndex::Insert(const std::string_view term, const duckdb::row_t row_id,
                           const duckdb::QueryContext context) {
    // TODO: Assert that row_id is non-decreasing globally. Memtable just checks within one partition.
    if (IsNewRowGroup(row_id)) {
        partitions_.push_back(memtable_->Seal(bm_, context));
        memtable_->Reset();
    }
    current_rowgroup_ = RowGroupOf(row_id);
    memtable_->Insert(term, row_id);
}

// Sealed partitions cover disjoint, ascending row-ID ranges (oldest first) and the Memtable holds the newest range, so
// merging is a plain concatenation without deduplication.
std::vector<duckdb::row_t> InvertedIndex::Lookup(const std::string_view term,
                                                 const duckdb::QueryContext context) const {
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

std::size_t InvertedIndex::NumSealedPartitions() const {
    return partitions_.size();
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

} // namespace logsearch::inverted_index

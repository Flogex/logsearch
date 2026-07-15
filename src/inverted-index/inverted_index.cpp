#ifndef INVERTED_INDEX_UNITY_BUILD
#error "inverted_index.cpp must be compiled as part of the Inverted-Index unity build"
#endif

#include "inverted_index.hpp"

#include <cstddef>
#include <duckdb/common/assert.hpp>
#include <duckdb/storage/buffer_manager.hpp>
#include <string_view>
#include <vector>

namespace logsearch::inverted_index {

InvertedIndex::InvertedIndex(duckdb::BufferManager& buffer_manager, const duckdb::idx_t row_group_size)
    : bm_(buffer_manager), memtable_(buffer_manager.GetBufferAllocator()), row_group_size_(row_group_size) {
    D_ASSERT(row_group_size_ > 0);
}

void InvertedIndex::Insert(const std::string_view term, const duckdb::row_t row_id) {
    if (is_new_rowgroup(row_id)) {
        partitions_.push_back(memtable_.Seal(bm_));
        memtable_.Reset();
    }
    current_rowgroup_ = RowGroupOf(row_id);
    memtable_.Insert(term, row_id);
}

std::vector<duckdb::row_t> InvertedIndex::Lookup(const std::string_view term) const {
    std::vector<duckdb::row_t> result;
    for (const SSTable& partition : partitions_) {
        const std::vector<duckdb::row_t> hits = partition.Lookup(term);
        result.insert(result.end(), hits.begin(), hits.end());
    }
    const std::vector<duckdb::row_t> memtable_hits = memtable_.Lookup(term);
    result.insert(result.end(), memtable_hits.begin(), memtable_hits.end());
    return result;
}

std::size_t InvertedIndex::NumSealedPartitions() const {
    return partitions_.size();
}

duckdb::idx_t InvertedIndex::RowGroupOf(const duckdb::row_t row_id) const {
    // Floor-dividing by the row group size assumes row groups start at multiples of row_group_size_. That holds for a
    // table carrying this index: indexed tables always top up their last row group (SUGGEST_NEW is ignored) and are
    // never vacuum-merged, so groups stay boundary-aligned -- full groups plus a possibly-partial *terminal* group,
    // which simply becomes its own (smaller) partition. It would NOT hold for a partial *non-terminal* group (only
    // reachable on a table without this index), which starts at a non-multiple and would need the row group's real
    // start from the storage layer.
    D_ASSERT(row_id >= 0);
    return static_cast<duckdb::idx_t>(row_id) / row_group_size_;
}

bool InvertedIndex::is_new_rowgroup(const duckdb::row_t row_id) const {
    // A jump across several row groups still seals only once; empty intermediate partitions are never created.
    return current_rowgroup_.has_value() && RowGroupOf(row_id) != *current_rowgroup_;
}

} // namespace logsearch::inverted_index

#ifndef INVERTED_INDEX_UNITY_BUILD
#error "ss_table.cpp must be compiled as part of the Inverted-Index unity build"
#endif

#include "ss_table.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <duckdb/common/assert.hpp>
#include <duckdb/common/constants.hpp>
#include <duckdb/common/query_context.hpp>
#include <duckdb/common/typedefs.hpp>
#include <duckdb/storage/buffer/buffer_handle.hpp>
#include <duckdb/storage/buffer_manager.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace logsearch::inverted_index {

SSTable::SSTable(duckdb::BufferManager& buffer_manager, std::vector<duckdb::shared_ptr<duckdb::BlockHandle>>&& blocks,
                 const duckdb::idx_t block_size, const SSTableHeader& header)
    : bm_(buffer_manager), blocks_(std::move(blocks)), block_size_(block_size), header_(header) {
    Verify();
}

class SSTable::PinnedBlockCache {
public:
    explicit PinnedBlockCache(const SSTable& table, const duckdb::QueryContext context)
        : table_(table), context_(context) {
    }

    //! Returns pointer to logical offset, ensuring that the block that contains this byte is pinned.
    //! The returned pointer is valid only until the next call that lands in a different block.
    //! A new block only gets pinned if a new read falls outside its bounds, reducing the total number of `Pin` calls.
    duckdb::const_data_ptr_t PointerAt(const std::uint64_t offset) {
        const duckdb::idx_t block_index = offset / table_.block_size_;
        const duckdb::idx_t block_offset = offset % table_.block_size_;
        if (block_index != pinned_block_index_) {
            // Pin takes a non-const shared_ptr&, and blocks_ is const through `table_`, so copy to a local lvalue.
            D_ASSERT(block_index < table_.blocks_.size());
            auto handle = table_.blocks_[block_index];
            // Reassigning `pinned_block_` drops the previous pin. Hence, at most one block stays pinned at a time.
            pinned_block_ = table_.bm_.Pin(context_, handle);
            pinned_block_index_ = block_index;
        }
        return pinned_block_.Ptr() + block_offset;
    }

    //! Copy `length` bytes from logical `offset` into `dst`, crossing (and re-pinning) blocks as needed.
    void ReadInto(std::uint64_t offset, std::uint64_t length, void* dst) {
        auto* out = static_cast<duckdb::data_ptr_t>(dst);
        while (length > 0) {
            const duckdb::idx_t block_offset = offset % table_.block_size_;
            const std::uint64_t chunk_size = std::min<std::uint64_t>(length, table_.block_size_ - block_offset);
            std::memcpy(out, PointerAt(offset), chunk_size);
            out += chunk_size;
            offset += chunk_size;
            length -= chunk_size;
        }
    }

private:
    const SSTable& table_;
    const duckdb::QueryContext context_;
    duckdb::idx_t pinned_block_index_ = duckdb::DConstants::INVALID_INDEX;
    duckdb::BufferHandle pinned_block_;
};

int SSTable::CompareTermAt(PinnedBlockCache& block_reader, std::uint64_t offset, const std::uint32_t stored_length,
                           const std::string_view comparatum) const {
    {
        const duckdb::idx_t block_offset = offset % block_size_;
        // Fast path (common case): The whole stored term is contiguous within one block.
        if (block_offset + stored_length <= block_size_) {
            const std::string_view stored(duckdb::const_char_ptr_cast(block_reader.PointerAt(offset)), stored_length);
            return stored.compare(comparatum);
        }
    }

    // Slow path: The stored term straddles a block boundary. Compare it piece by piece.
    const auto comparatum_length = static_cast<std::uint32_t>(comparatum.size());
    const std::uint32_t min_read_length = std::min(stored_length, comparatum_length);
    std::uint64_t remaining = min_read_length;
    std::uint64_t comparatum_pos = 0;
    while (remaining > 0) {
        const duckdb::idx_t block_offset = offset % block_size_;
        const std::uint64_t chunk_size = std::min<std::uint64_t>(remaining, block_size_ - block_offset);
        const int cmp = std::memcmp(block_reader.PointerAt(offset), comparatum.data() + comparatum_pos, chunk_size);
        if (cmp != 0) {
            return cmp;
        }
        offset += chunk_size;
        comparatum_pos += chunk_size;
        remaining -= chunk_size;
    }
    // Common prefix is equal: The shorter term sorts first.
    if (stored_length < comparatum_length) {
        return -1;
    }
    if (stored_length > comparatum_length) {
        return 1;
    }
    return 0;
}

// TODO: Maybe return std::optional
std::vector<duckdb::row_t> SSTable::Lookup(const std::string_view search_term,
                                           const duckdb::QueryContext context) const {
    // Two readers so recently used dictionary and string pool blocks stay pinned.
    PinnedBlockCache dict_reader(*this, context);
    PinnedBlockCache term_reader(*this, context);
    // Binary search the sorted, fixed-size dictionary entries.
    std::uint64_t low = 0;
    std::uint64_t high = header_.num_terms;
    while (low < high) {
        const std::uint64_t mid = low + ((high - low) / 2);
        // First, read the dictionary entry to get the term_offset to compare with `search_term`.
        // TODO: Alternatively, keep the complete dictionary always in memory as std::vector<SSTableDictEntry>.
        SSTableDictEntry entry;
        dict_reader.ReadInto(header_.dict_offset + (mid * sizeof(SSTableDictEntry)), sizeof(entry), &entry);
        // Do the comparison by reading the term from the string pool
        const int cmp =
            CompareTermAt(term_reader, header_.strings_offset + entry.term_offset, entry.term_length, search_term);
        if (cmp == 0) {
            std::vector<duckdb::row_t> result(entry.postings_count);
            if (entry.postings_count > 0) {
                term_reader.ReadInto(header_.postings_offset + entry.postings_offset,
                                     static_cast<std::uint64_t>(entry.postings_count) * sizeof(duckdb::row_t),
                                     result.data());
            }
            D_ASSERT(std::is_sorted(result.begin(), result.end()));
            D_ASSERT(std::adjacent_find(result.begin(), result.end()) == result.end()); // No duplicates
            return result;
        }
        if (cmp < 0) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    return {};
}

void SSTable::Verify() const {
#if defined(D_ASSERT_IS_ENABLED) || !defined(NDEBUG)
    D_ASSERT(header_.magic == SSTABLE_MAGIC);
    D_ASSERT(header_.version == SSTABLE_VERSION);
    D_ASSERT(header_.block_size == block_size_);
    D_ASSERT(header_.dict_offset == sizeof(SSTableHeader));
    D_ASSERT(header_.strings_offset <= header_.postings_offset);
    D_ASSERT(header_.postings_offset % alignof(duckdb::row_t) == 0);
    D_ASSERT(header_.postings_offset <= header_.total_size);
    D_ASSERT(header_.num_terms > 0);

    PinnedBlockCache block_reader(*this, duckdb::QueryContext());
    const std::uint64_t pool_capacity = header_.postings_offset - header_.strings_offset;
    std::string prev_term;
    for (std::uint64_t i = 0; i < header_.num_terms; i++) {
        SSTableDictEntry entry{};
        block_reader.ReadInto(header_.dict_offset + (i * sizeof(SSTableDictEntry)), sizeof(entry), &entry);

        const std::uint64_t postings_byte_offset = header_.postings_offset + entry.postings_offset;
        const std::uint64_t postings_bytes = static_cast<std::uint64_t>(entry.postings_count) * sizeof(duckdb::row_t);
        D_ASSERT(postings_byte_offset + postings_bytes <= header_.total_size);

        D_ASSERT(entry.term_length > 0);
        D_ASSERT(static_cast<std::uint64_t>(entry.term_offset) + entry.term_length <= pool_capacity);
        std::string term(entry.term_length, '\0');
        block_reader.ReadInto(header_.strings_offset + entry.term_offset, entry.term_length, term.data());
        if (i > 0) {
            D_ASSERT(prev_term < term); // strictly increasing => sorted and unique
        }
        prev_term = term;

        std::vector<duckdb::row_t> postings(entry.postings_count);
        if (entry.postings_count > 0) {
            block_reader.ReadInto(postings_byte_offset, postings_bytes, postings.data());
            for (std::size_t k = 0; k < postings.size(); k++) {
                D_ASSERT(postings[k] >= header_.min_row_id && postings[k] <= header_.max_row_id);
                if (k > 0) {
                    D_ASSERT(postings[k - 1] < postings[k]);
                }
            }
        }
    }
#endif
}

} // namespace logsearch::inverted_index

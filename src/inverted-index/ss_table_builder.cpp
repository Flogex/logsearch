#ifndef INVERTED_INDEX_UNITY_BUILD
#error "ss_table_builder.cpp must be compiled as part of the Inverted-Index unity build"
#endif

#include "ss_table_builder.hpp"

#include "ss_table.hpp"
#include "storage/multi_block_writer.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <duckdb/common/assert.hpp>
#include <duckdb/common/constants.hpp>
#include <duckdb/common/exception.hpp>
#include <duckdb/common/helper.hpp>
#include <duckdb/common/optional_idx.hpp>
#include <duckdb/common/query_context.hpp>
#include <duckdb/common/serializer/write_stream.hpp>
#include <duckdb/common/typedefs.hpp>
#include <duckdb/storage/buffer_manager.hpp>
#include <limits>
#include <vector>

namespace logsearch::inverted_index {
namespace {

SSTableHeader ComputeHeader(const std::vector<TermPostings>& terms, const duckdb::idx_t block_size) {
    const std::uint64_t num_terms = terms.size();
    // Dictionary starts right after the header.
    constexpr std::uint64_t dict_offset = sizeof(SSTableHeader);
    // String pool starts after the dictionary entries, which have a fixed size.
    constexpr std::uint64_t dict_entry_size = sizeof(SSTableDictEntry);
    const std::uint64_t strings_offset = dict_offset + (num_terms * dict_entry_size);

    std::uint64_t string_pool_size = 0;
    std::uint64_t total_postings = 0;
    duckdb::row_t min_row_id = duckdb::MAX_ROW_ID + 1;
    duckdb::row_t max_row_id = -1;
    for (const auto& [term, postings] : terms) {
        D_ASSERT(!term.empty());
        string_pool_size += term.size();

        D_ASSERT(postings != nullptr);
        D_ASSERT(!postings->empty());
        total_postings += postings->size();

        D_ASSERT(postings->front() <= duckdb::MAX_ROW_ID);
        D_ASSERT(postings->back() >= 0);
        D_ASSERT(std::is_sorted(postings->begin(), postings->end()));
        min_row_id = std::min(min_row_id, postings->front());
        max_row_id = std::max(max_row_id, postings->back());
    }
    D_ASSERT(min_row_id <= max_row_id);
    // The postings region is 8-byte-aligned, so raw row_t elements stay aligned and never split across a block.
    const std::uint64_t postings_offset =
        duckdb::AlignValue<std::uint64_t, alignof(duckdb::row_t)>(strings_offset + string_pool_size);

    SSTableHeader header{};
    header.magic = SSTABLE_MAGIC;
    header.version = SSTABLE_VERSION;
    header.block_size = block_size;
    header.total_size = postings_offset + (total_postings * sizeof(duckdb::row_t));
    header.num_terms = num_terms;
    header.dict_offset = dict_offset;
    header.strings_offset = strings_offset;
    header.postings_offset = postings_offset;
    header.min_row_id = min_row_id;
    header.max_row_id = max_row_id;
    return header;
}

void WriteSSTableBytes(duckdb::WriteStream& out, const std::vector<TermPostings>& terms, const SSTableHeader& header) {
    out.Write(header);

    /***** Dictionary *****/
    std::uint64_t current_term_offset = 0;
    std::uint64_t current_postings_offset = 0; // relative to header.postings_offset
    for (const auto& [term, postings] : terms) {
        SSTableDictEntry dict_entry{};
        // `term_offset` narrows a cumulative string-pool offset to uint32. For one row-group partition, the string pool
        // size should be far under 4 GiB in practice, but nothing formally caps it.
        if (current_term_offset > std::numeric_limits<std::uint32_t>::max()) {
            throw duckdb::OutOfRangeException(
                "SSTable string pool too large: term offset %llu does not fit in uint32_t", current_term_offset);
        }
        dict_entry.term_offset = static_cast<std::uint32_t>(current_term_offset);
        // TODO: The analyzer must cap token length. Right now, a term longer than 4 GiB would get truncated.
        D_ASSERT(term.size() <= std::numeric_limits<std::uint32_t>::max());
        dict_entry.term_length = static_cast<std::uint32_t>(term.size());
        // `postings_offset` narrows a cumulative postings-region offset to uint32 (same soft 4 GiB bound as the string
        // pool).
        if (current_postings_offset > std::numeric_limits<std::uint32_t>::max()) {
            throw duckdb::OutOfRangeException(
                "SSTable postings region too large: postings offset %llu does not fit in uint32_t",
                current_postings_offset);
        }
        dict_entry.postings_offset = static_cast<std::uint32_t>(current_postings_offset);
        // A term appears at most once per row, so postings_count <= rows in this partition <= MAX_ROW_GROUP_SIZE,
        // which fits into uint32.
        D_ASSERT(postings->size() <= std::numeric_limits<std::uint32_t>::max());
        dict_entry.postings_count = static_cast<std::uint32_t>(postings->size());
        out.Write(dict_entry);
        current_term_offset += term.size();
        current_postings_offset += postings->size() * sizeof(duckdb::row_t);
    }

    /***** String pool *****/
    std::uint64_t pool_size = 0;
    for (const auto& [term, _] : terms) {
        // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage): The length is passed explicitly alongside data().
        out.WriteData(duckdb::const_data_ptr_cast(term.data()), term.size());
        pool_size += term.size();
    }

    /***** Postings list *****/
    // Value of header.postings_offset is 8-byte-aligned
    const std::uint64_t padding_length = header.postings_offset - (header.strings_offset + pool_size);
    constexpr std::array<std::uint8_t, sizeof(duckdb::row_t)> zeros{};
    D_ASSERT(padding_length < zeros.size());
    out.WriteData(zeros.data(), padding_length);

    for (const auto& [_, postings] : terms) {
        out.WriteData(duckdb::const_data_ptr_cast(postings->data()), postings->size() * sizeof(duckdb::row_t));
    }
}

} // namespace

// The method takes `terms` as rvalue reference to force the caller to move it and prevent silent copies.
// We would just copy string_views and pointers (relatively cheap), but the caller doesn't need this vector anymore, so
// we can also avoid this copy. An alternative would be to take a sorted vector, but I prefer if Build owns this
// "precondition" and doesn't rely on the caller.
//
// NOLINTNEXTLINE(cppcoreguidelines-rvalue-reference-param-not-moved): consumed by writing sorted terms to block
SSTable SSTableBuilder::Build(duckdb::BufferManager& buffer_manager, std::vector<TermPostings>&& terms,
                              const duckdb::QueryContext context, const duckdb::optional_idx block_size_overwrite) {
    D_ASSERT(!terms.empty());
    // Tests pass a small `block_size_overwrite` value to get multiple blocks already with little data.
    // DEFAULT_BLOCK_SIZE is 262136 bytes (2^18 - 8 header bytes).
    const duckdb::idx_t block_size =
        block_size_overwrite.IsValid() ? block_size_overwrite.GetIndex() : buffer_manager.GetBlockSize();
    // `block_size` must be a multiple of 8, so block boundaries are 8-byte-aligned and a duckdb::row_t never straddles
    // one.
    D_ASSERT(block_size % alignof(duckdb::row_t) == 0);

    // Terms arrive in arbitrary order. The dictionary must be sorted for binary-search lookups.
    std::sort(terms.begin(), terms.end(), [](const TermPostings& a, const TermPostings& b) { return a.term < b.term; });
    // No duplicates
    D_ASSERT(std::adjacent_find(terms.begin(), terms.end(), [](const TermPostings& a, const TermPostings& b) {
                 return a.term == b.term;
             }) == terms.end());

    // Pass 1: Computes the region offsets, total size, and row-id range for the sorted terms.
    const SSTableHeader header = ComputeHeader(terms, block_size);

    // Pass 2: Writes the full SSTable byte layout (header, dictionary, string pool, alignment padding, postings).
    storage::MultiBlockWriter writer(buffer_manager, block_size, context);
    WriteSSTableBytes(writer, terms, header);
    auto blocks = writer.Finish();
    D_ASSERT(writer.BytesWritten() == header.total_size);

    return SSTable{buffer_manager, std::move(blocks), block_size, header};
}
} // namespace logsearch::inverted_index

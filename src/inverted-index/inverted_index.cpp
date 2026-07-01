#ifndef INVERTED_INDEX_UNITY_BUILD
#error "inverted_index.cpp must be compiled as part of the Inverted-Index unity build"
#endif

#include "inverted_index.hpp"

#include "postings_list.hpp"

#include <cstddef>
#include <cstdint>
#include <duckdb/common/assert.hpp>
#include <duckdb/common/typedefs.hpp>
#include <duckdb/storage/storage_info.hpp>
#include <string_view>
#include <vector>

namespace logsearch::inverted_index {

InvertedIndex::InvertedIndex(duckdb::Allocator& allocator) : arena_(allocator) {
    dictionary_.reserve(EXPECTED_NUM_TERMS);
}

void InvertedIndex::Insert(const std::string_view term, const duckdb::row_t row_id) {
    // try_emplace hashes the term only ones, compared to find + conditional emplace.
    // Constructs an empty PostingsList if the term is not found.
    // TODO: Avoid this string allocation by either taking a std::string&& r-value or enabling transparent lookup.
    const auto [it, was_inserted] = dictionary_.try_emplace(std::string(term));
    PostingsList& postings = it->second;

    if (was_inserted) {
        // Term was inserted into dictionary. Now construct new postings list with one segment.
        D_ASSERT(postings.size == 0);
        auto* segment = arena_.Make<PostingsSegment>();
        postings.head = segment;
        postings.tail = segment;

        static_assert(std::tuple_size_v<decltype(segment->entries)> > 0);
        segment->entries[0] = row_id;
    } else {
        D_ASSERT(postings.size > 0);
        // We fill segments completely before creating a new one. Only the tail segment could be partially filled.
        const std::uint32_t tail_fill = ((postings.size - 1) % PostingsSegment::Capacity()) + 1;
        const duckdb::row_t prev_row_id = postings.tail->entries[tail_fill - 1];

        if (prev_row_id == row_id) {
            // A single document (with the same row ID) can contain a term multiple times.
            // The analyzer emits a term per occurrence.
            // For simple boolean retrieval, we for now drop the duplicated entry.
            // In later versions of the extension, we need it for e.g., positional information and term frequency.
            // The same row ID will arrive consecutively because a document is completely processed before moving on to
            // the next one.
            return;
        }
        // Rows arrive ordered by row ID
        D_ASSERT(prev_row_id < row_id);

        if (tail_fill < PostingsSegment::Capacity()) {
            // `entries` hold POSTINGS_UNWRITTEN_SENTINEL when assertions are live. Checks that we write to a fresh
            // slot.
            D_ASSERT(postings.tail->entries[tail_fill] == POSTINGS_UNWRITTEN_SENTINEL);
            postings.tail->entries[tail_fill] = row_id;
        } else {
            D_ASSERT(tail_fill == PostingsSegment::Capacity());
            auto* segment = arena_.Make<PostingsSegment>();
            postings.tail->next = segment;
            postings.tail = segment;
            segment->entries[0] = row_id;
        }
    }

    postings.size++;
    // For now, we only have a memtable that supports at most one row group of data.
    D_ASSERT(postings.size <= DEFAULT_ROW_GROUP_SIZE);
}

std::vector<duckdb::row_t> InvertedIndex::Lookup(const std::string_view term) const {
    std::vector<duckdb::row_t> result;

    // TODO: Avoid string allocation
    const auto it = dictionary_.find(std::string(term));
    if (it == dictionary_.end()) {
        return result;
    }

    const PostingsList& postings = it->second;
    postings.Verify();

    result.reserve(postings.size);
    std::uint32_t remaining = postings.size;
    for (const PostingsSegment* segment = postings.head; segment != nullptr; segment = segment->next) {
        // nullptr passed to prefetch on last iteration is a harmless no-op
        __builtin_prefetch(segment->next, /*rw=*/0, /*locality=*/0);

        // All segments but the last one are completely filled
        const std::uint32_t count = remaining < PostingsSegment::Capacity() ? remaining : PostingsSegment::Capacity();
        result.insert(result.end(), segment->entries.data(), segment->entries.data() + count);
        remaining -= count;
    }
    D_ASSERT(remaining == 0);

#if defined(D_ASSERT_IS_ENABLED) || !defined(NDEBUG)
    // Assert ascending row ID order
    for (size_t i = 1; i < result.size(); i++) {
        D_ASSERT(result[i - 1] < result[i]);
    }
#endif

    return result;
}

std::size_t InvertedIndex::DictionarySize() const {
    return dictionary_.size();
}

} // namespace logsearch::inverted_index

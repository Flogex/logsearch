#ifndef INVERTED_INDEX_UNITY_BUILD
#error "memtable.cpp must be compiled as part of the Inverted-Index unity build"
#endif

#include "memtable.hpp"

#include "postings_list.hpp"
#include "ss_table.hpp"
#include "ss_table_builder.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <duckdb/common/assert.hpp>
#include <duckdb/common/query_context.hpp>
#include <duckdb/common/typedefs.hpp>
#include <duckdb/storage/storage_info.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace logsearch::inverted_index {

namespace {
//! Walk a postings segment chain into an ascending vector of row IDs.
std::vector<duckdb::row_t> CollectPostings(const PostingsList& postings) {
    postings.Verify();

    std::vector<duckdb::row_t> result;
    result.reserve(postings.size);
    std::uint32_t remaining = postings.size;
    for (const PostingsSegment* segment = postings.head; segment != nullptr; segment = segment->next) {
        // nullptr passed to prefetch on the last iteration is a harmless no-op.
        __builtin_prefetch(segment->next, /*rw=*/0, /*locality=*/0);
        // All segments but the last one are completely filled.
        const std::uint32_t count = remaining < PostingsSegment::Capacity() ? remaining : PostingsSegment::Capacity();
        result.insert(result.end(), segment->entries.data(), segment->entries.data() + count);
        remaining -= count;
    }

    D_ASSERT(remaining == 0);
    D_ASSERT(std::is_sorted(result.begin(), result.end()));
    D_ASSERT(std::adjacent_find(result.begin(), result.end()) == result.end()); // No duplicates

    return result;
}
} // namespace

Memtable::Memtable(duckdb::Allocator& allocator) : arena_(allocator) {
    dictionary_.reserve(EXPECTED_NUM_TERMS);
}

void Memtable::Insert(const std::string_view term, const duckdb::row_t row_id) {
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
    // The Memtable should get sealed before more rows than fit in one row group get inserted.
    D_ASSERT(postings.size <= duckdb::Storage::MAX_ROW_GROUP_SIZE);
    // To make storage_info.hpp used in release build
    std::ignore = duckdb::Storage::MAX_ROW_GROUP_SIZE;
}

std::vector<duckdb::row_t> Memtable::Lookup(const std::string_view term) const {
    // TODO: Avoid string allocation
    const auto it = dictionary_.find(std::string(term));
    if (it == dictionary_.end()) {
        return {};
    }
    return CollectPostings(it->second);
}

std::size_t Memtable::DictionarySize() const {
    return dictionary_.size();
}

SSTable Memtable::Seal(duckdb::BufferManager& buffer_manager, const duckdb::QueryContext context) const {
    // Materialize each term's postings chain into a vector. The TermPostings views point at these vectors and at the
    // dictionary keys, all of which outlive the SSTableBuilder::Build call.
    // TODO: Should use cursor into postings list instead of materializing it.
    std::vector<std::vector<duckdb::row_t>> postings_storage;
    // Reserve so `postings_storage` never reallocates, otherwise the pointers we take into it would dangle.
    postings_storage.reserve(dictionary_.size());
    std::vector<TermPostings> terms;
    terms.reserve(dictionary_.size());
    for (const auto& [term, postings] : dictionary_) {
        postings_storage.push_back(CollectPostings(postings));
        terms.push_back({term, &postings_storage.back()});
    }
    return SSTableBuilder::Build(buffer_manager, std::move(terms), context);
}

void Memtable::Reset() {
    // Note: clear() doesn't change the capacity of the dictionary.
    // TODO: Re-create `dictionary_` if capacity is above some threshold.
    dictionary_.clear();
    arena_.Reset();
}

} // namespace logsearch::inverted_index

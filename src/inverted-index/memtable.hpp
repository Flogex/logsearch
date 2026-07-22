#pragma once

#include "postings_list.hpp"

#include <cstddef>
#include <duckdb/common/allocator.hpp>
#include <duckdb/common/query_context.hpp>
#include <duckdb/common/typedefs.hpp>
#include <duckdb/storage/arena_allocator.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace duckdb {
class BufferManager;
} // namespace duckdb

namespace logsearch::inverted_index {

class SSTable;

//! In-memory inverted index for a single, not-yet-sealed partition (< 1 row group). Maps each term to an
//! append-only, deduplicated, ascending postings list of row IDs. Sealed into an immutable SSTable once the index
//! partition is full.
class Memtable {
public:
    explicit Memtable(duckdb::Allocator& allocator);

    // Non-copyable: dictionary_ holds raw PostingsSegment* into arena_. A memberwise
    // copy would leave those pointers aliasing/dangling into the source arena.
    // Non-movable: ArenaAllocator is not movable (holds a backing-allocator reference).
    Memtable(const Memtable&) = delete;
    Memtable& operator=(const Memtable&) = delete;
    Memtable(Memtable&&) = delete;
    Memtable& operator=(Memtable&&) = delete;
    ~Memtable() = default;

    //! Append `row_id` to `term`'s postings list. Row IDs must arrive non-decreasing. Repeated rows per term are
    //! deduplicated.
    void Insert(std::string_view term, duckdb::row_t row_id);

    //! Return `term`'s postings list in ascending row-ID order, or an empty vector if the term is absent.
    [[nodiscard]] std::vector<duckdb::row_t> Lookup(std::string_view term) const;

    //! Returns the number of terms in the dictionary of this index.
    [[nodiscard]] std::size_t DictionarySize() const;

    //! Seal this (full) Memtable into an immutable SSTable. This does not modify the Memtable.
    //! Call Reset() afterwards to reuse it for the next partition.
    [[nodiscard]] SSTable Seal(duckdb::BufferManager& buffer_manager, duckdb::QueryContext context = {}) const;

    //! Clears the dictionary and postings lists.
    void Reset();

private:
    //! Initial bucket reservation.
    // TODO: Make dynamic based on term distributions observed at runtime.
    static constexpr std::size_t EXPECTED_NUM_TERMS = 50000;

    // Posting segments are bump-allocated from one arena. This avoids a malloc/free per segment and frees them all at
    // once when the Memtable is sealed. Trade-off: The arena never reclaims individual allocations and tail segments
    // are partially filled, so the Memtable can hold somewhat more memory than the live postings strictly need.
    duckdb::ArenaAllocator arena_;

    // TODO: Benchmark and maybe replace with absl::flat_hash_map or ankerl::unordered_dense::map
    std::unordered_map<std::string, PostingsList> dictionary_;
};

} // namespace logsearch::inverted_index

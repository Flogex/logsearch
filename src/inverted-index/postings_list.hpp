#pragma once

#include <array>
#include <cstdint>
#include <duckdb/common/typedefs.hpp>
#include <type_traits>

namespace logsearch::inverted_index {

//! Number of row IDs per PostingsSegment.
// TODO: Benchmark different values for SEGMENT_CAPACITY.
static constexpr std::uint32_t POSTINGS_SEGMENT_CAPACITY = 16;
static_assert(POSTINGS_SEGMENT_CAPACITY > 0, "POSTINGS_SEGMENT_CAPACITY must be positive");
static_assert((POSTINGS_SEGMENT_CAPACITY & (POSTINGS_SEGMENT_CAPACITY - 1)) == 0,
              "POSTINGS_SEGMENT_CAPACITY must be a power of two for performant modulo operations");

//! Default value for slot in PostingsSegment to indicate that it has not been written yet.
constexpr duckdb::row_t POSTINGS_UNWRITTEN_SENTINEL = -1;

//! A PostingsList of a memtable consists of multiple segments, each storing an ordered list of the row IDs of the
//! documents that contain the corresponding term.
// The reason for using segments is to not have to reallocate a single growable array for each term.
// Since we are using the duckdb::ArenaAllocator, this would leave the old allocations stranded in the arena.
struct PostingsSegment {
    PostingsSegment() noexcept;

    //! Number of row IDs a segment can hold.
    [[nodiscard]] static constexpr std::uint32_t Capacity() noexcept {
        return POSTINGS_SEGMENT_CAPACITY;
    }

    //! Ordered row IDs of a subset of documents in this postings list.
    std::array<duckdb::row_t, POSTINGS_SEGMENT_CAPACITY> entries;
    //! Pointer to the next segment in the chain (linked list)
    PostingsSegment* next = nullptr;
};

// The ArenaAllocator frees segment memory in bulk without running destructors, so PostingsSegment must stay trivially
// destructible, otherwise a member's destructor would silently never run, leaking it.
static_assert(std::is_trivially_destructible_v<PostingsSegment>, "PostingsSegment must be trivially destructible");

// Compared to a std::deque, this linked list is currently append-only at tail, doesn't support random access,
// bump-allocates and bulk-frees using duckdb::ArenaAllocator and has a tunable capacity.
// The ArenaAllocator owns all PostingsSegments, hence we use raw pointer members.
struct PostingsList {
    // The head pointer is used during lookup (converting the PostingsList to a std::vector).
    PostingsSegment* head = nullptr;
    // The tail pointer is used for appending to the PostingsLists.
    PostingsSegment* tail = nullptr;
    //! Total number of row IDs in this postings list. All segments but the tail are full.
    // Maximum size is 122880 < 2^17 because we seal the memtable when one row group is full.
    std::uint32_t size = 0;

    //! Debug-only structural audit of the invariants
    void Verify() const;
};
} // namespace logsearch::inverted_index

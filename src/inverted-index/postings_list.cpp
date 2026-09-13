#include "postings_list.hpp"

#include "assertions.hpp"

#include <cstdint>
#include <duckdb/common/assert.hpp>
#include <duckdb/common/constants.hpp>
#include <duckdb/common/typedefs.hpp>

namespace logsearch::inverted_index {

// NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init): entries uninitialized by design without assertions
PostingsSegment::PostingsSegment() noexcept {
#ifdef LS_ASSERTS_ENABLED
    // `entries` is filled with a sentinel only when assertions are enabled, so Verify and Insert's write-once check can
    // distinguish written slots (any valid row ID, including 0) from never-written ones.
    entries.fill(POSTINGS_UNWRITTEN_SENTINEL);
#endif
}

void PostingsList::Verify() const {
#ifdef LS_ASSERTS_ENABLED
    if (size == 0) {
        D_ASSERT(head == nullptr);
        D_ASSERT(tail == nullptr);
        return;
    }

    D_ASSERT(head != nullptr);
    D_ASSERT(tail != nullptr);

    constexpr std::uint32_t capacity = PostingsSegment::Capacity();
    // All segments but the tail are full, so `size` alone determines the chain length and the tail's fill.
    const std::uint32_t expected_segments = (size + capacity - 1) / capacity;
    const std::uint32_t tail_fill = ((size - 1) % capacity) + 1;

    std::uint32_t segment_count = 0;
    std::uint32_t total_entries = 0;
    duckdb::row_t prev_row_id = POSTINGS_UNWRITTEN_SENTINEL;
    for (const PostingsSegment* segment = head; segment != nullptr; segment = segment->next) {
        segment_count++;
        D_ASSERT(segment_count <= expected_segments);

        const bool is_tail = segment->next == nullptr;
        // The tail is the unique segment with next == nullptr
        D_ASSERT(is_tail == (segment == tail));
        const std::uint32_t fill = is_tail ? tail_fill : capacity;

        // Every written slot holds a real row ID (never the sentinel), strictly increasing across the whole list.
        for (std::uint32_t i = 0; i < fill; i++) {
            const duckdb::row_t row_id = segment->entries[i];
            // A full segment has no never-written holes
            D_ASSERT(row_id != POSTINGS_UNWRITTEN_SENTINEL);
            // We expect committed row IDs, hence the upper bound is MAX_ROW_ID, not MAX_ROW_ID_LOCAL.
            D_ASSERT(row_id >= 0 && row_id <= duckdb::MAX_ROW_ID);
            D_ASSERT(prev_row_id < row_id);
            prev_row_id = row_id;
        }
        total_entries += fill;

        // Slots past the fill were never written, so they still hold the sentinel.
        if (is_tail) {
            for (std::uint32_t i = fill; i < capacity; i++) {
                D_ASSERT(segment->entries[i] == POSTINGS_UNWRITTEN_SENTINEL);
            }
        }
    }

    D_ASSERT(segment_count == expected_segments);
    D_ASSERT(total_entries == size);
#endif
}

} // namespace logsearch::inverted_index

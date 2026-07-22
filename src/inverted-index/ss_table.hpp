#pragma once

#include <cstddef>
#include <cstdint>
#include <duckdb/common/shared_ptr.hpp>
#include <duckdb/common/typedefs.hpp>
#include <duckdb/storage/buffer/block_handle.hpp>
#include <string_view>
#include <type_traits>
#include <vector>

namespace duckdb {
class BufferManager;
} // namespace duckdb

namespace logsearch::inverted_index {

//! Magic byte for an easy check if the bytes we read are valid.
static constexpr std::uint32_t SSTABLE_MAGIC = 0x4C535354; // "LSST" = Logsearch SSTable
//! Version tag, eventually needed for backward compatibility.
static constexpr std::uint32_t SSTABLE_VERSION = 1;

//! Fixed-size header that contains metadata about the index partition as well as offsets of other block regions.
struct SSTableHeader {
    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    //! Maximum number of bytes that are written to one block.
    std::uint64_t block_size = 0;
    //! Total number of bytes of the SSTable.
    std::uint64_t total_size = 0;
    //! Number of terms stored in the dictionary of this index partition.
    std::uint64_t num_terms = 0;
    //! Logical offset of the dictionary region (SSTableDictEntry array).
    std::uint64_t dict_offset = 0;
    //! Logical offset of the string pool.
    std::uint64_t strings_offset = 0;
    //! Logical offset of the postings region (8-byte aligned).
    std::uint64_t postings_offset = 0;
    //! Lowest row ID in this index partition for which there are associated terms.
    duckdb::row_t min_row_id = 0;
    //! Highest row ID in this index partition for which there are associated terms.
    duckdb::row_t max_row_id = 0;
};

// Header must be padding-free and trivially copyable because it gets memcopied verbatim.
static_assert(std::has_unique_object_representations_v<SSTableHeader>,
              "SSTableHeader must have no padding (for deterministic byte image)");
static_assert(std::is_standard_layout_v<SSTableHeader>,
              "SSTableHeader must be standard-layout (for predictable byte layout)");

//! Fixed-size dictionary entry. Fixed size keeps the array directly binary-searchable (no sparse
//! index needed). The term string bytes live in the string pool, the postings in the postings region of the SSTable.
// If this struct ever grows bigger, we might want to split this into multiple
// arrays to keep the jumps that we need to do small and touch as few as possible blocks during binary search.
struct SSTableDictEntry {
    //! Offset into the string pool (relative to SSTableHeader::strings_offset) where to find the term
    std::uint32_t term_offset = 0;
    //! Byte length of the term
    std::uint32_t term_length = 0;
    //! Byte offset of this term's postings list, relative to SSTableHeader::postings_offset.
    std::uint32_t postings_offset = 0;
    //! Number of row IDs in this term's postings list. Smaller or equal to row group size.
    std::uint32_t postings_count = 0;
};

// Dict entries must be padding-free and trivially copyable because they get memcopied verbatim.
static_assert(std::has_unique_object_representations_v<SSTableDictEntry>,
              "SSTableDictEntry must have no padding (for deterministic byte image)");
static_assert(std::is_standard_layout_v<SSTableDictEntry>,
              "SSTableDictEntry must be standard-layout (for predictable byte layout)");

static_assert(sizeof(duckdb::row_t) == sizeof(std::int64_t), "Postings list layout assumes 8-byte row_t");

//! Immutable index partition for one row group, storing the dictionary and postings lists.
class SSTable {
    friend class SSTableBuilder;

public:
    // Non-copyable (owns block handles + a BufferManager reference)
    SSTable(const SSTable&) = delete;
    SSTable& operator=(const SSTable&) = delete;
    // Movable so it can live in a std::vector.
    SSTable(SSTable&&) = default;
    // BufferManager& member can't be rebound and move-constructor is enough for std::vector.
    SSTable& operator=(SSTable&&) = delete;
    ~SSTable() = default;

    //! Return `term`'s postings in ascending row-ID order, or an empty vector if the term is absent.
    [[nodiscard]] std::vector<duckdb::row_t> Lookup(std::string_view search_term) const;

    //! Smallest row ID in this SSTable (inclusive). SSTables are never empty, so this is always a real row ID.
    [[nodiscard]] duckdb::row_t MinRowId() const {
        return header_.min_row_id;
    }

    //! Largest row ID in this SSTable (inclusive).
    [[nodiscard]] duckdb::row_t MaxRowId() const {
        return header_.max_row_id;
    }

    //! Number of terms in the dictionary of this SSTable.
    [[nodiscard]] std::size_t NumTerms() const {
        return static_cast<std::size_t>(header_.num_terms);
    }

    //! Number of buffer-managed blocks the SSTable byte image is spread across.
    [[nodiscard]] std::size_t NumBlocks() const {
        return blocks_.size();
    }

    //! Debug-only structural audit of the invariants.
    void Verify() const;

private:
    //! Pins the block covering a logical offset and holds it across reads, re-pinning only when an access crosses
    //! into a different block.
    class PinnedBlockCache;

    SSTable(duckdb::BufferManager& buffer_manager, std::vector<duckdb::shared_ptr<duckdb::BlockHandle>>&& blocks,
            duckdb::idx_t block_size, const SSTableHeader& header);

    //! Lexicographically compare the stored term at [offset, offset+stored_length) (in the blocks) against
    //! `comparatum`, without copying. Negative if stored < comparatum, 0 if equal, positive if stored > comparatum.
    //! Reads through `block_reader`.
    [[nodiscard]] int CompareTermAt(PinnedBlockCache& block_reader, std::uint64_t offset, std::uint32_t stored_length,
                                    std::string_view comparatum) const;

    duckdb::BufferManager& bm_;
    std::vector<duckdb::shared_ptr<duckdb::BlockHandle>> blocks_;
    duckdb::idx_t block_size_;
    SSTableHeader header_;
};

} // namespace logsearch::inverted_index

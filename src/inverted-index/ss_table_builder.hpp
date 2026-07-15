#pragma once

#include <duckdb/common/optional_idx.hpp>
#include <duckdb/common/typedefs.hpp>
#include <string_view>
#include <vector>

namespace duckdb {
class BufferManager;
} // namespace duckdb

namespace logsearch::inverted_index {

class SSTable;

//! Input to the Build method. A term from the dictionary of the memtable with its associated postings list in ascending
//! order and without duplicates. The lifetimes of both `term` and `postings` are tied to the lifetime of the memtable
//! (or specifically the underlying arena).
struct TermPostings {
    std::string_view term;
    const std::vector<duckdb::row_t>* postings = nullptr;
};

class SSTableBuilder {
public:
    //! Build an SSTable from per-term postings of the memtable.
    //! The final SSTable is backed by buffer-managed blocks of `block_size` bytes.
    // The blocks are allocated with can_destroy=false, so they survive eviction by spilling to a temp file.
    // Later we need to add real persistence.
    static SSTable Build(duckdb::BufferManager& buffer_manager, std::vector<TermPostings>&& terms,
                         duckdb::optional_idx block_size_overwrite = duckdb::optional_idx::Invalid());
};
} // namespace logsearch::inverted_index

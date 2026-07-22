#include "multi_block_writer.hpp"

#include <algorithm>
#include <cstring>
#include <duckdb/common/assert.hpp>
#include <duckdb/common/enums/memory_tag.hpp>
#include <duckdb/common/query_context.hpp>
#include <duckdb/common/shared_ptr.hpp>
#include <duckdb/common/typedefs.hpp>
#include <duckdb/storage/buffer/block_handle.hpp>
#include <duckdb/storage/buffer_manager.hpp>
#include <exception>
#include <utility>
#include <vector>

namespace logsearch::storage {

MultiBlockWriter::MultiBlockWriter(duckdb::BufferManager& buffer_manager, const duckdb::idx_t block_size,
                                   const duckdb::QueryContext context)
    : bm_(buffer_manager), block_size_(block_size), context_(context) {
    D_ASSERT(block_size_ > 0);
}

void MultiBlockWriter::WriteData(duckdb::const_data_ptr_t buffer, duckdb::idx_t write_size) {
    D_ASSERT(!finished_);
    bytes_written_ += write_size;
    while (write_size > 0) {
        EnsureSpace();
        const duckdb::idx_t chunk = std::min<duckdb::idx_t>(write_size, block_size_ - current_offset_in_block_);
        std::memcpy(current_block_.GetDataMutable() + current_offset_in_block_, buffer, chunk);
        buffer += chunk;
        current_offset_in_block_ += chunk;
        D_ASSERT(current_offset_in_block_ <= block_size_);
        write_size -= chunk;
    }
}

std::vector<duckdb::shared_ptr<duckdb::BlockHandle>> MultiBlockWriter::Finish() {
    D_ASSERT(!finished_);
    if (current_block_.IsValid()) {
        blocks_.push_back(current_block_.GetBlockHandle());
        current_block_.Destroy();
    }
    finished_ = true;
    return std::move(blocks_);
}

MultiBlockWriter::~MultiBlockWriter() {
    // The blocks written should have been handed over in Finish().
    D_ASSERT(finished_ || std::uncaught_exceptions() > 0);
    std::ignore = std::uncaught_exceptions(); // To use <exception> include in release mode
}

void MultiBlockWriter::EnsureSpace() {
    // We allocate with `can_destroy=false` so that the blocks stay in memory or temp space.
    // Only the block currently being written is pinned.
    // Finished blocks are kept alive because we store their BlockHandles in `blocks_`.
    if (!current_block_.IsValid()) {
        current_block_ = bm_.Allocate(context_, duckdb::MemoryTag::EXTENSION, block_size_, false);
        current_offset_in_block_ = 0;
    } else if (current_offset_in_block_ == block_size_) {
        blocks_.push_back(current_block_.GetBlockHandle());
        current_block_ = bm_.Allocate(context_, duckdb::MemoryTag::EXTENSION, block_size_, false);
        current_offset_in_block_ = 0;
    }
    // Allocate returns a buffer of at least block_size_ (it may round up). We only ever write block_size_ bytes.
    D_ASSERT(current_block_.GetFileBuffer().Size() >= block_size_);
}

} // namespace logsearch::storage

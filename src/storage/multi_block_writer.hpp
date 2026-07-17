#pragma once

#include <duckdb/common/query_context.hpp>
#include <duckdb/common/serializer/write_stream.hpp>
#include <duckdb/common/shared_ptr.hpp>
#include <duckdb/common/typedefs.hpp>
#include <duckdb/storage/buffer/block_handle.hpp>
#include <duckdb/storage/buffer/buffer_handle.hpp>
#include <vector>

namespace duckdb {
class BufferManager;
} // namespace duckdb

namespace logsearch::storage {

//! A WriteStream that streams bytes into a sequence of buffer-managed blocks, opening a new block whenever the
//! current one fills.
// TODO: Add persistence of blocks
class MultiBlockWriter final : public duckdb::WriteStream {
public:
    //! `context` attributes block allocations (and possible eviction/spill I/O) to the running query. The writer must
    //! not outlive the query it was created for.
    MultiBlockWriter(duckdb::BufferManager& buffer_manager, duckdb::idx_t block_size,
                     duckdb::QueryContext context = duckdb::QueryContext());

    MultiBlockWriter(const MultiBlockWriter&) = delete;
    MultiBlockWriter& operator=(const MultiBlockWriter&) = delete;
    MultiBlockWriter(MultiBlockWriter&&) = delete;
    MultiBlockWriter& operator=(MultiBlockWriter&&) = delete;
    ~MultiBlockWriter() override;

    //! Appends `write_size` bytes from buffer, spanning into new blocks as needed.
    void WriteData(duckdb::const_data_ptr_t buffer, duckdb::idx_t write_size) override;

    //! Hands over all BlockHandles of blocks created during writing (the caller takes ownership).
    [[nodiscard]] std::vector<duckdb::shared_ptr<duckdb::BlockHandle>> Finish();

    //! Total number of bytes written.
    [[nodiscard]] duckdb::idx_t BytesWritten() const {
        return bytes_written_;
    }

private:
    //! Guarantee the current block has room for at least one more byte, opening a new block if needed.
    void EnsureSpace();

    duckdb::BufferManager& bm_;
    duckdb::QueryContext context_;
    duckdb::idx_t block_size_;
    duckdb::BufferHandle current_block_;
    duckdb::idx_t current_offset_in_block_ = 0;
    duckdb::idx_t bytes_written_ = 0;
    bool finished_ = false;
    std::vector<duckdb::shared_ptr<duckdb::BlockHandle>> blocks_;
};

} // namespace logsearch::storage

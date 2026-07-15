#include "storage/multi_block_writer.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <duckdb/common/serializer/memory_stream.hpp>
#include <duckdb/common/shared_ptr.hpp>
#include <duckdb/common/typedefs.hpp>
#include <duckdb/main/database.hpp>
#include <duckdb/storage/buffer/block_handle.hpp>
#include <duckdb/storage/buffer/buffer_handle.hpp>
#include <duckdb/storage/buffer_manager.hpp>
#include <vector>

using logsearch::storage::MultiBlockWriter;

namespace {

// Unit tests have no DatabaseInstance by default, so spin up an in-memory DuckDB and borrow its BufferManager.
class BufferManagerFixture {
public:
    duckdb::DuckDB db{nullptr};
    duckdb::BufferManager& bm = duckdb::BufferManager::GetBufferManager(*db.instance);
};

// Pin each block in turn and concatenate the first `total` bytes into one contiguous buffer.
std::vector<std::uint8_t> Reassemble(duckdb::BufferManager& bm,
                                     const std::vector<duckdb::shared_ptr<duckdb::BlockHandle>>& blocks,
                                     duckdb::idx_t block_size, duckdb::idx_t total) {
    std::vector<std::uint8_t> out(total);
    duckdb::idx_t copied = 0;
    for (const auto& block : blocks) {
        if (copied >= total) {
            break;
        }
        duckdb::shared_ptr<duckdb::BlockHandle> handle = block; // Pin takes a non-const shared_ptr&
        const duckdb::BufferHandle pinned = bm.Pin(handle);
        const duckdb::idx_t chunk = std::min<duckdb::idx_t>(block_size, total - copied);
        std::memcpy(out.data() + copied, pinned.Ptr(), chunk);
        copied += chunk;
    }
    return out;
}

} // namespace

TEST_CASE_METHOD(BufferManagerFixture, "MultiBlockWriter chunks a byte stream identically to MemoryStream",
                 "[storage][multi_block_writer]") {
    constexpr duckdb::idx_t BLOCK = 32; // tiny, to force several blocks
    duckdb::MemoryStream oracle;
    MultiBlockWriter writer(bm, BLOCK);

    // Same sequence of typed values, a raw blob, and a wide value into both sinks.
    for (std::uint32_t i = 0; i < 20; i++) {
        oracle.Write<std::uint32_t>(i);
        writer.Write<std::uint32_t>(i);
    }
    const std::vector<std::uint8_t> blob = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    oracle.WriteData(blob.data(), blob.size());
    writer.WriteData(blob.data(), blob.size());
    oracle.Write<std::uint64_t>(0xDEADBEEFCAFEULL);
    writer.Write<std::uint64_t>(0xDEADBEEFCAFEULL);

    const std::vector<duckdb::shared_ptr<duckdb::BlockHandle>> blocks = writer.Finish();
    const duckdb::idx_t total = writer.BytesWritten();

    CHECK(total == oracle.GetPosition());
    CHECK(blocks.size() > 1); // dispersed across multiple blocks
    const std::vector<std::uint8_t> reassembled = Reassemble(bm, blocks, BLOCK, total);
    REQUIRE(reassembled.size() == total);
    CHECK(std::memcmp(reassembled.data(), oracle.GetData(), total) == 0);
}

TEST_CASE_METHOD(BufferManagerFixture, "Values written through MultiBlockWriter read back via MemoryStream",
                 "[storage][multi_block_writer]") {
    constexpr duckdb::idx_t BLOCK = 24;
    MultiBlockWriter writer(bm, BLOCK);
    for (std::uint64_t i = 0; i < 30; i++) {
        writer.Write<std::uint64_t>(i * 1000);
    }
    const std::vector<duckdb::shared_ptr<duckdb::BlockHandle>> blocks = writer.Finish();
    std::vector<std::uint8_t> reassembled = Reassemble(bm, blocks, BLOCK, writer.BytesWritten());

    duckdb::MemoryStream reader(reassembled.data(), reassembled.size()); // non-owning read over the bytes
    for (std::uint64_t i = 0; i < 30; i++) {
        CHECK(reader.Read<std::uint64_t>() == i * 1000);
    }
}

TEST_CASE_METHOD(BufferManagerFixture, "Writing an exact multiple of block_size uses full blocks",
                 "[storage][multi_block_writer]") {
    constexpr duckdb::idx_t BLOCK = 64;
    MultiBlockWriter writer(bm, BLOCK);
    const std::vector<std::uint8_t> data(BLOCK * 3, 0xAB); // exactly three full blocks
    writer.WriteData(data.data(), data.size());
    const std::vector<duckdb::shared_ptr<duckdb::BlockHandle>> blocks = writer.Finish();

    CHECK(writer.BytesWritten() == BLOCK * 3);
    CHECK(blocks.size() == 3);
    CHECK(Reassemble(bm, blocks, BLOCK, writer.BytesWritten()) == data);
}

TEST_CASE_METHOD(BufferManagerFixture, "A write smaller than block_size uses a single block",
                 "[storage][multi_block_writer]") {
    constexpr duckdb::idx_t BLOCK = 256;
    MultiBlockWriter writer(bm, BLOCK);
    const std::vector<std::uint8_t> data = {9, 8, 7, 6, 5};
    writer.WriteData(data.data(), data.size());
    const std::vector<duckdb::shared_ptr<duckdb::BlockHandle>> blocks = writer.Finish();

    CHECK(blocks.size() == 1);
    CHECK(writer.BytesWritten() == data.size());
    CHECK(Reassemble(bm, blocks, BLOCK, data.size()) == data);
}

TEST_CASE_METHOD(BufferManagerFixture, "A single WriteData larger than a block spans blocks",
                 "[storage][multi_block_writer]") {
    constexpr duckdb::idx_t BLOCK = 16;
    MultiBlockWriter writer(bm, BLOCK);
    std::vector<std::uint8_t> data(100);
    for (std::size_t i = 0; i < data.size(); i++) {
        data[i] = static_cast<std::uint8_t>(i);
    }
    writer.WriteData(data.data(), data.size());
    const std::vector<duckdb::shared_ptr<duckdb::BlockHandle>> blocks = writer.Finish();

    CHECK(blocks.size() > 1);
    CHECK(Reassemble(bm, blocks, BLOCK, writer.BytesWritten()) == data);
}

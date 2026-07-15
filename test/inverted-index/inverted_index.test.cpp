#include "inverted-index/inverted_index.hpp"

#include "inverted-index/memtable.hpp"
#include "inverted-index/ss_table.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_vector.hpp>
#include <duckdb/common/typedefs.hpp>
#include <duckdb/main/database.hpp>
#include <duckdb/storage/buffer_manager.hpp>
#include <vector>

using Catch::Matchers::Equals;
using duckdb::row_t;
using logsearch::inverted_index::InvertedIndex;
using logsearch::inverted_index::Memtable;
using logsearch::inverted_index::SSTable;

namespace {
// Unit tests have no DatabaseInstance by default, so spin up an in-memory DuckDB and borrow its BufferManager
// (backs the memtable arena via its allocator, and the sealed SSTables' blocks).
class BufferManagerFixture {
public:
    duckdb::DuckDB db{nullptr};
    duckdb::BufferManager& bm = duckdb::BufferManager::GetBufferManager(*db.instance);
};
} // namespace

TEST_CASE_METHOD(BufferManagerFixture, "Seal produces an SSTable matching the memtable", "[inverted_index][seal]") {
    Memtable memtable{bm.GetBufferAllocator()};
    memtable.Insert("alpha", 1);
    memtable.Insert("alpha", 5);
    memtable.Insert("beta", 2);
    memtable.Insert("gamma", 3);
    memtable.Insert("gamma", 3); // duplicate row ID within a document -> dropped
    memtable.Insert("gamma", 9);

    const SSTable sst = memtable.Seal(bm);

    CHECK(sst.NumTerms() == memtable.DictionarySize());
    // Differential: the sealed SSTable answers exactly like the memtable it came from.
    CHECK_THAT(sst.Lookup("alpha"), Equals(memtable.Lookup("alpha")));
    CHECK_THAT(sst.Lookup("beta"), Equals(memtable.Lookup("beta")));
    CHECK_THAT(sst.Lookup("gamma"), Equals(memtable.Lookup("gamma")));
    CHECK_THAT(sst.Lookup("alpha"), Equals(std::vector<row_t>{1, 5}));
    CHECK_THAT(sst.Lookup("gamma"), Equals(std::vector<row_t>{3, 9}));
    CHECK(sst.Lookup("absent").empty());
    sst.Verify();
}

TEST_CASE_METHOD(BufferManagerFixture, "Lookup on a single-partition index needs no sealed SSTables",
                 "[inverted_index]") {
    InvertedIndex index{bm, /*row_group_size=*/100};
    index.Insert("x", 0);
    index.Insert("x", 1);
    index.Insert("y", 2);

    CHECK(index.NumSealedPartitions() == 0);
    CHECK_THAT(index.Lookup("x"), Equals(std::vector<row_t>{0, 1}));
    CHECK_THAT(index.Lookup("y"), Equals(std::vector<row_t>{2}));
    CHECK(index.Lookup("z").empty());
}

TEST_CASE_METHOD(BufferManagerFixture, "Lookup merges sealed partitions and the memtable in ascending order",
                 "[inverted_index]") {
    // Tiny row group so crossing a boundary (and thus sealing) happens after only a few inserts.
    InvertedIndex index{bm, /*row_group_size=*/4};

    // Partition 0 (rows 0..3).
    index.Insert("a", 0);
    index.Insert("a", 1);
    index.Insert("b", 2);
    // Row 4 starts row group 1 -> seals partition 0.
    index.Insert("a", 4);
    index.Insert("c", 6);
    // Row 8 starts row group 2 -> seals partition 1. "a" now lives in both sealed partitions and the memtable.
    index.Insert("a", 8);

    CHECK(index.NumSealedPartitions() == 2);
    CHECK_THAT(index.Lookup("a"), Equals(std::vector<row_t>{0, 1, 4, 8})); // p0{0,1} + p1{4} + memtable{8}
    CHECK_THAT(index.Lookup("b"), Equals(std::vector<row_t>{2}));          // p0 only
    CHECK_THAT(index.Lookup("c"), Equals(std::vector<row_t>{6}));          // p1 only
    CHECK(index.Lookup("absent").empty());
}

TEST_CASE_METHOD(BufferManagerFixture, "A row-ID jump across several row groups seals only once", "[inverted_index]") {
    InvertedIndex index{bm, /*row_group_size=*/4};
    index.Insert("a", 0);  // row group 0
    index.Insert("a", 20); // jumps to row group 5 -> seals partition 0 exactly once, no empty partitions in between

    CHECK(index.NumSealedPartitions() == 1);
    CHECK_THAT(index.Lookup("a"), Equals(std::vector<row_t>{0, 20}));
}

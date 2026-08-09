#include "inverted-index/inverted_index.hpp"

#include "inverted-index/memtable.hpp"
#include "inverted-index/ss_table.hpp"
#include "test-utils/fixtures.hpp"

#include <algorithm>
#include <catch2/catch_get_random_seed.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_vector.hpp>
#include <cstddef>
#include <duckdb/common/typedefs.hpp>
#include <duckdb/main/database.hpp>
#include <duckdb/storage/buffer_manager.hpp>
#include <numeric>
#include <random>
#include <string>
#include <utility>
#include <vector>

using Catch::Matchers::Equals;
using duckdb::row_t;
using logsearch::inverted_index::InvertedIndex;
using logsearch::inverted_index::Memtable;
using logsearch::inverted_index::SSTable;

TEST_CASE_METHOD(BufferManagerFixture, "Moving an index carries over sealed partitions and the active Memtable",
                 "[inverted_index]") {
    InvertedIndex source{bm, /*row_group_size=*/4};
    source.Insert("a", 0); // sealed when row 4 opens the next row group
    source.Insert("a", 4); // still in the active Memtable

    const InvertedIndex moved{std::move(source)};
    CHECK(moved.NumSealedPartitions() == 1);
    CHECK_THAT(moved.Lookup("a"), Equals(std::vector<row_t>{0, 4}));
}

TEST_CASE_METHOD(BufferManagerFixture, "Seal produces an SSTable matching the memtable", "[inverted_index][seal]") {
    Memtable memtable{bm.GetBufferAllocator()};
    memtable.Insert("alpha", 1);
    memtable.Insert("alpha", 5);
    memtable.Insert("beta", 2);
    memtable.Insert("gamma", 3);
    memtable.Insert("gamma", 3); // duplicate row ID within a document gets dropped
    memtable.Insert("gamma", 9);

    // Calling Seal() does not reset the Memtable
    const SSTable sst = memtable.Seal(bm);

    CHECK(sst.NumTerms() == memtable.DictionarySize());
    // The sealed SSTable answers for Lookup exactly like the Memtable it came from.
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

TEST_CASE_METHOD(BufferManagerFixture, "Lookup merges sealed partitions and the Memtable in ascending order",
                 "[inverted_index]") {
    // Tiny row group so crossing a boundary (and thus sealing) happens after only a few inserts.
    InvertedIndex index{bm, /*row_group_size=*/4};

    // Partition 0 (rows 0..3).
    index.Insert("a", 0);
    index.Insert("a", 1);
    index.Insert("b", 2);
    // Row 4 starts row group 1 and seals partition 0.
    index.Insert("a", 4);
    index.Insert("c", 6);
    // Row 8 starts row group 2 and seals partition 1.
    // "a" now lives in both sealed partitions and the Memtable.
    index.Insert("a", 8);

    CHECK(index.NumSealedPartitions() == 2);
    CHECK_THAT(index.Lookup("a"), Equals(std::vector<row_t>{0, 1, 4, 8})); // p0{0,1} + p1{4} + memtable{8}
    CHECK_THAT(index.Lookup("b"), Equals(std::vector<row_t>{2}));          // p0 only
    CHECK_THAT(index.Lookup("c"), Equals(std::vector<row_t>{6}));          // p1 only
    CHECK(index.Lookup("absent").empty());
}

TEST_CASE_METHOD(BufferManagerFixture, "PairwiseMerge concatenates indexes covering disjoint row ranges",
                 "[inverted_index]") {
    InvertedIndex earlier{bm, /*row_group_size=*/4};
    earlier.Insert("a", 0);
    earlier.Insert("b", 2);
    InvertedIndex later{bm, /*row_group_size=*/4};
    later.Insert("a", 4);
    later.Insert("c", 9);

    // Merging in either direction has to produce the same, ascending result.
    const bool merge_later_into_earlier = GENERATE(false, true);
    InvertedIndex& target = merge_later_into_earlier ? earlier : later;
    InvertedIndex& source = merge_later_into_earlier ? later : earlier;
    target.PairwiseMerge(std::move(source), context);

    CHECK_THAT(target.Lookup("a"), Equals(std::vector<row_t>{0, 4}));
    CHECK_THAT(target.Lookup("b"), Equals(std::vector<row_t>{2}));
    CHECK_THAT(target.Lookup("c"), Equals(std::vector<row_t>{9}));
    // Both active Memtables were sealed: row groups 0, 1 and 2.
    CHECK(target.NumSealedPartitions() == 3);
}

TEST_CASE_METHOD(BufferManagerFixture, "Merging an empty index only seals the target", "[inverted_index]") {
    InvertedIndex index{bm, /*row_group_size=*/4};
    index.Insert("a", 0);
    InvertedIndex empty{bm, /*row_group_size=*/4};

    index.PairwiseMerge(std::move(empty), context);
    CHECK(index.NumSealedPartitions() == 1);
    CHECK_THAT(index.Lookup("a"), Equals(std::vector<row_t>{0}));

    // An empty target stays empty and gains the other index's content.
    InvertedIndex target{bm, /*row_group_size=*/4};
    target.PairwiseMerge(std::move(index), context);
    CHECK_THAT(target.Lookup("a"), Equals(std::vector<row_t>{0}));
}

TEST_CASE_METHOD(BufferManagerFixture, "Merging many indexes in arbitrary order keeps the postings ascending",
                 "[inverted_index]") {
    // One index per row group. Combine folds tasks in whatever order they finish, so pick a random one here.
    constexpr duckdb::idx_t row_group_size = 4;
    constexpr duckdb::idx_t partition_count = 8;
    std::vector<InvertedIndex> parts;
    std::vector<row_t> expected;
    for (duckdb::idx_t i = 0; i < partition_count; i++) {
        InvertedIndex part{bm, row_group_size};
        const auto row_id = static_cast<row_t>(i) * static_cast<row_t>(row_group_size);
        part.Insert("shared", row_id);
        parts.push_back(std::move(part));
        expected.push_back(row_id);
    }

    // InvertedIndex is not move-assignable, so shuffle a permutation rather than `parts` itself.
    std::vector<std::size_t> order(parts.size());
    std::iota(order.begin(), order.end(), 0);
    std::mt19937 rng(Catch::getSeed());
    std::shuffle(order.begin(), order.end(), rng);
    CAPTURE(order);

    InvertedIndex target{bm, row_group_size};
    for (const std::size_t i : order) {
        target.PairwiseMerge(std::move(parts[i]), context);
    }

    CHECK(target.NumSealedPartitions() == partition_count);
    CHECK_THAT(target.Lookup("shared"), Equals(expected));
}

TEST_CASE_METHOD(BufferManagerFixture, "Merging large interleaved indexes keeps every posting list ascending",
                 "[inverted_index]") {
    // Mirrors what a parallel build produces: each task owns several row groups, and the tasks' row groups
    // interleave, so no task's partitions form a contiguous run of the merged result.
    // 3 task indexes, interleaved row groups -> row groups per index: {0,3,6,9} / {1,4,7,10} / {2,5,8,11}.
    constexpr duckdb::idx_t row_group_size = 8192;
    constexpr duckdb::idx_t task_count = 3;
    constexpr duckdb::idx_t row_group_count = 12;
    constexpr auto row_count = static_cast<row_t>(row_group_count * row_group_size);

    std::vector<InvertedIndex> tasks;
    tasks.reserve(task_count);
    for (duckdb::idx_t task = 0; task < task_count; task++) {
        InvertedIndex index{bm, row_group_size};
        for (duckdb::idx_t group = task; group < row_group_count; group += task_count) {
            const auto first_row = static_cast<row_t>(group * row_group_size);
            const auto end_row = first_row + static_cast<row_t>(row_group_size);
            for (row_t row_id = first_row; row_id < end_row; row_id++) {
                // "all" and "rg<n>" are one dictionary entry per partition. Only the per-row term adds one per row.
                // That is what grows a sealed partition beyond a single block, so Lookup has to re-pin mid-search
                // here instead of reading everything from one block.
                index.Insert("all", row_id, context);
                index.Insert("rg" + std::to_string(group), row_id, context);
                index.Insert("row" + std::to_string(row_id), row_id, context);
            }
        }
        tasks.push_back(std::move(index));
    }

    InvertedIndex merged{bm, row_group_size};
    for (InvertedIndex& task : tasks) {
        merged.PairwiseMerge(std::move(task), context);
    }
    CHECK(merged.NumSealedPartitions() == row_group_count);

    // A term in every row: one posting per row, ascending and duplicate-free across all partitions.
    const std::vector<row_t> all_res = merged.Lookup("all", context);
    REQUIRE(all_res.size() == row_group_count * row_group_size);
    CHECK(all_res.front() == 0);
    CHECK(all_res.back() == row_count - 1);
    CHECK(std::is_sorted(all_res.begin(), all_res.end()));
    CHECK(std::adjacent_find(all_res.begin(), all_res.end()) == all_res.end());

    // A term confined to one row group, whose owning task holds it between row groups of the two other tasks.
    constexpr duckdb::idx_t probed_group = 5;
    std::vector<row_t> expected_group;
    expected_group.reserve(row_group_size);
    for (duckdb::idx_t i = 0; i < row_group_size; i++) {
        expected_group.push_back(static_cast<row_t>((probed_group * row_group_size) + i));
    }
    CHECK_THAT(merged.Lookup("rg" + std::to_string(probed_group), context), Equals(expected_group));

    // A term occurring exactly once, in the last row group.
    constexpr row_t unique_row = row_count - 3;
    CHECK_THAT(merged.Lookup("row" + std::to_string(unique_row), context), Equals(std::vector<row_t>{unique_row}));

    CHECK(merged.Lookup("absent", context).empty());
}

TEST_CASE_METHOD(BufferManagerFixture, "Inserting after a merge continues past the merged rows", "[inverted_index]") {
    InvertedIndex target{bm, /*row_group_size=*/4};
    InvertedIndex source{bm, /*row_group_size=*/4};
    source.Insert("a", 5);
    target.PairwiseMerge(std::move(source), context);

    // The merged partition stays sealed after a PairwiseMerge even when a new row of the same row group is inserted.
    target.Insert("a", 6);
    CHECK_THAT(target.Lookup("a"), Equals(std::vector<row_t>{5, 6}));
    CHECK(target.NumSealedPartitions() == 1);
}

TEST_CASE_METHOD(BufferManagerFixture, "A row-ID jump across several row groups seals only once", "[inverted_index]") {
    InvertedIndex index{bm, /*row_group_size=*/4};
    index.Insert("a", 0);  // Row group 0
    index.Insert("a", 20); // Jumps to row group 5 -> seals partition 0 exactly once, no empty partitions in between

    CHECK(index.NumSealedPartitions() == 1);
    CHECK_THAT(index.Lookup("a"), Equals(std::vector<row_t>{0, 20}));
}

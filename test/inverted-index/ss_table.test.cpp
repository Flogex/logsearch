#include "inverted-index/ss_table.hpp"

#include "inverted-index/ss_table_builder.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_vector.hpp>
#include <cstddef>
#include <duckdb/common/typedefs.hpp>
#include <duckdb/main/database.hpp>
#include <duckdb/storage/buffer_manager.hpp>
#include <duckdb/storage/storage_info.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using Catch::Matchers::Equals;
using duckdb::row_t;
using logsearch::inverted_index::SSTable;
using logsearch::inverted_index::SSTableBuilder;
using logsearch::inverted_index::TermPostings;

namespace {

// Unit tests have no DatabaseInstance by default, so spin up an in-memory DuckDB and
// borrow its BufferManager. Blocks allocated through it stay in memory (no checkpoint / no disk).
struct BufferManagerFixture {
    duckdb::DuckDB db{nullptr};
    duckdb::BufferManager& bm = duckdb::BufferManager::GetBufferManager(*db.instance);
};

// A small block size (multiple of 8) so even tiny inputs spread across multiple blocks.
constexpr duckdb::idx_t SMALL_BLOCK = 64;

// Owns the term strings and postings vectors so the non-owning TermPostings views handed to Build() stay valid.
class Corpus {
public:
    void Add(std::string term, std::vector<row_t> ids) {
        terms_.push_back(std::move(term));
        postings_.push_back(std::move(ids));
    }

    [[nodiscard]] std::vector<TermPostings> ToVector() const {
        std::vector<TermPostings> vec;
        vec.reserve(terms_.size());
        for (std::size_t i = 0; i < terms_.size(); i++) {
            vec.push_back(TermPostings{std::string_view(terms_[i]), &postings_[i]});
        }
        return vec;
    }

private:
    std::vector<std::string> terms_;
    std::vector<std::vector<row_t>> postings_;
};

// Unique, non-empty, lexicographically-i-ordered term for row id `i` (zero-padded so string order == numeric order).
std::string TermFor(const row_t i) {
    const std::string digits = std::to_string(i);
    return "t" + std::string(6 - digits.size(), '0') + digits;
}

} // namespace

TEST_CASE_METHOD(BufferManagerFixture, "SSTable produces correct Lookup results for single term",
                 "[inverted_index][ss_table]") {
    Corpus corpus;
    corpus.Add("term", {1, 2, 3});
    const SSTable sst = SSTableBuilder::Build(bm, corpus.ToVector(), SMALL_BLOCK);

    REQUIRE(sst.NumTerms() == 1);
    CHECK_THAT(sst.Lookup("term"), Equals(std::vector<row_t>{1, 2, 3}));
    CHECK(sst.Lookup("absent").empty());
    sst.Verify();
}

TEST_CASE_METHOD(BufferManagerFixture, "SSTable produces correct Lookup results for multiple terms",
                 "[inverted_index][ss_table]") {
    // Added out of order on purpose: Build must sort so binary search (and Verify's sorted-dict check) hold.
    Corpus corpus;
    corpus.Add("gamma", {3, 4, 9});
    corpus.Add("alpha", {1, 5});
    corpus.Add("beta", {2});
    const SSTable sst = SSTableBuilder::Build(bm, corpus.ToVector(), SMALL_BLOCK);

    REQUIRE(sst.NumTerms() == 3);
    CHECK_THAT(sst.Lookup("alpha"), Equals(std::vector<row_t>{1, 5}));
    CHECK_THAT(sst.Lookup("beta"), Equals(std::vector<row_t>{2}));
    CHECK_THAT(sst.Lookup("gamma"), Equals(std::vector<row_t>{3, 4, 9}));
    CHECK(sst.Lookup("delta").empty());
    sst.Verify();
}

TEST_CASE_METHOD(BufferManagerFixture, "A term whose postings span many blocks is read back correctly",
                 "[inverted_index][ss_table]") {
    // 200 ascending row IDs = 1600 bytes, far larger than one 64-byte block
    std::vector<row_t> ids;
    ids.reserve(200);
    for (row_t i = 0; i < 200; i++) {
        ids.push_back(i * 2);
    }
    Corpus corpus;
    corpus.Add("t", ids);
    const SSTable sst = SSTableBuilder::Build(bm, corpus.ToVector(), SMALL_BLOCK);

    CHECK(sst.NumBlocks() > 1);
    CHECK_THAT(sst.Lookup("t"), Equals(ids));
}

TEST_CASE_METHOD(BufferManagerFixture, "A postings list that straddles a block boundary is read back correctly",
                 "[inverted_index][ss_table]") {
    // The short leading term "a" pushes the postings region so "b"'s list does not start block-aligned.
    std::vector<row_t> straddling;
    straddling.reserve(20);
    for (row_t i = 0; i < 20; i++) {
        straddling.push_back(3 + (i * 7));
    }
    Corpus corpus;
    corpus.Add("a", {7});
    corpus.Add("b", straddling);
    const SSTable sst = SSTableBuilder::Build(bm, corpus.ToVector(), SMALL_BLOCK);

    CHECK(sst.NumBlocks() > 1);
    CHECK_THAT(sst.Lookup("a"), Equals(std::vector<row_t>{7}));
    CHECK_THAT(sst.Lookup("b"), Equals(straddling));
}

TEST_CASE_METHOD(BufferManagerFixture, "Many terms disperse across blocks and all look up correctly",
                 "[inverted_index][ss_table]") {
    constexpr std::size_t NUM_TERMS = 100;
    Corpus corpus;
    for (row_t i = 0; i < NUM_TERMS; i++) {
        corpus.Add(TermFor(i), {i, i + 2, i + 4});
    }
    const SSTable sst = SSTableBuilder::Build(bm, corpus.ToVector(), SMALL_BLOCK);

    CHECK(sst.NumTerms() == NUM_TERMS);
    CHECK(sst.NumBlocks() > 1);
    for (row_t i = 0; i < NUM_TERMS; i++) {
        CHECK_THAT(sst.Lookup(TermFor(i)), Equals(std::vector<row_t>{i, i + 2, i + 4}));
    }
    CHECK(sst.Lookup("absent").empty());
}

TEST_CASE_METHOD(BufferManagerFixture, "A few thousand terms build and look up across many blocks",
                 "[inverted_index][ss_table]") {
    constexpr std::size_t NUM_TERMS = 10000;
    Corpus corpus;
    for (row_t i = 0; i < NUM_TERMS; i++) {
        corpus.Add(TermFor(i), {i});
    }
    constexpr duckdb::idx_t MEDIUM_BLOCK = 128;
    const SSTable sst = SSTableBuilder::Build(bm, corpus.ToVector(), MEDIUM_BLOCK);

    CHECK(sst.NumTerms() == NUM_TERMS);
    CHECK(sst.NumBlocks() > 1);
    CHECK_THAT(sst.Lookup(TermFor(0)), Equals(std::vector<row_t>{0}));
    CHECK_THAT(sst.Lookup(TermFor(1000)), Equals(std::vector<row_t>{1000}));
    CHECK_THAT(sst.Lookup(TermFor(NUM_TERMS - 1)), Equals(std::vector<row_t>{NUM_TERMS - 1}));
    CHECK(sst.Lookup("absent").empty());
}

TEST_CASE_METHOD(BufferManagerFixture, "MinRowId/MaxRowId return the min and max row IDs in this index partition",
                 "[inverted_index][ss_table]") {
    Corpus corpus;
    corpus.Add("a", {10, 20});
    corpus.Add("b", {5, 30});
    corpus.Add("c", {15, 20});
    const SSTable sst = SSTableBuilder::Build(bm, corpus.ToVector(), SMALL_BLOCK);

    CHECK(sst.MinRowId() == 5);
    CHECK(sst.MaxRowId() == 30);
}

TEST_CASE_METHOD(BufferManagerFixture, "SSTable can handle long terms", "[inverted_index][ss_table]") {
    constexpr std::size_t LONG_LENGTH = 2000;
    Corpus corpus;
    for (int i = 0; i < 10; i++) {
        corpus.Add(std::string(LONG_LENGTH, 'x') + std::to_string(i), {1, 2, 3});
    }
    const SSTable sst = SSTableBuilder::Build(bm, corpus.ToVector(), SMALL_BLOCK);

    CHECK_THAT(sst.Lookup(std::string(LONG_LENGTH, 'x') + "0"), Equals(std::vector<row_t>{1, 2, 3}));
    CHECK_THAT(sst.Lookup(std::string(LONG_LENGTH, 'x') + "5"), Equals(std::vector<row_t>{1, 2, 3}));
    CHECK_THAT(sst.Lookup(std::string(LONG_LENGTH, 'x') + "9"), Equals(std::vector<row_t>{1, 2, 3}));
}

TEST_CASE_METHOD(BufferManagerFixture, "SSTable can handle high row IDs", "[inverted_index][ss_table]") {
    const std::vector<row_t> row_ids = {duckdb::MAX_ROW_ID - 2, duckdb::MAX_ROW_ID - 1, duckdb::MAX_ROW_ID};
    Corpus corpus;
    corpus.Add("t", row_ids);
    const SSTable sst = SSTableBuilder::Build(bm, corpus.ToVector(), SMALL_BLOCK);

    CHECK_THAT(sst.Lookup("t"), Equals(row_ids));
}

TEST_CASE_METHOD(BufferManagerFixture, "Lookup handles terms that are prefixes of one another",
                 "[inverted_index][ss_table]") {
    // Exercises length tiebreak (equal common prefix, differing lengths) in both directions, plus the
    // empty query.
    Corpus corpus;
    corpus.Add("a", {1});
    corpus.Add("ab", {2});
    corpus.Add("abc", {3});
    const SSTable sst = SSTableBuilder::Build(bm, corpus.ToVector(), SMALL_BLOCK);

    CHECK_THAT(sst.Lookup("a"), Equals(std::vector<row_t>{1}));
    CHECK_THAT(sst.Lookup("ab"), Equals(std::vector<row_t>{2}));
    CHECK_THAT(sst.Lookup("abc"), Equals(std::vector<row_t>{3}));
    CHECK(sst.Lookup("abcd").empty()); // extends past the longest stored term
    CHECK(sst.Lookup("ab ").empty());  // shares the "ab" prefix, then diverges
    CHECK(sst.Lookup("").empty());     // empty query: every stored term sorts after it
}

TEST_CASE_METHOD(BufferManagerFixture, "SSTable with 8-byte block size", "[inverted_index][ss_table]") {
    // Maximal fragmentation. The header and dict entries span many blocks, and each row_t sits in its own block.
    constexpr duckdb::idx_t MIN_BLOCK = 8;
    Corpus corpus;
    corpus.Add("alpha", {1, 4, 9});
    corpus.Add("beta", {2, 3});
    corpus.Add("gamma", {5});
    const SSTable sst = SSTableBuilder::Build(bm, corpus.ToVector(), MIN_BLOCK);

    CHECK(sst.NumBlocks() > 1);
    CHECK_THAT(sst.Lookup("alpha"), Equals(std::vector<row_t>{1, 4, 9}));
    CHECK_THAT(sst.Lookup("beta"), Equals(std::vector<row_t>{2, 3}));
    CHECK_THAT(sst.Lookup("gamma"), Equals(std::vector<row_t>{5}));
    CHECK(sst.Lookup("delta").empty());
    sst.Verify();
}

TEST_CASE_METHOD(BufferManagerFixture, "A moved SSTable still looks up correctly", "[inverted_index][ss_table]") {
    Corpus corpus;
    corpus.Add("alpha", {1, 2});
    corpus.Add("beta", {3});
    SSTable original = SSTableBuilder::Build(bm, corpus.ToVector(), SMALL_BLOCK);
    const SSTable moved = std::move(original);

    CHECK_THAT(moved.Lookup("alpha"), Equals(std::vector<row_t>{1, 2}));
    CHECK_THAT(moved.Lookup("beta"), Equals(std::vector<row_t>{3}));
    moved.Verify();
}

TEST_CASE_METHOD(BufferManagerFixture, "SSTable can handle postings list of row group size",
                 "[inverted_index][ss_table]") {
    std::vector<row_t> ids;
    ids.reserve(DEFAULT_ROW_GROUP_SIZE);
    for (row_t i = 0; i < DEFAULT_ROW_GROUP_SIZE; i++) {
        constexpr row_t ARBITRARY_OFFSET = DEFAULT_ROW_GROUP_SIZE * 2; // We assume full row groups
        ids.push_back(i + ARBITRARY_OFFSET);
    }

    Corpus corpus;
    corpus.Add("a", ids);
    corpus.Add("b", ids);
    const SSTable sst = SSTableBuilder::Build(bm, corpus.ToVector());

    REQUIRE(sst.NumTerms() == 2);
    CHECK(sst.Lookup("a").size() == DEFAULT_ROW_GROUP_SIZE);
    CHECK(sst.Lookup("b").size() == DEFAULT_ROW_GROUP_SIZE);
    CHECK(sst.Lookup("absent").empty());
}

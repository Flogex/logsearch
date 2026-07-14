#include "inverted-index/memtable.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_vector.hpp>
#include <duckdb/common/allocator.hpp>
#include <duckdb/common/typedefs.hpp>
#include <vector>

using Catch::Matchers::Equals;
using logsearch::inverted_index::Memtable;

namespace {
class AllocatorFixture {
public:
    // No DatabaseInstance in unit tests, so use the global default allocator
    duckdb::Allocator& allocator = duckdb::Allocator::DefaultAllocator();
};
} // namespace

TEST_CASE_METHOD(AllocatorFixture, "The dictionary of a new Memtable is empty", "[inverted_index][memtable]") {
    const Memtable index{allocator};
    CHECK(index.DictionarySize() == 0);
}

TEST_CASE_METHOD(AllocatorFixture, "Lookup on an absent term returns an empty list", "[inverted_index][memtable]") {
    Memtable index{allocator};
    index.Insert("present", 1);
    CHECK(index.Lookup("absent").empty());
}

TEST_CASE_METHOD(AllocatorFixture,
                 "After inserting a single term from a single document, the Lookup returns that document's row ID",
                 "[inverted_index][memtable]") {
    Memtable index{allocator};
    index.Insert("term", 1);

    CHECK(index.DictionarySize() == 1);
    CHECK_THAT(index.Lookup("term"), Equals(std::vector<duckdb::row_t>{1}));
}

TEST_CASE_METHOD(AllocatorFixture,
                 "After inserting a single term from a three documents, the Lookup returns that documents' row IDs",
                 "[inverted_index][memtable]") {
    Memtable index{allocator};
    index.Insert("term", 1);
    index.Insert("term", 2);
    index.Insert("term", 3);

    CHECK(index.DictionarySize() == 1);
    CHECK_THAT(index.Lookup("term"), Equals(std::vector<duckdb::row_t>{1, 2, 3}));
}

TEST_CASE_METHOD(AllocatorFixture,
                 "After inserting multiple unique terms, the Lookup returns the matching row ID for each",
                 "[inverted_index][memtable]") {
    Memtable index{allocator};

    index.Insert("term1", 1);
    index.Insert("term2", 2);
    index.Insert("term3", 3);

    CHECK(index.DictionarySize() == 3);
    CHECK_THAT(index.Lookup("term1"), Equals(std::vector<duckdb::row_t>{1}));
    CHECK_THAT(index.Lookup("term2"), Equals(std::vector<duckdb::row_t>{2}));
    CHECK_THAT(index.Lookup("term3"), Equals(std::vector<duckdb::row_t>{3}));
}

TEST_CASE_METHOD(AllocatorFixture,
                 "After inserting a term three times from a single documents, the Lookup returns only one row ID",
                 "[inverted_index][memtable]") {
    Memtable index{allocator};
    // The analyzer emits a term once per occurrence, so a term repeated within one row produces the same row ID twice.
    index.Insert("term", 1);
    index.Insert("term", 1);
    index.Insert("term", 1);

    CHECK(index.DictionarySize() == 1);
    CHECK_THAT(index.Lookup("term"), Equals(std::vector<duckdb::row_t>{1}));
}

TEST_CASE_METHOD(AllocatorFixture, "The dictionary can store long terms", "[inverted_index][memtable]") {
    Memtable index{allocator};
    index.Insert("donaudampfschifffahrtsgesellschaftskapitaen", 1);

    CHECK(index.DictionarySize() == 1);
    CHECK_THAT(index.Lookup("donaudampfschifffahrtsgesellschaftskapitaen"), Equals(std::vector<duckdb::row_t>{1}));
}

TEST_CASE_METHOD(AllocatorFixture, "Inserting 122880 row IDs into a single postings list works",
                 "[inverted_index][memtable]") {
    Memtable index{allocator};

    constexpr duckdb::row_t ROW_GROUP_SIZE = 122880;
    for (duckdb::row_t row_id = 0; row_id < ROW_GROUP_SIZE; row_id++) {
        index.Insert("term", row_id);
    }

    CHECK(index.DictionarySize() == 1);
    CHECK(index.Lookup("term").size() == ROW_GROUP_SIZE);
}

TEST_CASE_METHOD(AllocatorFixture, "Inserting one million terms into the dictionary works",
                 "[inverted_index][memtable]") {
    Memtable index{allocator};

    constexpr duckdb::row_t NUM_DOCUMENTS = 1000;
    constexpr size_t NUM_TERMS_PER_DOCUMENT = 1000;

    auto generate_term = [](size_t t) {
        std::string term;
        constexpr size_t MAX_TERM_LENGTH = 5; // log_26(1'000'000) < 5
        term.reserve(MAX_TERM_LENGTH);

        while (t > 0) {
            t--; // Bijective adjustment
            constexpr char NUM_CHARS = 26;
            term.push_back('a' + static_cast<char>(t % NUM_CHARS));
            t /= NUM_CHARS;
        }

        std::reverse(term.begin(), term.end());
        return term;
    };

    for (duckdb::row_t row_id = 0; row_id < NUM_DOCUMENTS; row_id++) {
        for (size_t t = 0; t < NUM_TERMS_PER_DOCUMENT; t++) {
            const std::string term = generate_term(t + (row_id * NUM_TERMS_PER_DOCUMENT));
            index.Insert(term, row_id);
        }
    }

    CHECK(index.DictionarySize() == NUM_DOCUMENTS * NUM_TERMS_PER_DOCUMENT);
    // Some spot checks
    CHECK(index.Lookup("a").size() == 1);
    CHECK(index.Lookup("aa").size() == 1);
    CHECK(index.Lookup("aaa").size() == 1);
}

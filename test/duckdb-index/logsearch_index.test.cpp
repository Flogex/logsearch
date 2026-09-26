#include "duckdb-index/logsearch_index.hpp"

#include "inverted-index/inverted_index.hpp"
#include "logsearch_extension.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <catch2/matchers/catch_matchers_vector.hpp>
#include <cstdint>
#include <duckdb/catalog/catalog.hpp>
#include <duckdb/catalog/catalog_entry/duck_table_entry.hpp>
#include <duckdb/catalog/catalog_entry/table_catalog_entry.hpp>
#include <duckdb/common/identifier.hpp>
#include <duckdb/common/shared_ptr.hpp>
#include <duckdb/common/typedefs.hpp>
#include <duckdb/common/types/value.hpp>
#include <duckdb/main/client_context.hpp>
#include <duckdb/main/connection.hpp>
#include <duckdb/main/database.hpp>
#include <duckdb/storage/data_table.hpp>
#include <duckdb/storage/storage_info.hpp>
#include <duckdb/storage/table/data_table_info.hpp>
#include <duckdb/storage/table/index_entry.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::Equals;
using duckdb::row_t;
using logsearch::duckdb_index::LogsearchIndex;
using logsearch::inverted_index::InvertedIndex;

namespace {

//! Returns a read handle on the LogsearchIndex named `index_name` on `table_name`.
//! The handle holds the index entry's lock shared. Anything that needs it exclusively (DML on the table, DROP INDEX,
//! checkpoints) blocks until the handle is gone, and deadlocks when run from the same thread.
duckdb::IndexReadHandle<LogsearchIndex> FindIndex(duckdb::Connection& con, const duckdb::Identifier& table_name,
                                                  const duckdb::Identifier& index_name) {
    std::optional<duckdb::IndexReadHandle<LogsearchIndex>> handle;
    con.context->RunFunctionInTransaction([&]() {
        auto& table = duckdb::Catalog::GetEntry<duckdb::TableCatalogEntry>(*con.context, table_name);
        duckdb::DataTable& storage = table.Cast<duckdb::DuckTableEntry>().GetStorage();
        const duckdb::shared_ptr<duckdb::IndexEntry> entry =
            storage.GetDataTableInfo()->GetIndexes().FindEntry(index_name);
        REQUIRE(entry);
        handle.emplace(entry->GetReadHandle<LogsearchIndex>());
    });
    return std::move(*handle);
}

//! An in-memory database with the logsearch extension loaded, plus helpers to run SQL and to reach the inverted index
//! that CREATE INDEX built.
struct LogsearchIndexFixture {
    LogsearchIndexFixture() {
        db.LoadStaticExtension<duckdb::LogsearchExtension>();
    }

    //! Run `sql` and fail the test with DuckDB's message if it does not succeed.
    void Query(const std::string& sql) {
        const auto result = con.Query(sql);
        CAPTURE(sql);
        INFO((result->HasError() ? result->GetError() : std::string{}));
        REQUIRE_FALSE(result->HasError());
    }

    //! Run `sql`, require that it fails, and return the error message.
    std::string RunExpectingError(const std::string& sql) {
        const auto result = con.Query(sql);
        CAPTURE(sql);
        REQUIRE(result->HasError());
        return result->GetError();
    }

    //! Run `sql` and return the column values of its single result row.
    [[nodiscard]] std::vector<duckdb::Value> QueryRow(const std::string& sql) {
        const auto result = con.Query(sql);
        CAPTURE(sql);
        INFO((result->HasError() ? result->GetError() : std::string{}));
        REQUIRE_FALSE(result->HasError());
        REQUIRE(result->RowCount() == 1);
        std::vector<duckdb::Value> row;
        row.reserve(result->ColumnCount());
        for (duckdb::idx_t column = 0; column < result->ColumnCount(); column++) {
            row.push_back(result->GetValue(column, 0));
        }
        return row;
    }

    //! Run `sql` and return the single value of the single-row, single-column result.
    [[nodiscard]] duckdb::Value QueryScalar(const std::string& sql) {
        return QueryRow(sql).at(0);
    }

    //! A read handle on the index `index_name` on `table_name`.
    [[nodiscard]] duckdb::IndexReadHandle<LogsearchIndex> IndexOf(const duckdb::Identifier& table_name,
                                                                  const duckdb::Identifier& index_name) {
        return FindIndex(con, table_name, index_name);
    }

    //! The inverted index behind `index_name` on `table_name`, without holding the entry lock.
    //! Only valid while nothing retires the index entry.
    const InvertedIndex& UnguardedIndexOf(const duckdb::Identifier& table_name, const duckdb::Identifier& index_name) {
        const auto handle = IndexOf(table_name, index_name);
        return handle->GetInvertedIndex();
    }

    duckdb::DuckDB db{nullptr};
    duckdb::Connection con{db};
};

} // namespace

TEST_CASE_METHOD(LogsearchIndexFixture, "CREATE INDEX builds an inverted index over the indexed column",
                 "[duckdb_index]") {
    Query("CREATE TABLE logs AS SELECT * FROM (VALUES ('connection refused'), ('connection established'), "
          "('disk full')) t(message)");
    Query("CREATE INDEX msg_idx ON logs USING logsearch (message)");

    const InvertedIndex& index = UnguardedIndexOf("logs", "msg_idx");
    CHECK_THAT(index.Lookup("connection"), Equals(std::vector<row_t>{0, 1}));
    CHECK_THAT(index.Lookup("refused"), Equals(std::vector<row_t>{0}));
    CHECK_THAT(index.Lookup("disk"), Equals(std::vector<row_t>{2}));
    CHECK(index.Lookup("absent").empty());
    // The rows all fall in row group 0, which the build never seals, so they answer straight from the Memtable.
    CHECK(index.NumSealedPartitions() == 0);
}

TEST_CASE_METHOD(LogsearchIndexFixture, "An index on an empty table is empty", "[duckdb_index]") {
    // A VALUES list cannot be empty, so this is the one table the tests cannot create from a query.
    Query("CREATE TABLE logs (message VARCHAR)");
    Query("CREATE INDEX msg_idx ON logs USING logsearch (message)");

    const InvertedIndex& index = UnguardedIndexOf("logs", "msg_idx");
    CHECK(index.TotalDictionarySize() == 0);
    CHECK(index.Lookup("anything").empty());
}

TEST_CASE_METHOD(LogsearchIndexFixture, "A index stays empty when inserting documents without any term",
                 "[duckdb_index]") {
    // Empty and whitespace-only documents analyze to zero terms, so the Memtable stays empty and must not be
    // sealed into an SSTable without any row ID.
    Query("CREATE TABLE logs AS SELECT * FROM (VALUES (''), ('   '), (E'\\t\\n')) t(message)");
    Query("CREATE INDEX msg_idx ON logs USING logsearch (message)");

    const InvertedIndex& index = UnguardedIndexOf("logs", "msg_idx");
    CHECK(index.TotalDictionarySize() == 0);
    CHECK(index.Lookup("the").empty());
    CHECK(index.Lookup("anything").empty());
}

TEST_CASE_METHOD(LogsearchIndexFixture, "The built index is registered in the catalog", "[duckdb_index]") {
    Query("CREATE TABLE logs AS SELECT * FROM (VALUES ('alpha')) t(message)");
    Query("CREATE INDEX msg_idx ON logs USING logsearch (message)");

    const std::vector<duckdb::Value> row = QueryRow("SELECT index_name, table_name, is_unique FROM duckdb_indexes()");
    REQUIRE(row.size() == 3);
    CHECK(row[0].ToString() == "msg_idx");
    CHECK(row[1].ToString() == "logs");
    CHECK_FALSE(row[2].GetValue<bool>());
}

TEST_CASE_METHOD(LogsearchIndexFixture, "DROP INDEX removes the index again", "[duckdb_index]") {
    Query("CREATE TABLE logs AS SELECT * FROM (VALUES ('alpha')) t(message)");
    Query("CREATE INDEX msg_idx ON logs USING logsearch (message)");
    Query("DROP INDEX msg_idx");

    CHECK(QueryScalar("SELECT count(*) FROM duckdb_indexes()").GetValue<int64_t>() == 0);
}

TEST_CASE_METHOD(LogsearchIndexFixture, "Lookup matches whole terms, not prefixes or suffixes", "[duckdb_index]") {
    // Exercises the dictionary binary search over terms that share a prefix.
    Query("CREATE TABLE logs AS SELECT * FROM (VALUES ('connect connection connections reconnect')) t(message)");
    Query("CREATE INDEX msg_idx ON logs USING logsearch (message)");

    const InvertedIndex& index = UnguardedIndexOf("logs", "msg_idx");
    CHECK_THAT(index.Lookup("connect"), Equals(std::vector<row_t>{0}));
    CHECK_THAT(index.Lookup("connection"), Equals(std::vector<row_t>{0}));
    CHECK_THAT(index.Lookup("connections"), Equals(std::vector<row_t>{0}));
    CHECK_THAT(index.Lookup("reconnect"), Equals(std::vector<row_t>{0}));
    CHECK(index.Lookup("conn").empty());
    CHECK(index.Lookup("connectio").empty());
    CHECK(index.Lookup("connectionss").empty());
    CHECK(index.Lookup("").empty());
}

TEST_CASE_METHOD(LogsearchIndexFixture, "Terms longer than an inlined string_t are indexed", "[duckdb_index]") {
    // A string_t up to 12 bytes is inlined, longer ones are a pointer into separate storage. Both have to reach the
    // analyzer intact.
    const std::string long_term(500, 'x');
    Query("CREATE TABLE logs AS SELECT * FROM (VALUES ('short " + long_term + "')) t(message)");
    Query("CREATE INDEX msg_idx ON logs USING logsearch (message)");

    const InvertedIndex& index = UnguardedIndexOf("logs", "msg_idx");
    CHECK_THAT(index.Lookup(long_term), Equals(std::vector<row_t>{0}));
    CHECK_THAT(index.Lookup("short"), Equals(std::vector<row_t>{0}));
    CHECK(index.Lookup(long_term.substr(0, 499)).empty());
}

TEST_CASE_METHOD(LogsearchIndexFixture, "Indexed terms are lowercased by the analyzer", "[duckdb_index]") {
    Query("CREATE TABLE logs AS SELECT * FROM (VALUES ('Connection REFUSED')) t(message)");
    Query("CREATE INDEX msg_idx ON logs USING logsearch (message)");

    const InvertedIndex& index = UnguardedIndexOf("logs", "msg_idx");
    CHECK_THAT(index.Lookup("connection"), Equals(std::vector<row_t>{0}));
    CHECK_THAT(index.Lookup("refused"), Equals(std::vector<row_t>{0}));
}

TEST_CASE_METHOD(LogsearchIndexFixture, "A term repeated within one document yields a single row ID",
                 "[duckdb_index]") {
    Query("CREATE TABLE logs AS SELECT * FROM (VALUES ('retry retry retry'), ('retry')) t(message)");
    Query("CREATE INDEX msg_idx ON logs USING logsearch (message)");

    const InvertedIndex& index = UnguardedIndexOf("logs", "msg_idx");
    CHECK_THAT(index.Lookup("retry"), Equals(std::vector<row_t>{0, 1}));
}

TEST_CASE_METHOD(LogsearchIndexFixture, "Rows with a NULL key are skipped without shifting the other row IDs",
                 "[duckdb_index]") {
    Query("CREATE TABLE logs AS SELECT * FROM (VALUES ('alpha'), (NULL), ('alpha beta')) t(message)");
    Query("CREATE INDEX msg_idx ON logs USING logsearch (message)");

    const InvertedIndex& index = UnguardedIndexOf("logs", "msg_idx");
    CHECK_THAT(index.Lookup("alpha"), Equals(std::vector<row_t>{0, 2}));
    CHECK_THAT(index.Lookup("beta"), Equals(std::vector<row_t>{2}));
}

TEST_CASE_METHOD(LogsearchIndexFixture, "Rows deleted before the build leave gaps in the row IDs", "[duckdb_index]") {
    Query("CREATE TABLE logs (message VARCHAR)");
    Query("INSERT INTO logs VALUES ('alpha'), ('beta'), ('alpha gamma'), ('beta')");
    Query("DELETE FROM logs WHERE message = 'beta'");
    Query("CREATE INDEX msg_idx ON logs USING logsearch (message)");

    const InvertedIndex& index = UnguardedIndexOf("logs", "msg_idx");
    CHECK_THAT(index.Lookup("alpha"), Equals(std::vector<row_t>{0, 2}));
    CHECK_THAT(index.Lookup("gamma"), Equals(std::vector<row_t>{2}));
    CHECK(index.Lookup("beta").empty());
}

TEST_CASE_METHOD(LogsearchIndexFixture, "Uncommitted rows never reach the index with a temporary row ID",
                 "[duckdb_index]") {
    // Transaction-local rows carry row IDs that are rewritten on commit, so indexing them would store row IDs that
    // never exist. DataTable::CreateIndexScan reads the persistent row groups only, so they never reach the build.
    Query("CREATE TABLE logs (message VARCHAR)");
    Query("INSERT INTO logs VALUES ('committed')");
    Query("BEGIN TRANSACTION");
    Query("INSERT INTO logs VALUES ('uncommitted')");

    Query("CREATE INDEX msg_idx ON logs USING logsearch (message)");
    const InvertedIndex& index = UnguardedIndexOf("logs", "msg_idx");
    CHECK_THAT(index.Lookup("committed"), Equals(std::vector<row_t>{0}));
    CHECK(index.Lookup("uncommitted").empty());
}

TEST_CASE_METHOD(LogsearchIndexFixture, "An index can be created on a table with generated columns", "[duckdb_index]") {
    // The indexed column is addressed by its physical ID, which skips generated columns.
    Query("CREATE TABLE logs (id INTEGER, upper_message VARCHAR GENERATED ALWAYS AS (upper(message)), "
          "message VARCHAR)");
    Query("INSERT INTO logs (id, message) VALUES (1, 'alpha'), (2, 'beta')");
    Query("CREATE INDEX msg_idx ON logs USING logsearch (message)");

    const InvertedIndex& index = UnguardedIndexOf("logs", "msg_idx");
    CHECK_THAT(index.Lookup("alpha"), Equals(std::vector<row_t>{0}));
    CHECK_THAT(index.Lookup("beta"), Equals(std::vector<row_t>{1}));
}

TEST_CASE_METHOD(LogsearchIndexFixture, "Several Logsearch indexes can coexist on one table", "[duckdb_index]") {
    Query("CREATE TABLE logs (message VARCHAR, component VARCHAR)");
    Query("INSERT INTO logs VALUES ('disk full', 'storage'), ('connection refused', 'network')");
    Query("CREATE INDEX msg_idx ON logs USING logsearch (message)");
    Query("CREATE INDEX component_idx ON logs USING logsearch (component)");

    const InvertedIndex& message_index = UnguardedIndexOf("logs", "msg_idx");
    const InvertedIndex& component_index = UnguardedIndexOf("logs", "component_idx");
    CHECK_THAT(message_index.Lookup("disk"), Equals(std::vector<row_t>{0}));
    CHECK(message_index.Lookup("storage").empty());
    CHECK_THAT(component_index.Lookup("storage"), Equals(std::vector<row_t>{0}));
    CHECK(component_index.Lookup("disk").empty());
}

TEST_CASE_METHOD(LogsearchIndexFixture, "An index name can only be used once", "[duckdb_index]") {
    Query("CREATE TABLE logs AS SELECT * FROM (VALUES ('alpha')) t(message)");
    Query("CREATE INDEX msg_idx ON logs USING logsearch (message)");

    CHECK_THAT(RunExpectingError("CREATE INDEX msg_idx ON logs USING logsearch (message)"),
               ContainsSubstring("already exists"));
    Query("CREATE INDEX IF NOT EXISTS msg_idx ON logs USING logsearch (message)");
    CHECK(QueryScalar("SELECT count(*) FROM duckdb_indexes()").GetValue<int64_t>() == 1);
}

TEST_CASE_METHOD(LogsearchIndexFixture, "A dictionary spanning many blocks stays searchable", "[duckdb_index]") {
    // One distinct term per row pushes the sealed SSTable's dictionary and string pool well past a single block.
    // Only a row-group change seals, so the table has to cross a boundary for an SSTable to exist at all. One task
    // keeps that deterministic (a second task would merge, and merging seals the trailing row group, too).
    constexpr duckdb::idx_t row_count = DEFAULT_ROW_GROUP_SIZE + 1;
    constexpr auto last_sealed_row = static_cast<row_t>(DEFAULT_ROW_GROUP_SIZE) - 1;
    Query("SET threads = 1");
    Query("CREATE TABLE logs AS SELECT 'term' || i AS message FROM range(" + std::to_string(row_count) + ") t(i)");
    Query("CREATE INDEX msg_idx ON logs USING logsearch (message)");

    const InvertedIndex& index = UnguardedIndexOf("logs", "msg_idx");
    CHECK(index.NumSealedPartitions() == 1);
    CHECK_THAT(index.Lookup("term0"), Equals(std::vector<row_t>{0}));
    CHECK_THAT(index.Lookup("term1"), Equals(std::vector<row_t>{1}));
    // Last term of the sealed partition, then the single row that is still in the Memtable.
    CHECK_THAT(index.Lookup("term" + std::to_string(last_sealed_row)), Equals(std::vector<row_t>{last_sealed_row}));
    CHECK_THAT(index.Lookup("term" + std::to_string(DEFAULT_ROW_GROUP_SIZE)),
               Equals(std::vector<row_t>{last_sealed_row + 1}));
    CHECK(index.Lookup("term" + std::to_string(row_count)).empty());
    CHECK(index.Lookup("term").empty());
}

TEST_CASE_METHOD(LogsearchIndexFixture, "A table spanning several row groups yields one partition per row group",
                 "[duckdb_index]") {
    // Whether the scan runs on one thread or many decides how the partitions are distributed over the build tasks,
    // so both have to produce the same index.
    const bool single_threaded = GENERATE(false, true);
    if (single_threaded) {
        Query("SET threads = 1");
    }

    constexpr duckdb::idx_t row_group_size = DEFAULT_ROW_GROUP_SIZE;
    constexpr duckdb::idx_t row_count = (2 * row_group_size) + 1000;
    constexpr row_t needle_row = static_cast<row_t>(row_group_size) + 7;
    Query("CREATE TABLE big_tbl AS SELECT 'term' || (i % 10) || CASE WHEN i = " + std::to_string(needle_row) +
          " THEN ' needle' ELSE '' END AS message FROM range(" + std::to_string(row_count) + ") t(i)");
    Query("CREATE INDEX big_idx ON big_tbl USING logsearch (message)");

    const InvertedIndex& index = UnguardedIndexOf("big_tbl", "big_idx");
    // The two full row groups are always sealed. The trailing one only is if a Merge ran, i.e. if the build used
    // more than one task, so the exact count is thread-count dependent while the Lookup results below are not.
    CHECK(index.NumSealedPartitions() == (single_threaded ? 2 : 3));

    // A term that occurs exactly once, in the middle partition.
    CHECK_THAT(index.Lookup("needle"), Equals(std::vector<row_t>{needle_row}));

    // A term that occurs in every partition: every tenth row, ascending across partition boundaries.
    const std::vector<row_t> hits = index.Lookup("term0");
    REQUIRE(hits.size() == row_count / 10);
    CHECK(hits.front() == 0);
    CHECK(hits.back() == static_cast<row_t>(row_count) - 10);
    CHECK(std::is_sorted(hits.begin(), hits.end()));
}

TEST_CASE("An index outlives the connection that built it", "[duckdb_index]") {
    // The index is owned by the table, not by the connection, so nothing it holds may come from per-connection state.
    // BufferManager::GetBufferManager(ClientContext&) returns a wrapper that ClientData destroys with the connection,
    // so sealing partitions through it would leave the SSTables pointing at freed memory.
    duckdb::DuckDB db(nullptr);
    db.LoadStaticExtension<duckdb::LogsearchExtension>();
    {
        duckdb::Connection builder(db);
        REQUIRE_FALSE(builder.Query("CREATE TABLE logs (message VARCHAR)")->HasError());
        REQUIRE_FALSE(builder.Query("INSERT INTO logs VALUES ('alpha beta'), ('alpha gamma')")->HasError());
        REQUIRE_FALSE(builder.Query("CREATE INDEX msg_idx ON logs USING logsearch (message)")->HasError());
    }

    duckdb::Connection reader(db);
    const auto handle = FindIndex(reader, "logs", "msg_idx");
    const InvertedIndex& index = handle->GetInvertedIndex();
    CHECK_THAT(index.Lookup("alpha"), Equals(std::vector<row_t>{0, 1}));
    CHECK_THAT(index.Lookup("gamma"), Equals(std::vector<row_t>{1}));
}

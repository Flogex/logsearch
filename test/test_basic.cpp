#include "logsearch_extension.hpp"

#include <catch2/catch_test_macros.hpp>
#include <duckdb/execution/index/index_type_set.hpp>
#include <duckdb/main/connection.hpp>
#include <duckdb/main/database.hpp>

TEST_CASE("LogsearchIndex registers as an index type", "[basic]") {
    duckdb::DuckDB db(nullptr);
    db.LoadStaticExtension<duckdb::LogsearchExtension>();

    auto& index_types = db.instance->config.GetIndexTypes();
    auto type = index_types.FindByName("logsearch");
    REQUIRE(type);
    REQUIRE(type->name == "logsearch");
}

TEST_CASE("LogsearchIndex can be created", "[basic]") {
    duckdb::DuckDB db(nullptr);
    db.LoadStaticExtension<duckdb::LogsearchExtension>();

    duckdb::Connection con(db);
    con.Query("CREATE TABLE test (id INTEGER, value VARCHAR)");
    const auto result = con.Query("CREATE INDEX idx ON test USING logsearch(value)");
    REQUIRE_FALSE(result->HasError());
}

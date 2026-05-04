#pragma once

#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "logsearch_extension.hpp"

namespace logsearch::test_helpers {

class DatabaseFixture {
public:
    DatabaseFixture() : db(nullptr), con(db) {
        db.LoadStaticExtension<duckdb::LogsearchExtension>();
    }

    duckdb::DuckDB db;
    duckdb::Connection con;
};

} // namespace logsearch::test_helpers

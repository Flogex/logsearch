#include "test_helpers/database_fixture.hpp"

#include <catch2/catch_test_macros.hpp>

using logsearch::test_helpers::DatabaseFixture;

TEST_CASE_METHOD(DatabaseFixture, "logsearch_version returns version string", "[basic]") {
    auto res = con.Query("SELECT logsearch_version()");
    REQUIRE_FALSE(res->HasError());
    REQUIRE(res->RowCount() == 1);
    REQUIRE(res->GetValue(0, 0).ToString() == "0.1.0");
}

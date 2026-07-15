// Assumptions the inverted-index design leans on, verified against the DuckDB
// version we build against (build/release/_deps/duckdb-src). These are not tests
// of our code; they pin down DuckDB storage behavior so we notice if an upgrade
// changes it under us.
//
// Facts we depend on and check here:
//   1. Row groups are DEFAULT_ROW_GROUP_SIZE (122880) rows. Committed batches
//      (even with CHECKPOINT between them) keep filling the *same* row group
//      until it is full; only then does a new one start at N * 122880. So one
//      "partition" == one full row group regardless of how the rows were loaded.
//   2. A row's rowid maps to its row group by integer division:
//      row_group_index == rowid / DEFAULT_ROW_GROUP_SIZE (for a table whose
//      first row group starts at row 0, which is the normal case).
//   3. DELETE does not renumber rows: surviving rowids are stable, deleted
//      rowids become permanent gaps, and freed ids are not recycled by later
//      inserts. => an SSTable's row ids stay valid; deletes are a query-time
//      filter (deleted-rows bitmap), not a postings rewrite.
//   4. UPDATE is in place: the rowid is preserved and the content changes.
//      => the index will hold stale postings for the old term and miss the new
//      term. This is the update-invalidation problem the TODO calls out.
//   5. Layout and rowids survive checkpoint + reopen.

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <duckdb/catalog/catalog.hpp>
#include <duckdb/catalog/catalog_entry/duck_table_entry.hpp>
#include <duckdb/catalog/catalog_entry/table_catalog_entry.hpp>
#include <duckdb/common/allocator.hpp>
#include <duckdb/common/constants.hpp>
#include <duckdb/common/types/data_chunk.hpp>
#include <duckdb/common/types/vector.hpp>
#include <duckdb/main/client_context.hpp>
#include <duckdb/main/connection.hpp>
#include <duckdb/main/database.hpp>
#include <duckdb/storage/data_table.hpp>
#include <duckdb/storage/storage_index.hpp>
#include <duckdb/storage/table/row_group_collection.hpp>
#include <duckdb/storage/table/scan_state.hpp>
#include <duckdb/storage/table/segment_tree.hpp>
#include <duckdb/transaction/duck_transaction.hpp>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

// DEFAULT_ROW_GROUP_SIZE from duckdb/storage/storage_info.hpp. Hard-coded on
// purpose: if DuckDB ever changes it, these tests should fail loudly.
constexpr std::int64_t ROW_GROUP_SIZE = 122880;

// A self-cleaning on-disk database path. Checkpoint only does real work against
// a file-backed database, so the row-group-coalescing and reopen tests need one.
struct TempDatabase {
    std::filesystem::path path;

    explicit TempDatabase(const std::string& name)
        : path(std::filesystem::temp_directory_path() / ("logsearch_" + name + ".db")) {
        Cleanup();
    }
    ~TempDatabase() {
        Cleanup();
    }
    TempDatabase(const TempDatabase&) = delete;
    TempDatabase& operator=(const TempDatabase&) = delete;

    void Cleanup() const {
        std::error_code ec;
        std::filesystem::remove(path, ec);
        std::filesystem::remove(std::filesystem::path(path.string() + ".wal"), ec);
    }
    [[nodiscard]] std::string str() const {
        return path.string();
    }
};

void Run(duckdb::Connection& con, const std::string& sql) {
    auto result = con.Query(sql);
    INFO(sql);
    INFO((result->HasError() ? result->GetError() : std::string{}));
    REQUIRE_FALSE(result->HasError());
}

std::int64_t ScalarI64(duckdb::Connection& con, const std::string& sql) {
    auto result = con.Query(sql);
    INFO(sql);
    INFO((result->HasError() ? result->GetError() : std::string{}));
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->RowCount() == 1);
    return result->GetValue<std::int64_t>(0, 0);
}

bool ScalarBool(duckdb::Connection& con, const std::string& sql) {
    auto result = con.Query(sql);
    INFO(sql);
    INFO((result->HasError() ? result->GetError() : std::string{}));
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->RowCount() == 1);
    return result->GetValue<bool>(0, 0);
}

// Run `fn` with the table's live RowGroupCollection inside an active transaction
// (a catalog lookup needs one). Uses Connection::BeginTransaction so a REQUIRE
// inside `fn` reports normally. `catalog` is the attached-database name, or
// INVALID_CATALOG for the default one.
template <class Fn>
void WithRowGroups(duckdb::Connection& con, const std::string& catalog, const std::string& table, Fn fn) {
    con.BeginTransaction();
    auto& entry = duckdb::Catalog::GetEntry<duckdb::TableCatalogEntry>(*con.context, catalog, DEFAULT_SCHEMA, table);
    auto& storage = entry.Cast<duckdb::DuckTableEntry>().GetStorage();
    fn(*storage.GetRowGroupCollection());
    con.Commit();
}

std::int64_t NumRowGroups(duckdb::Connection& con, const std::string& table) {
    // pragma_storage_info reports one row per column *segment*; distinct
    // row_group_id therefore counts row groups.
    return ScalarI64(con, "SELECT COUNT(DISTINCT row_group_id) FROM pragma_storage_info('" + table + "')");
}

// INSERT a contiguous [lo, hi) run of (id, msg) rows using range(), one committed
// statement (auto-commit transaction).
void InsertRange(duckdb::Connection& con, std::int64_t lo, std::int64_t hi) {
    Run(con,
        "INSERT INTO logs SELECT i, 'msg ' || i FROM range(" + std::to_string(lo) + ", " + std::to_string(hi) +
            ") t(i)");
}

} // namespace

TEST_CASE("Batched inserts with checkpoints still coalesce into one full row group", "[duckdb-storage]") {
    TempDatabase tmp("batched_rowgroups");
    duckdb::DuckDB db(tmp.str());
    duckdb::Connection con(db);
    Run(con, "CREATE TABLE logs(id BIGINT, msg VARCHAR)");

    // Exactly one row group's worth, loaded in five committed batches with a
    // checkpoint after each.
    const std::vector<std::pair<std::int64_t, std::int64_t>> batches = {
        {0, 30000}, {30000, 60000}, {60000, 90000}, {90000, 120000}, {120000, ROW_GROUP_SIZE}};
    for (const auto& [lo, hi] : batches) {
        InsertRange(con, lo, hi);
        Run(con, "CHECKPOINT");
    }

    REQUIRE(ScalarI64(con, "SELECT COUNT(*) FROM logs") == ROW_GROUP_SIZE);
    REQUIRE(ScalarI64(con, "SELECT MIN(rowid) FROM logs") == 0);
    REQUIRE(ScalarI64(con, "SELECT MAX(rowid) FROM logs") == ROW_GROUP_SIZE - 1);

    // The core assumption: batching + intervening checkpoints did NOT fragment
    // the data; it all sits in a single row group.
    REQUIRE(NumRowGroups(con, "logs") == 1);

    // One more row spills into a brand-new row group that starts at N*122880.
    Run(con, "INSERT INTO logs VALUES (999999, 'overflow')");
    Run(con, "CHECKPOINT");
    REQUIRE(ScalarI64(con, "SELECT COUNT(*) FROM logs") == ROW_GROUP_SIZE + 1);
    REQUIRE(ScalarI64(con, "SELECT MAX(rowid) FROM logs") == ROW_GROUP_SIZE);
    REQUIRE(NumRowGroups(con, "logs") == 2);
    // Its rowid is the first row of row group 1.
    REQUIRE(ScalarI64(con, "SELECT rowid FROM logs WHERE id = 999999") == ROW_GROUP_SIZE);
}

TEST_CASE("rowid maps to its row group via division by DEFAULT_ROW_GROUP_SIZE", "[duckdb-storage]") {
    TempDatabase tmp("rowid_mapping");
    duckdb::DuckDB db(tmp.str());
    duckdb::Connection con(db);
    Run(con, "CREATE TABLE logs(id BIGINT, msg VARCHAR)");

    // 2.5 row groups.
    const std::int64_t n = ROW_GROUP_SIZE * 2 + 5000;
    InsertRange(con, 0, n);
    Run(con, "CHECKPOINT");

    REQUIRE(ScalarI64(con, "SELECT COUNT(*) FROM logs") == n);
    REQUIRE(ScalarI64(con, "SELECT MIN(rowid) FROM logs") == 0);
    REQUIRE(ScalarI64(con, "SELECT MAX(rowid) FROM logs") == n - 1);
    REQUIRE(NumRowGroups(con, "logs") == 3);

    // The mapping we rely on: rowid / 122880 == row group index. Bucketing every
    // row by that formula yields two full groups and one partial.
    const std::string rg = std::to_string(ROW_GROUP_SIZE);
    REQUIRE(ScalarI64(con, "SELECT COUNT(*) FROM logs WHERE rowid // " + rg + " = 0") == ROW_GROUP_SIZE);
    REQUIRE(ScalarI64(con, "SELECT COUNT(*) FROM logs WHERE rowid // " + rg + " = 1") == ROW_GROUP_SIZE);
    REQUIRE(ScalarI64(con, "SELECT COUNT(*) FROM logs WHERE rowid // " + rg + " = 2") == 5000);

    // Storage-side cross-check. Careful: pragma_storage_info's `start` is the
    // segment's offset WITHIN its row group (each row group's column segment
    // tree restarts at 0), NOT an absolute table row position. So the only
    // invariant we can assert per segment is that it fits inside one row group.
    REQUIRE(ScalarI64(con,
                      "SELECT COUNT(*) FROM pragma_storage_info('logs') "
                      "WHERE start + count > " +
                          rg) == 0);
    // Row groups are numbered 0..2 contiguously.
    REQUIRE(ScalarI64(con, "SELECT MIN(row_group_id) FROM pragma_storage_info('logs')") == 0);
    REQUIRE(ScalarI64(con, "SELECT MAX(row_group_id) FROM pragma_storage_info('logs')") == 2);
}

TEST_CASE("DELETE keeps rowids stable, leaves gaps, and does not recycle ids", "[duckdb-storage]") {
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);
    Run(con, "CREATE TABLE logs(id BIGINT, msg VARCHAR)");

    const std::int64_t n = 1000;
    InsertRange(con, 0, n);
    // Freshly loaded, contiguous from 0: rowid == id for every row.
    REQUIRE(ScalarI64(con, "SELECT COUNT(*) FROM logs WHERE rowid = id") == n);

    // Delete a middle chunk [400, 500).
    Run(con, "DELETE FROM logs WHERE id >= 400 AND id < 500");
    REQUIRE(ScalarI64(con, "SELECT COUNT(*) FROM logs") == n - 100);
    // Survivors keep their original rowid.
    REQUIRE(ScalarI64(con, "SELECT COUNT(*) FROM logs WHERE rowid = id") == n - 100);
    // No renumbering: the max rowid is untouched.
    REQUIRE(ScalarI64(con, "SELECT MAX(rowid) FROM logs") == n - 1);
    // The deleted rowids are a permanent gap.
    REQUIRE(ScalarI64(con, "SELECT COUNT(*) FROM logs WHERE rowid >= 400 AND rowid < 500") == 0);

    // A later insert gets a fresh rowid past the previous max; the freed ids in
    // [400,500) are NOT reused.
    Run(con, "INSERT INTO logs VALUES (10000, 'new')");
    REQUIRE(ScalarI64(con, "SELECT rowid FROM logs WHERE id = 10000") == n);
}

TEST_CASE("UPDATE preserves rowid and changes content in place", "[duckdb-storage]") {
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);
    Run(con, "CREATE TABLE logs(id BIGINT, msg VARCHAR)");
    InsertRange(con, 0, 1000);

    const std::int64_t rowid_before = ScalarI64(con, "SELECT rowid FROM logs WHERE id = 123");

    Run(con, "UPDATE logs SET msg = 'updated' WHERE id = 123");

    // Same rowid, new content: an UPDATE is not a delete+insert.
    REQUIRE(ScalarI64(con, "SELECT rowid FROM logs WHERE id = 123") == rowid_before);
    REQUIRE(ScalarI64(con, "SELECT COUNT(*) FROM logs WHERE id = 123 AND msg = 'updated'") == 1);
    // Cardinality and id space are untouched.
    REQUIRE(ScalarI64(con, "SELECT COUNT(*) FROM logs") == 1000);
    REQUIRE(ScalarI64(con, "SELECT MAX(rowid) FROM logs") == 999);

    // Storage flags the updated column's segment. This is exactly the signal we
    // would need to know an on-disk partition's postings for `msg` are stale.
    REQUIRE(ScalarBool(con,
                       "SELECT bool_or(has_updates) FROM pragma_storage_info('logs') "
                       "WHERE column_name = 'msg'"));
}

TEST_CASE("Row-group layout and rowids survive checkpoint and reopen", "[duckdb-storage]") {
    TempDatabase tmp("reopen");
    const std::int64_t n = ROW_GROUP_SIZE + 1000;

    {
        duckdb::DuckDB db(tmp.str());
        duckdb::Connection con(db);
        Run(con, "CREATE TABLE logs(id BIGINT, msg VARCHAR)");
        InsertRange(con, 0, n);
        Run(con, "CHECKPOINT");
    } // close + flush

    {
        duckdb::DuckDB db(tmp.str());
        duckdb::Connection con(db);
        REQUIRE(ScalarI64(con, "SELECT COUNT(*) FROM logs") == n);
        REQUIRE(ScalarI64(con, "SELECT MIN(rowid) FROM logs") == 0);
        REQUIRE(ScalarI64(con, "SELECT MAX(rowid) FROM logs") == n - 1);
        REQUIRE(NumRowGroups(con, "logs") == 2);
    }
}

// ---------------------------------------------------------------------------
// Direct verification through the internal storage APIs (SegmentTree /
// RowGroupCollection), not just SQL introspection.
// ---------------------------------------------------------------------------

TEST_CASE("Non-default row group size: SegmentTree reports it and maps rows to groups", "[duckdb-storage]") {
    // ROW_GROUP_SIZE is a per-database ATTACH option (storage_manager.cpp). Using
    // a small one keeps the test fast and proves nothing hard-codes 122880.
    TempDatabase tmp("rg_size");
    const std::int64_t rg = 2048;
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);
    Run(con, "ATTACH '" + tmp.str() + "' AS s (ROW_GROUP_SIZE " + std::to_string(rg) + ")");
    Run(con, "CREATE TABLE s.logs(id BIGINT)");
    // One statement => contiguous, boundary-aligned groups: 2048 | 2048 | 500.
    const std::int64_t n = rg * 2 + 500;
    Run(con, "INSERT INTO s.logs SELECT i FROM range(" + std::to_string(n) + ") t(i)");
    Run(con, "CHECKPOINT s");

    std::uint64_t rg_size = 0;
    std::uint64_t seg_count = 0;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> start_count; // (row_start, count) per segment
    std::uint64_t last_index = 0;
    std::uint64_t idx_first = 99, idx_g0_last = 99, idx_g1_first = 99, idx_g2_first = 99, idx_last_row = 99;
    bool out_of_range_threw = false;

    WithRowGroups(con, "s", "logs", [&](duckdb::RowGroupCollection& rgc) {
        rg_size = rgc.GetRowGroupSize();
        auto tree = rgc.GetRowGroups();
        auto lock = tree->Lock();
        seg_count = tree->GetSegmentCount(lock);
        for (std::int64_t i = 0; i < static_cast<std::int64_t>(seg_count); i++) {
            auto node = tree->GetSegmentByIndex(lock, i);
            start_count.emplace_back(node->GetRowStart(), node->GetCount());
        }
        last_index = tree->GetSegmentByIndex(lock, -1)->GetIndex(); // -1 == last segment
        idx_first = tree->GetSegmentIndex(lock, 0);
        idx_g0_last = tree->GetSegmentIndex(lock, static_cast<std::uint64_t>(rg - 1));
        idx_g1_first = tree->GetSegmentIndex(lock, static_cast<std::uint64_t>(rg));
        idx_g2_first = tree->GetSegmentIndex(lock, static_cast<std::uint64_t>(2 * rg));
        idx_last_row = tree->GetSegmentIndex(lock, static_cast<std::uint64_t>(n - 1));
        try {
            tree->GetSegmentIndex(lock, static_cast<std::uint64_t>(n)); // past the last row
        } catch (...) {
            out_of_range_threw = true;
        }
    });

    REQUIRE(rg_size == static_cast<std::uint64_t>(rg));
    REQUIRE(seg_count == 3);
    REQUIRE(start_count.size() == 3);
    REQUIRE(start_count[0].first == 0);
    REQUIRE(start_count[0].second == static_cast<std::uint64_t>(rg));
    REQUIRE(start_count[1].first == static_cast<std::uint64_t>(rg));
    REQUIRE(start_count[1].second == static_cast<std::uint64_t>(rg));
    REQUIRE(start_count[2].first == static_cast<std::uint64_t>(2 * rg));
    REQUIRE(start_count[2].second == 500);
    REQUIRE(last_index == 2);
    // GetSegmentIndex is DuckDB's own rowid -> row group lookup.
    REQUIRE(idx_first == 0);
    REQUIRE(idx_g0_last == 0);
    REQUIRE(idx_g1_first == 1);
    REQUIRE(idx_g2_first == 2);
    REQUIRE(idx_last_row == 2);
    REQUIRE(out_of_range_threw);
}

TEST_CASE("RowGroupCollection::Fetch resolves row ids to the correct rows", "[duckdb-storage]") {
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);
    Run(con, "CREATE TABLE logs(id BIGINT, msg VARCHAR)");
    InsertRange(con, 0, 1000); // rowid == id

    std::vector<std::int64_t> fetched;
    con.BeginTransaction();
    {
        auto& entry =
            duckdb::Catalog::GetEntry<duckdb::TableCatalogEntry>(*con.context, INVALID_CATALOG, DEFAULT_SCHEMA, "logs");
        auto& storage = entry.Cast<duckdb::DuckTableEntry>().GetStorage();
        auto& transaction = duckdb::DuckTransaction::Get(*con.context, entry.ParentCatalog());

        duckdb::Vector row_ids(duckdb::LogicalType::BIGINT);
        auto* ids = duckdb::FlatVector::GetData<duckdb::row_t>(row_ids);
        ids[0] = 5;
        ids[1] = 100;
        ids[2] = 999;

        duckdb::DataChunk result;
        duckdb::vector<duckdb::LogicalType> fetch_types{duckdb::LogicalType::BIGINT};
        result.Initialize(duckdb::Allocator::Get(*con.context), fetch_types);
        // Fetch only physical column 0 (id). DataTable::Fetch delegates to
        // RowGroupCollection::Fetch for committed rows.
        duckdb::vector<duckdb::StorageIndex> column_ids{duckdb::StorageIndex(0)};
        duckdb::ColumnFetchState fetch_state;
        storage.Fetch(transaction, result, column_ids, row_ids, 3, fetch_state);

        for (std::int64_t i = 0; i < static_cast<std::int64_t>(result.size()); i++) {
            fetched.push_back(result.GetValue(0, i).GetValue<std::int64_t>());
        }
    }
    con.Commit();

    // Row id r fetches id == r (contiguous load).
    REQUIRE(fetched == (std::vector<std::int64_t>{5, 100, 999}));
}

TEST_CASE("Non-indexed table: a vacuuming checkpoint merges row groups and changes row ids", "[duckdb-storage]") {
    TempDatabase tmp("vacuum_merge");
    const std::int64_t rg = 2048;
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);
    Run(con, "ATTACH '" + tmp.str() + "' AS s (ROW_GROUP_SIZE " + std::to_string(rg) + ")");
    Run(con, "CREATE TABLE s.logs(id BIGINT)");
    const std::int64_t n = rg * 5; // five full row groups
    Run(con, "INSERT INTO s.logs SELECT i FROM range(" + std::to_string(n) + ") t(i)");
    Run(con, "CHECKPOINT s");

    REQUIRE(ScalarI64(con, "SELECT rowid FROM s.logs WHERE id = 5000") == 5000);
    std::uint64_t groups_before = 0;
    WithRowGroups(con, "s", "logs", [&](duckdb::RowGroupCollection& rgc) {
        groups_before = rgc.GetRowGroups()->GetSegmentCount();
    });
    REQUIRE(groups_before == 5);

    // Sparse deletes (keep every 100th row). Survivors of adjacent groups now fit
    // together, so the full checkpoint's vacuum merges the row groups.
    Run(con, "DELETE FROM s.logs WHERE id % 100 <> 0");
    Run(con, "CHECKPOINT s");

    std::uint64_t groups_after = 0;
    WithRowGroups(con, "s", "logs", [&](duckdb::RowGroupCollection& rgc) {
        groups_after = rgc.GetRowGroups()->GetSegmentCount();
    });

    // Row groups were merged...
    REQUIRE(groups_after < groups_before);
    // ...the row is still there with the same value...
    REQUIRE(ScalarI64(con, "SELECT COUNT(*) FROM s.logs WHERE id = 5000") == 1);
    // ...but its rowid moved. No index => vacuum is free to renumber survivors.
    // This is why an inverted index CANNOT store raw row ids of a non-indexed,
    // vacuum-eligible table across a checkpoint.
    REQUIRE(ScalarI64(con, "SELECT rowid FROM s.logs WHERE id = 5000") != 5000);
}

TEST_CASE("Indexed table: a vacuuming checkpoint preserves row ids", "[duckdb-storage]") {
    TempDatabase tmp("vacuum_indexed");
    const std::int64_t rg = 2048;
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);
    Run(con, "ATTACH '" + tmp.str() + "' AS s (ROW_GROUP_SIZE " + std::to_string(rg) + ")");
    Run(con, "CREATE TABLE s.logs(id BIGINT)");
    // Any index sets has_indexes; vacuum_rebuild_indexes defaults to 0 (disabled),
    // so can_change_row_ids is false and the vacuum may not move live rows.
    Run(con, "CREATE INDEX idx ON s.logs(id)");
    const std::int64_t n = rg * 5;
    Run(con, "INSERT INTO s.logs SELECT i FROM range(" + std::to_string(n) + ") t(i)");
    Run(con, "CHECKPOINT s");
    REQUIRE(ScalarI64(con, "SELECT rowid FROM s.logs WHERE id = 5000") == 5000);

    Run(con, "DELETE FROM s.logs WHERE id % 100 <> 0");
    Run(con, "CHECKPOINT s");

    // Same rowid: the index pins it. This is the mechanism that keeps a stored
    // inverted index's row ids valid across deletes + checkpoints.
    REQUIRE(ScalarI64(con, "SELECT COUNT(*) FROM s.logs WHERE id = 5000") == 1);
    REQUIRE(ScalarI64(con, "SELECT rowid FROM s.logs WHERE id = 5000") == 5000);
}

TEST_CASE("Reopen sets SUGGEST_NEW: an append creates a partial, non-terminal row group", "[duckdb-storage]") {
    TempDatabase tmp("suggest_new");
    const std::int64_t rg = 2048;

    // Session 1: a single partial row group (1000 < 2048 rows).
    {
        duckdb::DuckDB db(nullptr);
        duckdb::Connection con(db);
        Run(con, "ATTACH '" + tmp.str() + "' AS s (ROW_GROUP_SIZE " + std::to_string(rg) + ")");
        Run(con, "CREATE TABLE s.logs(id BIGINT)");
        Run(con, "INSERT INTO s.logs SELECT i FROM range(1000) t(i)");
        Run(con, "CHECKPOINT s");
    }

    // Session 2: reopening from disk sets RowGroupAppendMode::SUGGEST_NEW
    // (data_table.cpp), so the next append opens a NEW row group instead of
    // topping up the partial one.
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);
    Run(con, "ATTACH '" + tmp.str() + "' AS s (ROW_GROUP_SIZE " + std::to_string(rg) + ")");
    Run(con, "INSERT INTO s.logs SELECT i FROM range(1000, 2000) t(i)");

    std::vector<std::pair<std::uint64_t, std::uint64_t>> start_count;
    std::uint64_t seg_index_of_row_1000 = 99;
    WithRowGroups(con, "s", "logs", [&](duckdb::RowGroupCollection& rgc) {
        auto tree = rgc.GetRowGroups();
        auto lock = tree->Lock();
        for (std::int64_t i = 0; i < static_cast<std::int64_t>(tree->GetSegmentCount(lock)); i++) {
            auto node = tree->GetSegmentByIndex(lock, i);
            start_count.emplace_back(node->GetRowStart(), node->GetCount());
        }
        seg_index_of_row_1000 = tree->GetSegmentIndex(lock, 1000);
    });

    REQUIRE(start_count.size() == 2);
    // Group 0 is partial (1000 < 2048) AND not the last group => partial non-terminal.
    REQUIRE(start_count[0].first == 0);
    REQUIRE(start_count[0].second == 1000);
    // Group 1 starts at absolute row 1000, NOT at a multiple of the row group size.
    REQUIRE(start_count[1].first == 1000);
    // So the naive `rowid / row_group_size` formula is WRONG here: row 1000 is in
    // row group 1, but 1000 / 2048 == 0. Always ask the SegmentTree, never divide.
    REQUIRE(seg_index_of_row_1000 == 1);
    REQUIRE(1000 / rg == 0);
}

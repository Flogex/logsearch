// Pins down the DuckDB scan behaviour the logsearch index build relies on. Nothing here touches logsearch code: if a
// DuckDB upgrade changes how a parallel table scan hands out rows, these tests fail instead of the index silently
// producing unordered postings.

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <catch2/matchers/catch_matchers_vector.hpp>
#include <cstdint>
#include <duckdb/catalog/catalog.hpp>
#include <duckdb/common/enum_util.hpp>
#include <duckdb/common/exception.hpp>
#include <duckdb/common/mutex.hpp>
#include <duckdb/common/thread_annotation.hpp>
#include <duckdb/common/typedefs.hpp>
#include <duckdb/common/types/data_chunk.hpp>
#include <duckdb/common/types/vector.hpp>
#include <duckdb/execution/index/index_type.hpp>
#include <duckdb/execution/index/index_type_set.hpp>
#include <duckdb/function/scalar_function.hpp>
#include <duckdb/main/client_context.hpp>
#include <duckdb/main/connection.hpp>
#include <duckdb/main/database.hpp>
#include <duckdb/parser/parsed_data/create_index_info.hpp>
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>
#include <duckdb/planner/expression.hpp>
#include <duckdb/storage/storage_info.hpp>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <unordered_set>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::Equals;
using namespace std::string_literals;
using duckdb::idx_t;
using duckdb::row_t;

namespace {

//! What one scan task observed. Only the aggregates the assertions need, so the probe stays cheap enough not to
//! serialize the scan it is measuring.
struct TaskObservation {
    //! Row IDs arrived in non-decreasing order.
    bool ascending = true;
    //! The row groups this task pulled rows from.
    std::unordered_set<idx_t> row_groups;
    row_t last_row_id = -1;
    idx_t last_row_group = 0;
};

//! Collects one TaskObservation per scan task.
class ScanProbe {
public:
    void Record(const void* task, const row_t* row_ids, const idx_t count, const idx_t row_group_size) {
        const duckdb::annotated_lock_guard<duckdb::annotated_mutex> guard(lock_);
        TaskObservation& observation = observations_[task];
        for (idx_t i = 0; i < count; i++) {
            const row_t row_id = row_ids[i];
            observation.ascending = observation.ascending && row_id >= observation.last_row_id;
            observation.last_row_id = row_id;

            const idx_t row_group = static_cast<idx_t>(row_id) / row_group_size;
            if (observation.row_groups.empty() || row_group != observation.last_row_group) {
                observation.row_groups.insert(row_group);
                observation.last_row_group = row_group;
            }
        }
    }

    [[nodiscard]] std::map<const void*, TaskObservation> Take() {
        const duckdb::annotated_lock_guard<duckdb::annotated_mutex> guard(lock_);
        return std::move(observations_);
    }

private:
    duckdb::annotated_mutex lock_;
    std::map<const void*, TaskObservation> observations_ DUCKDB_GUARDED_BY(lock_);
};

//! Reports every batch of values a probe function saw, tagged with the task that evaluated them.
using ProbeCallback = std::function<void(const void* task, const row_t* values, idx_t count)>;

//! Registers `name`(BIGINT) -> BIGINT, which forwards its argument and hands every batch to `record`.
//!
//! The ExpressionState is created once per ExpressionExecutor and an ExpressionExecutor belongs to one operator state,
//! so its address identifies the task that evaluated the row. That is exactly the unit the index build keys its
//! partitions on.
void RegisterProbe(duckdb::Connection& con, const char* name, const duckdb::FunctionStability stability,
                   ProbeCallback record) {
    duckdb::ScalarFunction function(
        name,
        {duckdb::LogicalType::BIGINT},
        duckdb::LogicalType::BIGINT,
        [record = std::move(record)](duckdb::DataChunk& args, duckdb::ExpressionState& state, duckdb::Vector& result) {
            args.Flatten();
            record(&state, duckdb::FlatVector::GetData<row_t>(args.data[0]), args.size());
            result.Reference(args.data[0]);
        });
    function.SetStability(stability);

    con.BeginTransaction();
    duckdb::CreateScalarFunctionInfo info(function);
    duckdb::Catalog::GetSystemCatalog(*con.context).CreateFunction(*con.context, info);
    con.Commit();
}

//! Registers scan_probe(BIGINT) -> BIGINT, recording per-task observations in `probe`.
void RegisterScanProbe(duckdb::Connection& con, ScanProbe& probe, const idx_t row_group_size) {
    // Reading a row ID must not be folded away or cached.
    RegisterProbe(con,
                  "scan_probe",
                  duckdb::FunctionStability::VOLATILE,
                  [&probe, row_group_size](const void* task, const row_t* values, const idx_t count) {
                      probe.Record(task, values, count, row_group_size);
                  });
}

//! Collects every value a probe saw, in no particular order.
class ValueProbe {
public:
    void Record(const row_t* values, const idx_t count) {
        const duckdb::annotated_lock_guard<duckdb::annotated_mutex> guard(lock_);
        values_.insert(values_.end(), values, values + count);
    }

    [[nodiscard]] std::vector<row_t> TakeSorted() {
        const duckdb::annotated_lock_guard<duckdb::annotated_mutex> guard(lock_);
        std::vector<row_t> result = std::move(values_);
        values_.clear();
        std::sort(result.begin(), result.end());
        return result;
    }

private:
    duckdb::annotated_mutex lock_;
    std::vector<row_t> values_ DUCKDB_GUARDED_BY(lock_);
};

//! The physical plan that EXPLAIN prints for `sql`, without running it.
std::string ExplainPlan(duckdb::Connection& con, const std::string& sql) {
    const auto result = con.Query("EXPLAIN " + sql);
    INFO(sql);
    INFO((result->HasError() ? result->GetError() : std::string{}));
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->RowCount() == 1);
    return result->GetValue(1, 0).ToString();
}

void Run(duckdb::Connection& con, const std::string& sql) {
    const auto result = con.Query(sql);
    INFO(sql);
    INFO((result->HasError() ? result->GetError() : std::string{}));
    REQUIRE_FALSE(result->HasError());
}

//! What the index-type probe below saw. IndexType callbacks are plain function pointers with nowhere to put a
//! capture, and only one CREATE INDEX runs at a time per test case, so a namespace-scope slot does the job.
struct ProbeCapture {
    //! Recorded in build_bind, one entry per index key.
    std::vector<std::string> input_expression_classes;
    std::vector<std::string> info_expression_classes;
    std::vector<std::string> info_parsed_expression_classes;
    std::vector<std::string> input_expression_types;
    //! Recorded in build_global_init.
    std::vector<duckdb::column_t> logical_column_ids;
    std::vector<duckdb::column_t> physical_column_ids;
    bool captured = false;
    //! Recorded in build_sink, one entry per chunk plus totals over all of them.
    std::vector<std::string> document_vector_types;
    std::vector<std::string> row_id_vector_types;
    duckdb::idx_t sunk_rows = 0;
    duckdb::idx_t sunk_null_documents = 0;
    bool sink_reached = false;
};
ProbeCapture probe_capture;
//! build_sink is the only probe callback that several tasks can enter at once.
std::mutex probe_sink_lock;

//! Both Expression and ParsedExpression derive from BaseExpression, which is where GetExpressionClass lives.
std::string ClassOf(const duckdb::BaseExpression& expression) {
    return duckdb::EnumUtil::ToChars(expression.GetExpressionClass());
}

duckdb::unique_ptr<duckdb::IndexBuildBindData> ProbeIndexBind(duckdb::IndexBuildBindInput& input) {
    for (const auto& expression : input.expressions) {
        probe_capture.input_expression_classes.push_back(ClassOf(*expression));
        probe_capture.input_expression_types.push_back(expression->GetReturnType().ToString());
    }
    for (const auto& expression : input.info.expressions) {
        probe_capture.info_expression_classes.push_back(ClassOf(*expression));
    }
    for (const auto& expression : input.info.parsed_expressions) {
        probe_capture.info_parsed_expression_classes.push_back(ClassOf(*expression));
    }
    return duckdb::make_uniq<duckdb::IndexBuildBindData>();
}

duckdb::unique_ptr<duckdb::IndexBuildGlobalState> ProbeIndexGlobalInit(duckdb::IndexBuildInitGlobalStateInput& input) {
    probe_capture.logical_column_ids.assign(input.info.column_ids.begin(), input.info.column_ids.end());
    probe_capture.physical_column_ids.assign(input.storage_ids.begin(), input.storage_ids.end());
    probe_capture.captured = true;
    return duckdb::make_uniq<duckdb::IndexBuildGlobalState>();
}

duckdb::unique_ptr<duckdb::IndexBuildLocalState> ProbeIndexLocalInit(duckdb::IndexBuildInitLocalStateInput& /*input*/) {
    return duckdb::make_uniq<duckdb::IndexBuildLocalState>();
}

void ProbeIndexSink(duckdb::IndexBuildSinkInput& /*input*/, duckdb::DataChunk& document_chunk,
                    duckdb::DataChunk& row_id_chunk) {
    const std::lock_guard<std::mutex> guard(probe_sink_lock);
    probe_capture.document_vector_types.emplace_back(duckdb::EnumUtil::ToChars(document_chunk.data[0].GetVectorType()));
    probe_capture.row_id_vector_types.emplace_back(duckdb::EnumUtil::ToChars(row_id_chunk.data[0].GetVectorType()));
    probe_capture.sunk_rows += document_chunk.size();

    // Read through a unified format so this works whatever vector type actually arrives, rather than assuming the
    // flat layout that is itself under test.
    duckdb::UnifiedVectorFormat documents;
    document_chunk.data[0].ToUnifiedFormat(documents);
    for (duckdb::idx_t i = 0; i < document_chunk.size(); i++) {
        if (!documents.validity.RowIsValid(documents.sel->get_index(i))) {
            probe_capture.sunk_null_documents++;
        }
    }
    probe_capture.sink_reached = true;
}

void ProbeIndexCombine(duckdb::IndexBuildCombineInput& /*input*/) {
}

duckdb::unique_ptr<duckdb::BoundIndex> ProbeIndexFinalize(duckdb::IndexBuildFinalizeInput& /*input*/) {
    // Everything the probe wants has been recorded by now. Aborting here keeps it from having to implement a whole
    // BoundIndex just to satisfy the finalize contract and the catalog registration that follows it.
    throw duckdb::NotImplementedException("index build probe");
}

//! Registers an index type that records what the build hands it and then aborts before producing an index.
void RegisterIndexProbe(duckdb::DuckDB& db) {
    duckdb::IndexType probe;
    probe.name = "index_probe";
    probe.build_bind = ProbeIndexBind;
    probe.build_global_init = ProbeIndexGlobalInit;
    probe.build_local_init = ProbeIndexLocalInit;
    probe.build_sink = ProbeIndexSink;
    probe.build_combine = ProbeIndexCombine;
    probe.build_finalize = ProbeIndexFinalize;
    db.instance->config.GetIndexTypes().RegisterIndexType(probe);
}

//! Run `sql`, require that it fails, and return what the probe captured on the way.
ProbeCapture RunProbe(duckdb::Connection& con, const std::string& sql) {
    probe_capture = {};
    const auto result = con.Query(sql);
    INFO(sql);
    REQUIRE(result->HasError());
    REQUIRE(probe_capture.captured);
    return probe_capture;
}

} // namespace

TEST_CASE("A parallel table scan gives each row group to exactly one task, in ascending row order",
          "[duckdb_assumptions]") {
    // The logsearch index build assembles one partition per row group in task-local state without any locking. That is
    // only correct because a row group belongs to a single task and a task sees ascending row IDs.
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);

    ScanProbe probe;
    RegisterScanProbe(con, probe, DEFAULT_ROW_GROUP_SIZE);

    constexpr idx_t row_group_count = 8;
    Run(con, "SET threads = 4");
    Run(con,
        "CREATE TABLE big AS SELECT i FROM range(" + std::to_string(row_group_count * DEFAULT_ROW_GROUP_SIZE) +
            ") t(i)");
    Run(con, "SELECT max(scan_probe(rowid)) FROM big");

    const std::map<const void*, TaskObservation> observations = probe.Take();
    INFO("scan tasks: " << observations.size());
    // With eight row groups over four threads the scan has to run on more than one task, otherwise the exclusivity
    // check below would hold vacuously.
    REQUIRE(observations.size() > 1);

    std::unordered_set<idx_t> claimed_row_groups;
    for (const auto& [task, observation] : observations) {
        CHECK(observation.ascending);
        for (const idx_t row_group : observation.row_groups) {
            INFO("row group " << row_group);
            CHECK(claimed_row_groups.insert(row_group).second);
        }
    }
    // Every row group was scanned, so no task was skipped in the check above.
    CHECK(claimed_row_groups.size() == row_group_count);
}

TEST_CASE("Row groups start at multiples of the row group size", "[duckdb_assumptions]") {
    // The index build derives a row group from a row ID by dividing, rather than consulting the row group tree.
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);

    Run(con, "CREATE TABLE big AS SELECT i FROM range(" + std::to_string(3 * DEFAULT_ROW_GROUP_SIZE) + ") t(i)");
    // DELETE leaves gaps in the row IDs but must not move the surviving rows to different row groups.
    Run(con, "DELETE FROM big WHERE i % 3 = 0");

    const auto result = con.Query("SELECT min(rowid), max(rowid), count(*) FROM big");
    REQUIRE_FALSE(result->HasError());
    REQUIRE(result->RowCount() == 1);
    CHECK(result->GetValue(0, 0).GetValue<int64_t>() == 1);
    CHECK(result->GetValue(1, 0).GetValue<int64_t>() == (3 * DEFAULT_ROW_GROUP_SIZE) - 1);

    // Every row group boundary is a multiple of DEFAULT_ROW_GROUP_SIZE, so a row ID never crosses into a row group
    // that a lower row ID already belongs to.
    const auto groups =
        con.Query("SELECT count(DISTINCT rowid // " + std::to_string(DEFAULT_ROW_GROUP_SIZE) + ") FROM big");
    REQUIRE_FALSE(groups->HasError());
    CHECK(groups->GetValue(0, 0).GetValue<int64_t>() == 3);
}

TEST_CASE("info.column_ids are logical and storage_ids are physical column ids", "[duckdb_assumptions]") {
    // A generated column occupies a logical position but has no storage, so from there on the two id spaces diverge.
    // BoundIndex wants the physical ones (index.hpp), and that is what the build callbacks get as storage_ids -
    // info.column_ids is still logical at that point.
    duckdb::DuckDB db(nullptr);
    RegisterIndexProbe(db);
    duckdb::Connection con(db);

    Run(con, "CREATE TABLE t (a INTEGER, gen AS (2 * a), b INTEGER, c VARCHAR)");
    Run(con, "INSERT INTO t (a, b, c) VALUES (1, 2, 'x')");

    // Logical order is a=0, gen=1, b=2, c=3; physical order is a=0, b=1, c=2.
    const ProbeCapture generated = RunProbe(con, "CREATE INDEX idx ON t USING index_probe (c)");
    CHECK_THAT(generated.logical_column_ids, Equals(std::vector<duckdb::column_t>{3}));
    CHECK_THAT(generated.physical_column_ids, Equals(std::vector<duckdb::column_t>{2}));

    // Control: without a generated column the two spaces coincide, so the gap above really is caused by `gen`.
    Run(con, "CREATE TABLE plain (a INTEGER, b INTEGER, c VARCHAR)");
    const ProbeCapture plain = RunProbe(con, "CREATE INDEX plain_idx ON plain USING index_probe (c)");
    CHECK_THAT(plain.logical_column_ids, Equals(std::vector<duckdb::column_t>{2}));
    CHECK_THAT(plain.physical_column_ids, Equals(std::vector<duckdb::column_t>{2}));
}

TEST_CASE("build_bind receives bound expressions, while CreateIndexInfo keeps the parsed ones",
          "[duckdb_assumptions]") {
    // IndexBuildBindInput::expressions is what an index type has to work with: already bound, so the expression
    // class and the return type are resolved. CreateIndexInfo carries the parser's view, and IndexBinder consumes
    // `info.expressions` while binding - `info.parsed_expressions` is the copy that survives it.
    duckdb::DuckDB db(nullptr);
    RegisterIndexProbe(db);
    duckdb::Connection con(db);

    Run(con, "CREATE TABLE t (id INTEGER, message VARCHAR)");
    const ProbeCapture capture = RunProbe(con, "CREATE INDEX idx ON t USING index_probe (message)");

    // Bound: a BoundColumnRefExpression with a resolved type, which is what lets an index type check its key.
    CHECK_THAT(capture.input_expression_classes, Equals(std::vector{"BOUND_COLUMN_REF"s}));
    CHECK_THAT(capture.input_expression_types, Equals(std::vector{"VARCHAR"s}));

    // Not parsed column refs any more: the binder replaced each entry in place with the husk it moved the bound
    // expression out of, so reading info.expressions in a build callback gets nothing useful.
    CHECK_THAT(capture.info_expression_classes, Equals(std::vector{"BOUND_EXPRESSION"s}));

    // The durable copy is untouched and still in parser form. This is what the catalog entry keeps and what an
    // index is re-bound from after a restart.
    CHECK_THAT(capture.info_parsed_expression_classes, Equals(std::vector{"COLUMN_REF"s}));
}

TEST_CASE("The index sink is handed flat vectors", "[duckdb_assumptions]") {
    // PhysicalCreateIndex::Sink calls DataChunk::Flatten before referencing the key and row ID columns, so a build
    // sink may read them with FlatVector::GetData rather than going through a UnifiedVectorFormat.
    duckdb::DuckDB db(nullptr);
    RegisterIndexProbe(db);
    duckdb::Connection con(db);

    Run(con, "CREATE TABLE t (message VARCHAR)");
    Run(con, "INSERT INTO t SELECT 'same value for every row' FROM range(5)");
    const ProbeCapture capture = RunProbe(con, "CREATE INDEX idx ON t USING index_probe (message)");

    REQUIRE(capture.sink_reached);
    CHECK_THAT(capture.document_vector_types, Equals(std::vector{"FLAT_VECTOR"s}));
    CHECK_THAT(capture.row_id_vector_types, Equals(std::vector{"FLAT_VECTOR"s}));
}

TEST_CASE("NULL documents never reach the index sink", "[duckdb_assumptions]") {
    // The consequence of the IS NOT NULL filter below, seen from the sink: a build sink can treat every row it gets
    // as a valid document instead of checking the validity mask per row.
    duckdb::DuckDB db(nullptr);
    RegisterIndexProbe(db);
    duckdb::Connection con(db);

    Run(con, "CREATE TABLE t (message VARCHAR)");
    Run(con, "INSERT INTO t VALUES ('alpha'), (NULL), ('beta'), (NULL), ('gamma')");
    const ProbeCapture capture = RunProbe(con, "CREATE INDEX idx ON t USING index_probe (message)");

    REQUIRE(capture.sink_reached);
    CHECK(capture.sunk_rows == 3);
    CHECK(capture.sunk_null_documents == 0);
}

TEST_CASE("CREATE INDEX filters NULL keys out before the index sink", "[duckdb_assumptions]") {
    // The build sink treats a NULL key as contributing no terms rather than rejecting it, because the planner is
    // supposed to have removed those rows already: plan_create_index.cpp inserts an IS NOT NULL filter between the
    // projection that evaluates the key and the sink. Note the filter sits *above* the projection, so the key
    // expression is still evaluated for NULL rows -- only the sink is shielded.
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);

    Run(con, "CREATE TABLE t (id BIGINT NOT NULL, v VARCHAR)");
    Run(con, "INSERT INTO t VALUES (1, 'a'), (2, NULL)");

    const std::string create_plan = ExplainPlan(con, "CREATE INDEX i ON t USING ART (v)");
    INFO(create_plan);
    CHECK_THAT(create_plan, ContainsSubstring("IS NOT NULL"));

    // The same operator without the filter: `need_filter = op.alter_table_info == nullptr`. This path is out of
    // reach for us anyway, because bind_alter.cpp hardcodes ART with a PRIMARY constraint, and a PRIMARY constraint
    // is rejected in LogsearchBuildBind.
    const std::string alter_plan = ExplainPlan(con, "ALTER TABLE t ADD PRIMARY KEY (id)");
    INFO(alter_plan);
    CHECK(alter_plan.find("IS NOT NULL") == std::string::npos);

    // Both really are index builds, so the check above cannot pass just because EXPLAIN printed something else.
    CHECK_THAT(create_plan, ContainsSubstring("Create Index"));
    CHECK_THAT(alter_plan, ContainsSubstring("Create Index"));
}

TEST_CASE("A CREATE INDEX scan does not see transaction-local rows", "[duckdb_assumptions]") {
    // Uncommitted rows carry row IDs at or above MAX_ROW_ID that are rewritten on commit, so an index must never
    // store them. IndexBinder::BindCreateIndex sets TableScanBindData::is_create_index, which routes the scan to
    // DataTable::CreateIndexScan; unlike DataTable::Scan that one reads the persistent row groups only.
    duckdb::DuckDB db(nullptr);
    duckdb::Connection con(db);

    // The probe has to be CONSISTENT: the planner rejects an index key that is not, and volatility buys nothing
    // here because the argument is a column rather than a constant.
    ValueProbe probe;
    RegisterProbe(con,
                  "index_probe",
                  duckdb::FunctionStability::CONSISTENT,
                  [&probe](const void*, const row_t* values, const idx_t count) { probe.Record(values, count); });

    Run(con, "CREATE TABLE t (id BIGINT)");
    Run(con, "INSERT INTO t VALUES (1), (2)");
    Run(con, "BEGIN TRANSACTION");
    Run(con, "INSERT INTO t VALUES (3)");

    // A plain scan inside the transaction does see the local row, and it is numbered above MAX_ROW_ID.
    const auto local = con.Query("SELECT count(*), max(rowid) FROM t");
    REQUIRE_FALSE(local->HasError());
    CHECK(local->GetValue(0, 0).GetValue<int64_t>() == 3);
    CHECK(local->GetValue(1, 0).GetValue<int64_t>() >= duckdb::MAX_ROW_ID);

    // Indexing an expression over the key routes every scanned row through the probe, so what it records is exactly
    // what the CREATE INDEX scan delivered.
    Run(con, "CREATE INDEX idx ON t USING ART (index_probe(id))");
    CHECK_THAT(probe.TakeSorted(), Equals(std::vector<row_t>{1, 2}));

    // The local row is not lost, it just reaches the index later: committing appends it, which evaluates its key.
    Run(con, "COMMIT");
    CHECK_THAT(probe.TakeSorted(), Equals(std::vector<row_t>{3}));

    // And once it is committed, a rebuild scans it like any other row. Without this the check above would also pass
    // if the probe had simply stopped recording.
    Run(con, "CREATE INDEX idx2 ON t USING ART (index_probe(id))");
    CHECK_THAT(probe.TakeSorted(), Equals(std::vector<row_t>{1, 2, 3}));
}

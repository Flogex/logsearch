#include "logsearch_index.hpp"

#include "duckdb/execution/index/index_type.hpp"
#include "duckdb/execution/operator/scan/physical_dummy_scan.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"
#include "duckdb/planner/operator/logical_create_index.hpp"

duckdb::IndexType LogsearchIndex::GetIndexType() {
    duckdb::IndexType logsearch_type;
    logsearch_type.name = LogsearchIndex::NAME;
    // Stub: emit a no-op physical plan so CREATE INDEX ... USING logsearch
    // succeeds without building an actual index. Replace with real build
    // pipeline (build_bind/build_sink/.../create_instance) once implemented.
    logsearch_type.create_plan = [](duckdb::PlanIndexInput& input) -> duckdb::PhysicalOperator& {
        return input.planner.Make<duckdb::PhysicalDummyScan>(input.op.types, input.op.estimated_cardinality);
    };
    return logsearch_type;
}

#ifndef DUCKDB_INDEX_UNITY_BUILD
#error "logsearch_index_type.cpp must be compiled as part of the DuckDB-Index unity build"
#endif

#include "logsearch_index_type.hpp"

#include "analyzer/pipeline.hpp"
#include "inverted-index/inverted_index.hpp"
#include "logsearch_index.hpp"
#include "logsearch_index_builder.hpp"

#include <duckdb/catalog/catalog_entry/duck_table_entry.hpp>
#include <duckdb/common/assert.hpp>
#include <duckdb/common/constants.hpp>
#include <duckdb/common/enums/expression_type.hpp>
#include <duckdb/common/exception.hpp>
#include <duckdb/common/exception/binder_exception.hpp>
#include <duckdb/common/helper.hpp>
#include <duckdb/common/mutex.hpp>
#include <duckdb/common/query_context.hpp>
#include <duckdb/common/sql_identifier.hpp>
#include <duckdb/common/string_util.hpp>
#include <duckdb/common/thread_annotation.hpp>
#include <duckdb/common/types.hpp>
#include <duckdb/common/types/data_chunk.hpp>
#include <duckdb/common/types/string_type.hpp>
#include <duckdb/common/types/vector.hpp>
#include <duckdb/main/client_config.hpp>
#include <duckdb/main/client_context.hpp>
#include <duckdb/parser/parsed_data/create_index_info.hpp>
#include <duckdb/planner/expression.hpp>
#include <duckdb/storage/buffer_manager.hpp>
#include <duckdb/storage/data_table.hpp>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace logsearch {
namespace {

/***** Binding *****/

class LogsearchBuildBindData : public duckdb::IndexBuildBindData {};

// Called during `CREATE INDEX <name> ON <table> USING logsearch (<column expression>)`.
// We currently only support creating the index on a single VARCHAR column.
duckdb::unique_ptr<duckdb::IndexBuildBindData> LogsearchBuildBind(duckdb::IndexBuildBindInput& input) {
    D_ASSERT(duckdb::StringUtil::CIEquals(input.info.index_type, LogsearchIndex::NAME));

    // CREATE UNIQUE INDEX ... USING logsearch reaches us with a UNIQUE constraint type.
    if (input.info.constraint_type != duckdb::IndexConstraintType::NONE) {
        throw duckdb::BinderException("A Logsearch index cannot enforce a UNIQUE or PRIMARY KEY constraint");
    }
    if (input.expressions.size() != 1) {
        throw duckdb::NotImplementedException("A Logsearch index must cover exactly one column, but %llu were given",
                                              static_cast<unsigned long long>(input.expressions.size()));
    }
    // TODO: Support arbitrary expressions such as `(varchar_col | 'tagged')`.
    const auto& col_expression = input.expressions[0];
    if (col_expression->GetExpressionClass() != duckdb::ExpressionClass::BOUND_COLUMN_REF) {
        throw duckdb::BinderException("A Logsearch index can only be created on a column reference expression");
    }
    const auto col_type = col_expression->GetReturnType();
    if (col_type != duckdb::LogicalTypeId::VARCHAR) {
        // ExceptionFormatValue only accepts strings and numbers, so render the identifier here rather than passing
        // the SQLQuotedIdentifier itself.
        const auto col_name = duckdb::SQLQuotedIdentifier::ToString(col_expression->GetName().GetIdentifierName());
        throw duckdb::BinderException("A Logsearch index can only be created on a VARCHAR column, but %s is of type %s",
                                      col_name,
                                      col_type.ToString());
    }

    if (!input.info.options.empty()) {
        throw duckdb::BinderException("A Logsearch index takes no options, but \"%s\" was given",
                                      input.info.options.begin()->first);
    }

    // The build partitions by row group and folds the per-task indexes together by concatenating their partitions,
    // which requires that no two tasks see rows of the same row group. verify_parallelism breaks that: it hands out
    // one vector per scan task instead of one row group (see duckdb::RowGroupCollection::NextParallelScan).
    if (duckdb::ClientConfig::GetConfig(input.context).verify_parallelism) {
        throw duckdb::NotImplementedException(
            "A Logsearch index cannot be created when verify_parallelism is enabled, because it splits a row group "
            "across scan tasks. Run PRAGMA disable_verify_parallelism first");
    }
    return duckdb::make_uniq<LogsearchBuildBindData>();
}

/***** Build state *****/

//! Folds the per-task indexes together as they arrive. The first task's index becomes the one all others merge into.
struct LogsearchBuildGlobalState : duckdb::IndexBuildGlobalState {
    // The constructor parameters are named apart from the members so that they do not shadow them.
    LogsearchBuildGlobalState(const duckdb::QueryContext query_context, LogsearchIndexBuilder index_builder)
        : context(query_context), builder(std::move(index_builder)) {
    }

    const duckdb::QueryContext context;
    duckdb::annotated_mutex lock;
    //! Stores the parameters required to construct a duckdb::BoundIndex until the Finalize.
    LogsearchIndexBuilder builder;
    std::unique_ptr<inverted_index::InvertedIndex> merged DUCKDB_GUARDED_BY(lock);
};

//! Holds the InvertedIndex created by one task.
// We build one InvertedIndex instance per task and merge them together when finalizing the build.
// DuckDB assigns a whole row group to one scan task (see duckdb::RowGroupCollection::NextParallelScan).
// Each task consumes possibly multiple row groups in ascending order.
struct LogsearchBuildLocalState : duckdb::IndexBuildLocalState {
    LogsearchBuildLocalState(duckdb::ClientContext& context, const duckdb::idx_t row_group_size)
        // The index stores this BufferManager by reference and outlives the connection that built it, so it has to
        // be the one owned by duckdb::DatabaseInstance (instead of the decorated one from duckdb::ClientContext).
        : LogsearchBuildLocalState(duckdb::QueryContext(context), duckdb::BufferManager::GetBufferManager(*context.db),
                                   row_group_size) {
    }

    const duckdb::QueryContext context;
    analyzer::Pipeline analyzer;
    inverted_index::InvertedIndex index;

private:
    LogsearchBuildLocalState(const duckdb::QueryContext context, duckdb::BufferManager& bm,
                             const duckdb::idx_t row_group_size)
        // The buffer allocator counts the analyzer scratch space against the memory limit.
        : context(context), analyzer(bm.GetBufferAllocator()), index(bm, row_group_size) {
    }
};

/***** Build pipeline *****/

// A sort orders by the indexed expression and would destroy the ascending row-ID order that the partitioning
// below relies on.
bool LogsearchBuildSort(duckdb::IndexBuildSortInput& /*input*/) {
    return false;
}

duckdb::unique_ptr<duckdb::IndexBuildGlobalState>
LogsearchBuildGlobalInit(duckdb::IndexBuildInitGlobalStateInput& input) {
    // We need to store all the parameters that we later pass to BoundIndex because in Finalize we don't get all this
    // information from IndexBuildFinalizeInput. The duckdb::Index wants the physical column IDs of the indexed column.
    // `input.storage_ids` gives us the physical column IDs, whereas `input.info.column_ids` would give us the logical
    // column IDs. The duckdb::BoundIndex constructor accepts a vector of "unbound_expressions". Those are bound
    // expressions whose leaves are BoundColumnRefExpressions, so they do carry a ColumnBinding. "Unbound" only means
    // that they have not been rewritten into BoundReferenceExpressions addressing an incoming DataChunk yet. Therefore,
    // passing `input.expressions`, which stores bound expressions, is correct.
    duckdb::DataTable& data_table = input.table.GetStorage();
    LogsearchIndexBuilder builder(input.info.GetIndexName(),
                                  input.info.index_type,
                                  input.info.constraint_type,
                                  input.storage_ids,
                                  data_table.GetTableIOManager(),
                                  input.expressions,
                                  data_table.db);
    return duckdb::make_uniq<LogsearchBuildGlobalState>(duckdb::QueryContext(input.context), std::move(builder));
}

duckdb::unique_ptr<duckdb::IndexBuildLocalState> LogsearchBuildLocalInit(duckdb::IndexBuildInitLocalStateInput& input) {
    return duckdb::make_uniq<LogsearchBuildLocalState>(input.context, input.table.GetStorage().GetRowGroupSize());
}

void LogsearchBuildSink(duckdb::IndexBuildSinkInput& input, duckdb::DataChunk& document_chunk,
                        duckdb::DataChunk& row_id_chunk) {
    auto& state = input.local_state.Cast<LogsearchBuildLocalState>();

    D_ASSERT(document_chunk.ColumnCount() == 1);
    D_ASSERT(row_id_chunk.ColumnCount() == 1);
    // PhysicalCreateIndex::Sink flattens the chunk before referencing the document and row ID columns. Flat is
    // also the only layout the loop below can read.
    D_ASSERT(document_chunk.data[0].GetVectorType() == duckdb::VectorType::FLAT_VECTOR);
    D_ASSERT(row_id_chunk.data[0].GetVectorType() == duckdb::VectorType::FLAT_VECTOR);
    // The planner puts an IS NOT NULL filter in front of the sink, so a NULL document never gets here.
    D_ASSERT(duckdb::FlatVector::Validity(document_chunk.data[0]).CheckAllValid(document_chunk.size()));
    D_ASSERT(duckdb::FlatVector::Validity(row_id_chunk.data[0]).CheckAllValid(row_id_chunk.size()));
    const auto* documents = duckdb::FlatVector::GetData<duckdb::string_t>(document_chunk.data[0]);
    const auto* row_ids = duckdb::FlatVector::GetData<duckdb::row_t>(row_id_chunk.data[0]);

    for (duckdb::idx_t i = 0; i < document_chunk.size(); i++) {
        const std::string_view document(documents[i].GetData(), documents[i].GetSize());
        const duckdb::row_t row_id = row_ids[i];
        // CREATE INDEX only runs over committed data
        // TODO: Decide where to do these checks, probably here at outer layer
        D_ASSERT(row_id >= 0 && row_id <= duckdb::MAX_ROW_ID);

        for (const std::string& term : state.analyzer.Run(document)) {
            state.index.Insert(term, row_id, state.context);
        }
    }
}

void LogsearchBuildCombine(duckdb::IndexBuildCombineInput& input) {
    auto& global_state = input.global_state.Cast<LogsearchBuildGlobalState>();
    auto& local_state = input.local_state.Cast<LogsearchBuildLocalState>();

    const duckdb::annotated_lock_guard<duckdb::annotated_mutex> guard(global_state.lock);
    if (!global_state.merged) {
        // The active partition of the index doesn't get sealed here. This only happens during PairwiseMerge.
        // If we only see a single InvertedIndex during the build, then the Memtable will stay open and only
        // be sealed during a subsequent Insert once full.
        global_state.merged = std::make_unique<inverted_index::InvertedIndex>(std::move(local_state.index));
    } else {
        // Merging keeps the partitions ordered, so the tasks may be folded in in any order. `PairwiseMerge` requires
        // disjoint row-ID ranges, which holds because a scan task always receives whole row groups.
        global_state.merged->PairwiseMerge(std::move(local_state.index), global_state.context);
    }
}

//! Wrap the merged inverted index in the BoundIndex that CREATE INDEX registers.
//! Runs once, after every Combine has returned, so the global state needs no locking here. The thread-safety
//! analysis cannot see that and would flag the guarded member, hence the opt-out.
duckdb::unique_ptr<duckdb::BoundIndex>
LogsearchBuildFinalize(duckdb::IndexBuildFinalizeInput& input) DUCKDB_NO_THREAD_SAFETY_ANALYSIS {
    auto& global_state = input.global_state.Cast<LogsearchBuildGlobalState>();

    // Without a single task there is nothing to hand over and the builder produces an empty index.
    if (global_state.merged) {
        global_state.builder.SetInvertedIndex(std::move(global_state.merged));
    }
    return global_state.builder.Build();
}

duckdb::unique_ptr<duckdb::BoundIndex> LogsearchCreateInstance(duckdb::CreateIndexInput& /*input*/) {
    throw duckdb::NotImplementedException("Logsearch indexes cannot be restored from storage yet");
}

} // namespace

duckdb::IndexType CreateLogsearchIndexType() {
    duckdb::IndexType logsearch_type;
    logsearch_type.name = LogsearchIndex::NAME;
    logsearch_type.build_bind = LogsearchBuildBind;
    logsearch_type.build_sort = LogsearchBuildSort;
    logsearch_type.build_global_init = LogsearchBuildGlobalInit;
    logsearch_type.build_local_init = LogsearchBuildLocalInit;
    logsearch_type.build_sink = LogsearchBuildSink;
    logsearch_type.build_combine = LogsearchBuildCombine;
    logsearch_type.build_finalize = LogsearchBuildFinalize;
    logsearch_type.create_instance = LogsearchCreateInstance;
    return logsearch_type;
}

} // namespace logsearch

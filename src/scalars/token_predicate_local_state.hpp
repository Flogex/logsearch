#pragma once

#include "analyzer/pipeline.hpp"

#include <duckdb/common/allocator.hpp>
#include <duckdb/common/helper.hpp>
#include <duckdb/common/unique_ptr.hpp>
#include <duckdb/execution/expression_executor_state.hpp>
#include <duckdb/function/function.hpp>

namespace duckdb {
class BoundFunctionExpression;
}

namespace logsearch::scalars {

//! Per-thread execution state for the token predicates. The predicates analyze the column value of every row they see.
// `analyzer::Pipeline` is not thread-safe, so every thread evaluating a predicate needs its own.
struct TokenPredicateLocalState final : duckdb::FunctionLocalState {
    explicit TokenPredicateLocalState(duckdb::Allocator& allocator) : pipeline(allocator) {
    }

    // Matches `duckdb::init_local_state_t`, so it can be handed to a ScalarFunction as-is.
    static duckdb::unique_ptr<duckdb::FunctionLocalState> Init(duckdb::ExpressionState& state,
                                                               const duckdb::BoundFunctionExpression& /*expr*/,
                                                               duckdb::FunctionData* /*bind_data*/) {
        return duckdb::make_uniq<TokenPredicateLocalState>(duckdb::Allocator::Get(state.GetContext()));
    }

    static TokenPredicateLocalState& Get(duckdb::ExpressionState& state) {
        return duckdb::ExecuteFunctionState::GetFunctionState(state)->Cast<TokenPredicateLocalState>();
    }

    analyzer::Pipeline pipeline;
};

} // namespace logsearch::scalars

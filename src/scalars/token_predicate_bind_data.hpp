#pragma once

#include <duckdb/common/helper.hpp>
#include <duckdb/common/unique_ptr.hpp>
#include <duckdb/execution/expression_executor_state.hpp>
#include <duckdb/function/function.hpp>
#include <duckdb/planner/expression/bound_function_expression.hpp>
#include <string>
#include <utility>
#include <vector>

namespace logsearch::scalars {

//! Bind data for the token predicate scalar functions.
//! Stores the already analyzed search terms which are provided as constants and resolved during binding.
struct TokenPredicateBindData final : duckdb::FunctionData {
    explicit TokenPredicateBindData(std::vector<std::string> terms_p) : terms(std::move(terms_p)) {
    }

    [[nodiscard]] static const TokenPredicateBindData& Get(const duckdb::ExpressionState& state) {
        const auto& expr = state.expr.Cast<duckdb::BoundFunctionExpression>();
        return expr.BindInfo()->Cast<TokenPredicateBindData>();
    }

    //! The distinct (analyzed) terms the query argument asks for, in first-seen order.
    std::vector<std::string> terms;

    [[nodiscard]] duckdb::unique_ptr<duckdb::FunctionData> Copy() const override {
        return duckdb::make_uniq<TokenPredicateBindData>(terms);
    }

    [[nodiscard]] bool Equals(const duckdb::FunctionData& other) const override {
        return terms == other.Cast<TokenPredicateBindData>().terms;
    }

    // We don't overwrite serialize/deserialize because the roundtrip works by rebinding.
};

} // namespace logsearch::scalars

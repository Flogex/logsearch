#include "token_predicates.hpp"

#include "constants.hpp"
#include "query_tokens.hpp"
#include "token_predicate_bind_data.hpp"
#include "token_predicate_local_state.hpp"

#include <algorithm>
#include <duckdb/common/assert.hpp>
#include <duckdb/common/exception.hpp>
#include <duckdb/common/exception/binder_exception.hpp>
#include <duckdb/common/helper.hpp>
#include <duckdb/common/types.hpp>
#include <duckdb/common/types/data_chunk.hpp>
#include <duckdb/common/types/string_type.hpp>
#include <duckdb/common/types/value.hpp>
#include <duckdb/common/types/vector.hpp>
#include <duckdb/common/unique_ptr.hpp>
#include <duckdb/common/vector_operations/binary_executor.hpp>
#include <duckdb/execution/expression_executor_state.hpp>
#include <duckdb/function/function.hpp>
#include <duckdb/function/scalar_function.hpp>
#include <duckdb/main/extension/extension_loader.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace logsearch::scalars {

namespace {

//! Zero-copy view of a DuckDB string. The bytes stay owned by the vector the string came from.
std::string_view ToStringView(const duckdb::string_t& value) noexcept {
    return {value.GetData(), value.GetSize()};
}

/***** Matching *****/

//! True if `doc_terms` contains `search_term`. Both sides come out of the same analyzer, so terms compare
//! byte for byte. The empty term stands for a query token that analyzed to nothing, which every document
//! satisfies.
bool Contains(const std::vector<std::string>& doc_terms, const std::string& search_term) {
    return search_term.empty() || std::find(doc_terms.begin(), doc_terms.end(), search_term) != doc_terms.end();
}

//! True if `doc_terms` contains every term of `query_terms`. A query that asks for no term at all (empty
//! list or list with only empty terms) is trivially satisfied.
bool MatchesAll(const std::vector<std::string>& doc_terms, const std::vector<std::string>& query_terms) {
    return std::all_of(query_terms.begin(), query_terms.end(), [&doc_terms](const std::string& search_term) {
        return Contains(doc_terms, search_term);
    });
}

//! True if `doc_terms` contains at least one of `query_terms`. An empty list of query tokens is always
//! false, while a list holding only empty terms is always true.
bool MatchesAny(const std::vector<std::string>& doc_terms, const std::vector<std::string>& query_terms) {
    return std::any_of(query_terms.begin(), query_terms.end(), [&doc_terms](const std::string& search_term) {
        return Contains(doc_terms, search_term);
    });
}

/***** Bind *****/

std::optional<duckdb::Value> GetConstantQueryArgument(const duckdb::BindScalarFunctionInput& input,
                                                      const duckdb::LogicalType& declared_type) {
    std::optional<duckdb::Value> constant = input.TryGetConstant(QUERY_ARGUMENT);
    if (!constant) {
        throw duckdb::BinderException("%s only supports a constant second argument for now",
                                      input.GetBoundFunction().GetName());
    }
    // Under FunctionNullHandling::DEFAULT_NULL_HANDLING, which these predicates use, DuckDB replaces a call
    // holding a NULL constant argument with a typed NULL before it binds the function at all, so a NULL the
    // user wrote cannot reach this code.
    D_ASSERT(!constant->IsNull());

    // Cast the query token to the declared type before we use it in the bind data. DuckDB's conversion would only run
    // later.
    if (constant->type() != declared_type && !constant->TryCastAs(input.GetClientContext(), declared_type)) {
        // DuckDB will throw a ConversionException later during binding when the value cannot be casted to the declared
        // parameter type.
        return std::nullopt;
    }

    // Assert that a potential cast cannot produce a NULL value from a non-NULL one.
    D_ASSERT(!constant->IsNull());

    return constant;
}

// duckdb::bind_scalar_function_t takes a non-const reference, so `input` cannot be const here.
// NOLINTNEXTLINE(misc-const-correctness)
duckdb::unique_ptr<duckdb::FunctionData> BindContainsToken(duckdb::BindScalarFunctionInput& input) {
    const std::optional<duckdb::Value> token = GetConstantQueryArgument(input, duckdb::LogicalType::VARCHAR);
    // token is nullopt if the type of the token constant is different from the declared type and cannot be casted to
    // it. In that case, DuckDB will throw a ConversionException later in the binding, so we can just leave the bind
    // data unset.
    if (!token.has_value()) {
        return nullptr;
    }

    QueryTokens query(input);
    // We analyze the token already during binding because we need it in analyzed form at the optimization stage for the
    // index lookup.
    query.AnalyzeAndInsert(duckdb::StringValue::Get(*token));

    return duckdb::make_uniq<TokenPredicateBindData>(std::move(query).TakeTerms());
}

// Shared by contains_all_tokens and contains_any_tokens
// NOLINTNEXTLINE(misc-const-correctness)
duckdb::unique_ptr<duckdb::FunctionData> BindListArgument(duckdb::BindScalarFunctionInput& input) {
    const std::optional<duckdb::Value> token_list =
        GetConstantQueryArgument(input, duckdb::LogicalType::LIST(duckdb::LogicalType::VARCHAR));
    // See BindContainsToken: a constant that cannot be cast to the declared type leaves the bind data unset.
    if (!token_list.has_value()) {
        return nullptr;
    }

    QueryTokens query(input);
    for (const duckdb::Value& token : duckdb::ListValue::GetChildren(*token_list)) {
        if (token.IsNull()) {
            // "Does this document contain NULL" has no useful answer, and returning NULL would silently
            // swallow a malformed query.
            throw duckdb::BinderException("%s does not accept NULL as a list element",
                                          input.GetBoundFunction().GetName());

            // If we want to change this, we should take inspiration from the boolean semantics of NULL:
            //
            // SELECT TRUE AND NULL -> NULL
            // SELECT FALSE AND NULL -> FALSE
            // SELECT TRUE OR NULL -> TRUE
            // SELECT FALSE OR NULL -> NULL
            //
            // This implies, for example, that contains_all_tokens(msg, ['a', NULL]) is FALSE if 'a' is absent and
            // NULL if present. contains_any_tokens(msg, ['a', NULL]) would be TRUE if 'a' is present and NULL if
            // absent.
        }
        query.AnalyzeAndInsert(duckdb::StringValue::Get(token));
    }

    return duckdb::make_uniq<TokenPredicateBindData>(std::move(query).TakeTerms());
}

/***** Execute *****/

void ContainsTokenFunction(duckdb::DataChunk& args, duckdb::ExpressionState& state, duckdb::Vector& result) {
    D_ASSERT(args.ColumnCount() == 2);

    const std::vector<std::string>& query_terms = TokenPredicateBindData::Get(state).terms;
    TokenPredicateLocalState& lstate = TokenPredicateLocalState::Get(state);

    // TODO: Use UnaryExecutor if argument is constant
    duckdb::BinaryExecutor::Execute<duckdb::string_t, duckdb::string_t, bool>(
        // The second argument is constant and was already resolved during binding.
        args.data[0],
        args.data[1],
        result,
        [&](const duckdb::string_t document, const duckdb::string_t /*token*/) {
            const std::vector<std::string> doc_terms = lstate.pipeline.Run(ToStringView(document));
            D_ASSERT(query_terms.size() == 1);
            return MatchesAll(doc_terms, query_terms);
        });
}

void ContainsAllTokensFunction(duckdb::DataChunk& args, duckdb::ExpressionState& state, duckdb::Vector& result) {
    // Same input arguments as ContainsTokenFunction, except that the query argument is a list.
    D_ASSERT(args.ColumnCount() == 2);

    const std::vector<std::string>& query_terms = TokenPredicateBindData::Get(state).terms;
    TokenPredicateLocalState& lstate = TokenPredicateLocalState::Get(state);

    duckdb::BinaryExecutor::Execute<duckdb::string_t, duckdb::list_entry_t, bool>(
        args.data[0],
        args.data[1],
        result,
        [&](const duckdb::string_t document, const duckdb::list_entry_t& /*token list*/) {
            const std::vector<std::string> doc_terms = lstate.pipeline.Run(ToStringView(document));
            return MatchesAll(doc_terms, query_terms);
        });
}

void ContainsAnyTokensFunction(duckdb::DataChunk& args, duckdb::ExpressionState& state, duckdb::Vector& result) {
    // Same input arguments as ContainsTokenFunction, except that the query argument is a list.
    D_ASSERT(args.ColumnCount() == 2);

    const std::vector<std::string>& query_terms = TokenPredicateBindData::Get(state).terms;
    TokenPredicateLocalState& lstate = TokenPredicateLocalState::Get(state);

    duckdb::BinaryExecutor::Execute<duckdb::string_t, duckdb::list_entry_t, bool>(
        args.data[0],
        args.data[1],
        result,
        [&](const duckdb::string_t document, const duckdb::list_entry_t& /*token list*/) {
            const std::vector<std::string> doc_terms = lstate.pipeline.Run(ToStringView(document));
            return MatchesAny(doc_terms, query_terms);
        });
}

} // namespace

// None of the three is marked fallible, although the analyzer can throw. Both of its throws are accidents of the
// current implementation rather than properties of the predicates: non-ASCII input is a NotImplementedException that
// goes away with Unicode support, and a term over the length limit is unreachable through a DuckDB VARCHAR, whose
// maximum length is exactly that limit. Declaring them infallible is what lets DuckDB push a conjunction of them
// into a scan, and it lets the executor evaluate them once per distinct value of a dictionary vector.
void RegisterTokenPredicates(duckdb::ExtensionLoader& loader) {
    const auto token_list_type = duckdb::LogicalType::LIST(duckdb::LogicalType::VARCHAR);

    duckdb::ScalarFunction contains_token(
        CONTAINS_TOKEN_NAME,
        {{"string", duckdb::LogicalType::VARCHAR}, {"token", duckdb::LogicalType::VARCHAR}},
        duckdb::LogicalType::BOOLEAN,
        ContainsTokenFunction,
        BindContainsToken,
        nullptr,
        TokenPredicateLocalState::Init);
    loader.RegisterFunction(std::move(contains_token));

    duckdb::ScalarFunction contains_all_tokens(CONTAINS_ALL_TOKENS_NAME,
                                               {{"string", duckdb::LogicalType::VARCHAR}, {"tokens", token_list_type}},
                                               duckdb::LogicalType::BOOLEAN,
                                               ContainsAllTokensFunction,
                                               BindListArgument,
                                               nullptr,
                                               TokenPredicateLocalState::Init);
    loader.RegisterFunction(std::move(contains_all_tokens));

    duckdb::ScalarFunction contains_any_tokens(CONTAINS_ANY_TOKENS_NAME,
                                               {{"string", duckdb::LogicalType::VARCHAR}, {"tokens", token_list_type}},
                                               duckdb::LogicalType::BOOLEAN,
                                               ContainsAnyTokensFunction,
                                               BindListArgument,
                                               nullptr,
                                               TokenPredicateLocalState::Init);
    loader.RegisterFunction(std::move(contains_any_tokens));
}

} // namespace logsearch::scalars

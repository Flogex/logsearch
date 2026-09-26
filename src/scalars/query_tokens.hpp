#pragma once

#include "analyzer/pipeline.hpp"

#include <algorithm>
#include <duckdb/common/allocator.hpp>
#include <duckdb/common/exception/binder_exception.hpp>
#include <duckdb/common/identifier.hpp>
#include <duckdb/common/typedefs.hpp>
#include <duckdb/common/types.hpp>
#include <duckdb/function/scalar_function.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace logsearch::scalars {

//! The document is argument 0 of every token predicate, the query argument is argument 1.
constexpr duckdb::idx_t QUERY_ARGUMENT = 1;

//! Builds the term set a query argument asks for. This lives only during bind-time.
//! The analyzed terms are retrieved at the end using TakeTerms.
// The class owns the analyzer the query tokens run through, so it is not copyable and does not belong in the bind data
// that outlives the bind call.
class QueryTokens final {
public:
    explicit QueryTokens(const duckdb::BindScalarFunctionInput& input)
        : pipeline_(duckdb::Allocator::Get(input.GetClientContext())),
          function_name_(input.GetBoundFunction().GetName()),
          argument_is_list_(input.GetBoundFunction().GetArguments()[QUERY_ARGUMENT].id() ==
                            duckdb::LogicalTypeId::LIST) {
    }

    //! Analyzes one query token and adds its term to the set, skipping a term already present.
    void AnalyzeAndInsert(const std::string_view token) {
        std::vector<std::string> analyzed = pipeline_.Run(token);

        if (analyzed.size() > 1) {
            // The scalar functions expect tokens. Something like contains_token(col, 'hello world') is multiple tokens.
            // We might want to change this for better ergonomics later.
            throw duckdb::BinderException(
                "%s only accepts a single token per argument, but '%s' analyzes to more than one. %s",
                function_name_,
                std::string(token),
                argument_is_list_ ? "Pass each token as its own list element."
                                  : "Use contains_all_tokens to require all of them.");
        }

        // A token that analyzes to nothing is kept as the empty term, which every document satisfies.
        // The analyzer never produces an empty term, so there is no chance of a real token trivially satisfying every
        // document.
        D_ASSERT(analyzed.empty() || !analyzed.front().empty());
        std::string term = analyzed.empty() ? std::string() : std::move(analyzed.front());

        // A query argument holds a handful of terms, so a linear scan beats building a hash set.
        if (std::find(terms_.begin(), terms_.end(), term) == terms_.end()) {
            terms_.push_back(std::move(term));
        }
    }

    //! The distinct terms, in first-seen order.
    [[nodiscard]] std::vector<std::string> TakeTerms() && {
        return std::move(terms_);
    }

private:
    analyzer::Pipeline pipeline_;
    duckdb::Identifier function_name_;
    bool argument_is_list_;
    std::vector<std::string> terms_;
};

} // namespace logsearch::scalars

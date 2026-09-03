// Pins down the DuckDB filter-pushdown behaviour the Logsearch optimizer relies on. Nothing here touches
// logsearch code and the extension is deliberately not loaded, so what is asserted is DuckDB's own plan shape. If
// an upgrade stops pushing a predicate into the scan, or changes how the filtered column is referenced, these
// tests fail instead of the rewrite silently doing nothing.
//
// The optimizer reads LogicalGet::table_filters and matches a filter on four properties: it is an
// ExpressionFilter, its top-level expression is a comparison / a known function / a conjunction, it has exactly
// two arguments, and its first argument is BoundReferenceExpression(0). Every test below pins one of those, or
// pins a shape that deliberately fails them.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <duckdb/common/column_index.hpp>
#include <duckdb/common/enum_util.hpp>
#include <duckdb/common/enums/expression_type.hpp>
#include <duckdb/common/optional_ptr.hpp>
#include <duckdb/common/typedefs.hpp>
#include <duckdb/main/client_context.hpp>
#include <duckdb/main/connection.hpp>
#include <duckdb/main/database.hpp>
#include <duckdb/main/query_result.hpp>
#include <duckdb/planner/expression.hpp>
#include <duckdb/planner/expression/bound_function_expression.hpp>
#include <duckdb/planner/expression/bound_reference_expression.hpp>
#include <duckdb/planner/filter/expression_filter.hpp>
#include <duckdb/planner/logical_operator.hpp>
#include <duckdb/planner/operator/logical_get.hpp>
#include <duckdb/planner/table_filter.hpp>
#include <string>

using Catch::Matchers::ContainsSubstring;
using duckdb::ExpressionClass;

namespace {

//! What DuckDB pushed into a table scan for one column.
struct PushedFilter {
    //! True if a filter for that column reached the scan at all.
    bool pushed = false;
    //! Rendered the way EXPLAIN shows it, which is also what the SQLLogicTests match on.
    std::string rendered;
    //! Filters in a plan being optimized are always ExpressionFilters; the Legacy* classes exist only for storage.
    bool is_expression_filter = false;
    //! Top-level expression of the filter, as enumerator names so a failure reports what it found.
    std::string expression_class;
    std::string expression_type;
    //! Function name when the top level is a bound function, empty otherwise.
    std::string function_name;
    //! Arguments of that function. The optimizer only matches two: the column and a constant.
    duckdb::idx_t argument_count = 0;
    //! True if the first argument is BoundReferenceExpression(0), i.e. the filtered column itself.
    bool column_is_first_reference = false;
};

//! The first LogicalGet in `op`, or nothing when the plan has none.
duckdb::optional_ptr<duckdb::LogicalGet> FindGet(duckdb::LogicalOperator& op) {
    if (op.type == duckdb::LogicalOperatorType::LOGICAL_GET) {
        return op.Cast<duckdb::LogicalGet>();
    }
    for (auto& child : op.children) {
        if (const duckdb::optional_ptr<duckdb::LogicalGet> get = FindGet(*child)) {
            return get;
        }
    }
    return nullptr;
}

class PushdownFixture {
public:
    PushdownFixture() {
        // Two VARCHAR columns, because some pushdown decisions only show for a predicate spanning both. A NULL and
        // a second distinct value keep statistics propagation from folding away the predicates below.
        Run("CREATE TABLE logs (id INTEGER, msg VARCHAR, note VARCHAR)");
        Run(R"(INSERT INTO logs VALUES
                 (1, 'alice bob carol dave', 'x red y'),
                 (2, 'zeta eta theta iota', 'x blue y'),
                 (3, NULL, NULL))");
    }

    void Run(const std::string& sql) {
        const auto result = con_.Query(sql);
        REQUIRE_FALSE(result->HasError());
    }

    //! What the scan in `query` ended up filtering `column` by.
    [[nodiscard]] PushedFilter PushedInQuery(const std::string& query, const std::string& column) const {
        const duckdb::unique_ptr<duckdb::LogicalOperator> plan = con_.context->ExtractPlan(query);
        REQUIRE(plan);
        const duckdb::optional_ptr<duckdb::LogicalGet> get = FindGet(*plan);
        REQUIRE(get);

        for (const auto& entry : get->table_filters) {
            const duckdb::ColumnIndex& column_index = get->GetColumnIndex(entry.GetIndex());
            if (get->GetColumnName(column_index).GetIdentifierName() != column) {
                continue;
            }
            return Describe(entry.Filter(), column);
        }
        return {};
    }

    [[nodiscard]] PushedFilter Pushed(const std::string& predicate, const std::string& column = "msg") const {
        return PushedInQuery("SELECT id FROM logs WHERE " + predicate, column);
    }

    //! Number of columns the scan in `query` carries a filter for.
    [[nodiscard]] duckdb::idx_t FilterCountInQuery(const std::string& query) const {
        const duckdb::unique_ptr<duckdb::LogicalOperator> plan = con_.context->ExtractPlan(query);
        REQUIRE(plan);
        const duckdb::optional_ptr<duckdb::LogicalGet> get = FindGet(*plan);
        REQUIRE(get);
        return get->table_filters.FilterCount();
    }

    [[nodiscard]] duckdb::idx_t FilterCount(const std::string& predicate) const {
        return FilterCountInQuery("SELECT id FROM logs WHERE " + predicate);
    }

private:
    static PushedFilter Describe(const duckdb::TableFilter& filter, const std::string& column) {
        PushedFilter result;
        result.pushed = true;
        result.is_expression_filter = filter.filter_type == duckdb::TableFilterType::EXPRESSION_FILTER;
        if (!result.is_expression_filter) {
            return result;
        }

        const auto& expression_filter = filter.Cast<duckdb::ExpressionFilter>();
        const duckdb::Expression& expr = *expression_filter.expr;
        result.rendered = expression_filter.ToString(column);
        // EnumUtil, not ExpressionTypeToString: it spells the enumerator the optimizer compares against
        // (COMPARE_EQUAL) rather than the display form (EQUAL).
        result.expression_class = duckdb::EnumUtil::ToString(expr.GetExpressionClass());
        result.expression_type = duckdb::EnumUtil::ToString(expr.GetExpressionType());
        if (expr.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
            return result;
        }

        const auto& function = expr.Cast<duckdb::BoundFunctionExpression>();
        result.function_name = function.Function().GetName().GetIdentifierName();
        result.argument_count = function.GetChildren().size();
        if (result.argument_count > 0) {
            const duckdb::Expression& first = *function.GetChildren()[0];
            result.column_is_first_reference = first.GetExpressionClass() == ExpressionClass::BOUND_REF &&
                                               first.Cast<duckdb::BoundReferenceExpression>().Index() == 0;
        }
        return result;
    }

    duckdb::DuckDB db_{nullptr};
    duckdb::Connection con_{db_};
};

} // namespace

/***** The conventions the whole matcher rests on *****/

TEST_CASE_METHOD(PushdownFixture, "A pushed filter references its column as the first bound reference",
                 "[optimizer][assumptions]") {
    // A table filter is evaluated on a chunk holding only the filtered column, so the column is reference 0. The
    // optimizer builds its own rowid filter the same way, so this convention has to hold in both directions.
    const PushedFilter filter = Pushed("contains(msg, 'alice bob carol')");
    REQUIRE(filter.pushed);
    CHECK(filter.column_is_first_reference);
    CHECK(filter.argument_count == 2);
}

TEST_CASE_METHOD(PushdownFixture, "Pushed filters are ExpressionFilters, never a legacy filter class",
                 "[optimizer][assumptions]") {
    // Since the TableFilter unification the Legacy* classes only appear at (de)serialization time.
    for (const char* predicate : {"msg = 'alice bob carol dave'",
                                  "contains(msg, 'alice bob carol')",
                                  "msg IS NOT NULL",
                                  "contains(msg, 'a b c') AND contains(msg, 'd e f')"}) {
        const PushedFilter filter = Pushed(predicate);
        REQUIRE(filter.pushed);
        CHECK(filter.is_expression_filter);
    }
}

/***** Predicate shapes the optimizer matches *****/

TEST_CASE_METHOD(PushdownFixture, "Equality is pushed down as a comparison function", "[optimizer][assumptions]") {
    // Comparisons are BoundFunctionExpressions too, which is why the optimizer dispatches on the expression *type*
    // to tell `col = 'x'` apart from a call to a scalar function.
    const PushedFilter filter = Pushed("msg = 'alice bob carol dave'");
    REQUIRE(filter.pushed);
    CHECK(filter.expression_class == "BOUND_FUNCTION");
    CHECK(filter.expression_type == "COMPARE_EQUAL");
    CHECK(filter.argument_count == 2);
    CHECK(filter.column_is_first_reference);
    CHECK_THAT(filter.rendered, ContainsSubstring("msg = 'alice bob carol dave'"));
}

TEST_CASE_METHOD(PushdownFixture, "contains() is pushed down under that name", "[optimizer][assumptions]") {
    const PushedFilter filter = Pushed("contains(msg, 'alice bob carol')");
    REQUIRE(filter.pushed);
    CHECK(filter.expression_class == "BOUND_FUNCTION");
    CHECK(filter.function_name == "contains");
}

TEST_CASE_METHOD(PushdownFixture, "suffix() is pushed down under that name", "[optimizer][assumptions]") {
    const PushedFilter filter = Pushed("suffix(msg, 'carol dave')");
    REQUIRE(filter.pushed);
    CHECK(filter.function_name == "suffix");
    CHECK(filter.column_is_first_reference);
}

TEST_CASE_METHOD(PushdownFixture, "LIKE with both wildcards becomes contains()", "[optimizer][assumptions]") {
    // The optimizer answers LIKE '%needle%' only because DuckDB rewrites it before we see it.
    const PushedFilter filter = Pushed("msg LIKE '%alice bob%'");
    REQUIRE(filter.pushed);
    CHECK(filter.function_name == "contains");
}

TEST_CASE_METHOD(PushdownFixture, "LIKE with a leading wildcard becomes suffix()", "[optimizer][assumptions]") {
    const PushedFilter filter = Pushed("msg LIKE '%carol dave'");
    REQUIRE(filter.pushed);
    CHECK(filter.function_name == "suffix");
}

TEST_CASE_METHOD(PushdownFixture, "LIKE with a trailing wildcard becomes a range, not a prefix call",
                 "[optimizer][assumptions]") {
    // This is why prefix predicates are unsupported: there is no `prefix` call to match, only a range over the
    // pattern's constant part.
    const PushedFilter filter = Pushed("msg LIKE 'alice bob%'");
    REQUIRE(filter.pushed);
    CHECK(filter.expression_class == "BOUND_CONJUNCTION");
    CHECK(filter.expression_type == "CONJUNCTION_AND");
    CHECK(filter.function_name.empty());
    CHECK_THAT(filter.rendered, ContainsSubstring(">="));
    CHECK_THAT(filter.rendered, ContainsSubstring("<"));
}

TEST_CASE_METHOD(PushdownFixture, "Two filters on one column arrive as a single conjunction",
                 "[optimizer][assumptions]") {
    // PushFilter ANDs same-column filters, so the matcher has to walk a conjunction rather than expect one
    // predicate per column.
    const std::string predicate = "contains(msg, 'a b c') AND contains(msg, 'd e f')";
    const PushedFilter filter = Pushed(predicate);
    REQUIRE(filter.pushed);
    CHECK(filter.expression_class == "BOUND_CONJUNCTION");
    CHECK(filter.expression_type == "CONJUNCTION_AND");
    CHECK(FilterCount(predicate) == 1);
}

TEST_CASE_METHOD(PushdownFixture, "A disjunction on one column is pushed down whole", "[optimizer][assumptions]") {
    // Whole *and unwrapped*, which is what lets the matcher walk into the conjunction and find the predicates
    // below it. Were this wrapped the way the comparison disjunction in the next test is, the top level would be
    // a bound function whose name is not one of the token predicates, and the rewrite would quietly stop firing.
    const PushedFilter filter = Pushed("contains(msg, 'a b c') OR contains(msg, 'd e f')");
    REQUIRE(filter.pushed);
    CHECK(filter.expression_class == "BOUND_CONJUNCTION");
    CHECK(filter.expression_type == "CONJUNCTION_OR");
    CHECK(filter.function_name.empty());
}

TEST_CASE_METHOD(PushdownFixture, "A disjunction of comparisons is pushed as a prune-only optional filter",
                 "[optimizer][assumptions]") {
    // Not every same-column disjunction reaches the scan in the same shape. This one is wrapped in
    // __internal_tablefilter_optional, a filter that exists only to be asked at row-group level and is then
    // dropped for the rest of that group (RowGroup::CheckZonemap -> SetFilterAlwaysTrue), with the real predicate
    // left behind in a LogicalFilter above the scan. Pinned as the boundary of the test above: if DuckDB ever
    // extended that wrapping to function calls, the matcher would see this name instead of a token predicate.
    const PushedFilter filter = Pushed("msg = 'alice bob carol dave' OR msg = 'zeta eta theta iota'");
    REQUIRE(filter.pushed);
    CHECK(filter.expression_class == "BOUND_FUNCTION");
    CHECK(filter.function_name == "__internal_tablefilter_optional");
    CHECK(filter.argument_count == 1);
}

TEST_CASE_METHOD(PushdownFixture, "A conjunction over two columns is split per column", "[optimizer][assumptions]") {
    const std::string predicate = "contains(msg, 'a b c') AND contains(note, 'x y z')";
    CHECK(FilterCount(predicate) == 2);
    CHECK(Pushed(predicate, "msg").function_name == "contains");
    CHECK(Pushed(predicate, "note").function_name == "contains");
}

TEST_CASE_METHOD(PushdownFixture, "A filter survives a renaming subquery", "[optimizer][assumptions]") {
    // Pushdown goes through projections, so a predicate written against an alias still reaches the scan.
    CHECK(FilterCountInQuery("SELECT id FROM (SELECT id, msg AS m FROM logs) WHERE contains(m, 'a b c')") == 1);
}

/***** Shapes that never reach the scan, and are therefore invisible to the optimizer *****/

TEST_CASE_METHOD(PushdownFixture, "A disjunction across two columns is not pushed down", "[optimizer][assumptions]") {
    // A TableFilterSet is per column and cannot express an OR spanning two of them, so this stays in a
    // LogicalFilter above the scan. It is the one shape the optimizer could answer but never sees.
    const std::string predicate = "contains(msg, 'a b c') OR contains(note, 'x y z')";
    CHECK(FilterCount(predicate) == 0);
    CHECK_FALSE(Pushed(predicate, "msg").pushed);
    CHECK_FALSE(Pushed(predicate, "note").pushed);
}

TEST_CASE_METHOD(PushdownFixture, "A predicate over two columns is not pushed down", "[optimizer][assumptions]") {
    CHECK(FilterCount("contains(msg, note)") == 0);
}

TEST_CASE_METHOD(PushdownFixture, "A volatile argument is not pushed down", "[optimizer][assumptions]") {
    // The optimizer needs a constant literal anyway; this pins that such a predicate never even arrives.
    CHECK(FilterCount("contains(msg, CAST(random() AS VARCHAR))") == 0);
}

/***** Why the optimizer has to look above the scan as well *****/

TEST_CASE_METHOD(PushdownFixture, "A filter that can throw is only pushed down when it is the only one",
                 "[optimizer][assumptions]") {
    // Pushing a throwing predicate into a scan loses the short-circuit evaluation order it had in the
    // LogicalFilter, so DuckDB does it only when there is nothing to short-circuit against
    // (`expr.CanThrow() && filters.size() > 1` in pushdown_get.cpp).
    //
    // The token predicates are deliberately not marked fallible, so this rule does not currently apply to them
    // and a conjunction of them reaches the scan. That is what lets the optimizer read only
    // LogicalGet::table_filters. It is pinned here, with a cast standing in for a throwing predicate, because
    // making one of them fallible again would move a conjunction back above the scan and silently stop it being
    // rewritten.
    CHECK(FilterCount("CAST(msg AS INTEGER) = 1") == 1);
    CHECK(FilterCount("CAST(msg AS INTEGER) = 1 AND CAST(note AS INTEGER) = 2") == 0);
}

TEST_CASE_METHOD(PushdownFixture, "No filter is pushed into a scan that samples", "[optimizer][assumptions]") {
    // A percentage sample lives inside the scan (LogicalGet::extra_info.sample_options) and draws from the rows
    // the scan reads, so a filter below it would sample a different population. DuckDB never pushes one -- not
    // even a plain comparison. The optimizer relies on that: it injects a rowid filter directly, which would
    // break the same rule, so CanOptimize declines a sampled get. Should this stop holding, that guard becomes
    // load-bearing rather than belt-and-braces.
    CHECK(FilterCountInQuery("SELECT id FROM logs USING SAMPLE 50% WHERE id = 1") == 0);
    CHECK(FilterCountInQuery("SELECT id FROM logs USING SAMPLE 50% WHERE contains(msg, 'a b c')") == 0);
}

TEST_CASE_METHOD(PushdownFixture, "No filter is pushed into a scan that produces a row number",
                 "[optimizer][assumptions]") {
    // `row_number() OVER ()` is folded into a virtual column of the scan, whose value is the position of the row
    // in what the scan emits. Narrowing the scan renumbers it, so DuckDB pushes no filter into such a get --
    // again, not even a comparison. Same standing as the sample above: it is why the optimizer's virtual-column
    // guard is currently unreachable.
    CHECK(FilterCountInQuery("SELECT id FROM (SELECT id, msg, row_number() OVER () rn FROM logs) WHERE id = 1") == 0);
    CHECK(FilterCountInQuery(
              "SELECT id FROM (SELECT id, msg, row_number() OVER () rn FROM logs) WHERE contains(msg, 'a b c')") == 0);

    // Only the subquery form folds. At the same query level `WHERE` runs before the window, so DuckDB leaves a
    // real Window operator above the scan, pushes the filter down as usual, and puts no virtual column in the
    // scan's column list. That shape needs no guard: the Window numbers rows the scan has already filtered, so
    // narrowing the scan removes only rows the residual predicate would have removed anyway.
    CHECK(FilterCountInQuery("SELECT id, row_number() OVER () rn FROM logs WHERE id = 1") == 1);
    CHECK(FilterCountInQuery("SELECT id, row_number() OVER () rn FROM logs WHERE contains(msg, 'a b c')") == 1);
}

/***** Shapes that are pushed down, but not in a form the matcher accepts *****/

TEST_CASE_METHOD(PushdownFixture, "An IN list arrives as a single-argument optional filter",
                 "[optimizer][assumptions]") {
    // An IN list at or above InClauseRewriter's threshold is answered by a mark join, and the scan only gets an
    // `optional` wrapper for zonemap pruning. Its single argument is what makes the matcher skip it, and it is why
    // row-ID lists must never be handed to DuckDB as `rowid IN (...)`.
    const PushedFilter filter = Pushed("msg IN ('alice bob carol dave', 'b', 'c', 'd', 'e', 'f')");
    REQUIRE(filter.pushed);
    CHECK(filter.function_name == "__internal_tablefilter_optional");
    CHECK(filter.argument_count == 1);
}

TEST_CASE_METHOD(PushdownFixture, "A wrapped column is pushed down but is not a bare reference",
                 "[optimizer][assumptions]") {
    // contains(lower(msg), ...) reaches the scan, yet its first argument is a function rather than the column, so
    // the matcher skips it. Accepting index-preserving wrappers is a TODO, and this is the case that changes.
    const PushedFilter filter = Pushed("contains(lower(msg), 'a b c')");
    REQUIRE(filter.pushed);
    CHECK(filter.function_name == "contains");
    CHECK(filter.argument_count == 2);
    CHECK_FALSE(filter.column_is_first_reference);
}

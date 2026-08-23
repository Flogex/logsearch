#include "analyzer/pipeline.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_vector.hpp>
#include <duckdb/common/allocator.hpp>
#include <string>
#include <vector>

using Catch::Matchers::Equals;
using logsearch::analyzer::Pipeline;

// End-to-end tests for the analyzer Pipeline. These deliberately do NOT
// re-test individual stage behavior (lowercase rules, whitespace handling,
// etc.) — those have focused tests per stage. The
// goal here is to verify that the stages are wired together correctly and
// that the chain produces sensible output on representative real input.

namespace {

std::vector<std::string> RunPipeline(std::string_view document) {
    Pipeline pipeline(duckdb::Allocator::DefaultAllocator());
    return pipeline.Run(document);
}

} // namespace

TEST_CASE("Pipeline returns no terms for empty input", "[analyzer][pipeline]") {
    REQUIRE(RunPipeline("").empty());
}

TEST_CASE("Pipeline analyzes a realistic web access log line", "[analyzer][pipeline]") {
    // Exercises every wired stage in one go:
    //  - mixed case   -> Lowercaser
    //  - tabs + multi-space + leading whitespace -> Tokenizer
    //  - punctuation, paths, status codes -> kept attached to surrounding tokens
    //  - TermCollector collects the terms into a vector
    const std::string input = "  GET /api/Users\tHTTP/1.1  200   the  request to /api/users  was OK";
    REQUIRE_THAT(RunPipeline(input),
                 Equals(std::vector<std::string>{
                     "get", "/api/users", "http/1.1", "200", "the", "request", "to", "/api/users", "was", "ok"}));
}

TEST_CASE("Pipeline analyzes a realistic exception log line", "[analyzer][pipeline]") {
    // Different shape than the web log: long words and no slashes.
    const std::string input = "ERROR NullPointerException at com.example.Service.handle(Service.java:42) "
                              "could not be resolved by the runtime";
    REQUIRE_THAT(RunPipeline(input),
                 Equals(std::vector<std::string>{
                     "error",
                     "nullpointerexception",
                     "at",
                     "com.example.service.handle(service.java:42)",
                     "could",
                     "not",
                     "be",
                     "resolved",
                     "by",
                     "the",
                     "runtime",
                 }));
}

TEST_CASE("Previous runs of the same Pipeline do not affect the current run", "[analyzer][pipeline]") {
    Pipeline pipeline(duckdb::Allocator::DefaultAllocator());

    REQUIRE_THAT(pipeline.Run("alpha beta gamma delta epsilon"),
                 Equals(std::vector<std::string>{"alpha", "beta", "gamma", "delta", "epsilon"}));
    REQUIRE_THAT(pipeline.Run("zeta"), Equals(std::vector<std::string>{"zeta"}));
    REQUIRE(pipeline.Run("").empty());
    REQUIRE_THAT(pipeline.Run("eta theta"), Equals(std::vector<std::string>{"eta", "theta"}));
}

TEST_CASE("Pipeline analyzes documents larger than the initial arena chunk", "[analyzer][pipeline]") {
    // The arena starts out with a 2 KB chunk, so a longer document forces a fresh one.
    Pipeline pipeline(duckdb::Allocator::DefaultAllocator());
    const std::string long_term(4000, 'x');

    REQUIRE_THAT(pipeline.Run(long_term), Equals(std::vector<std::string>{long_term}));
    REQUIRE_THAT(pipeline.Run("short"), Equals(std::vector<std::string>{"short"}));
    REQUIRE_THAT(pipeline.Run(long_term + " tail"), Equals(std::vector<std::string>{long_term, "tail"}));
}

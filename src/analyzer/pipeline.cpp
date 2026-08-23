#ifndef ANALYZER_UNITY_BUILD
#error "pipeline.cpp must be compiled as part of the Analyzer unity build"
#endif

#include "pipeline.hpp"

#include "lowercaser.hpp"
#include "mutable_span.hpp"
#include "string_predicates.hpp"
#include "tokenizer.hpp"

#include <cstring>
#include <duckdb/common/allocator.hpp>
#include <duckdb/common/exception.hpp>
#include <duckdb/storage/arena_allocator.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace logsearch::analyzer {

namespace {

//! Terminal stage: collects surviving terms into a vector. Placeholder until
//! a real TermSink lands.
class TermCollector {
    std::vector<std::string>& output_;

public:
    explicit TermCollector(std::vector<std::string>& output) noexcept : output_(output) {
    }

    void PushToken(MutableSpan term) {
        output_.emplace_back(term.data(), term.size());
    }
};

//! ASCII fast path. Stage order:
//! 1. Lowercaser (ASCII case fold, in place)
//! 2. Tokenizer (whitespace split)
//! 3. TermCollector
// cppcheck-suppress passedByValue ; MutableSpan is small enough to be copied
void RunAsciiPipeline(MutableSpan doc, std::vector<std::string>& output) {
    // The analyzer chain that is fixed at compile-time. We don't have the flexibility to change the stage order at
    // runtime. But there are no virtual function calls and the compiler can optimize the whole pipeline.
    TermCollector collector(output);
    Tokenizer<TermCollector> tokenizer(collector);
    Lowercaser<Tokenizer<TermCollector>> chain(tokenizer);
    chain.ProcessDocument(doc);
}

[[noreturn]] void RunUnicodePipeline(MutableSpan /*doc*/, std::vector<std::string>& /*output*/) {
    throw duckdb::NotImplementedException("Analyzer pipeline does not yet support non-ASCII input");
}

} // namespace

Pipeline::Pipeline(duckdb::Allocator& allocator) : arena_(allocator) {
}

std::vector<std::string> Pipeline::Run(const std::string_view document) {
    std::vector<std::string> output;

    // Every stage pushes its tokens downstream before Run returns and the terms are collected as owning strings, so
    // nothing points into the arena between calls. Resetting here keeps reduces the memory footprint because only
    // a single document is processed.
    // In the future, we will probably get some kind of `Collect` function that returns an iterator and, when done,
    // resets the arena.
    arena_.Reset();

    // Take ownership of the string to allow in-place modifications.
    // Previous owner is some DuckDB operator, and we cannot be sure that the string is not used for anything else (e.g.
    // updating a different inex).
    auto* buffer = reinterpret_cast<char*>(arena_.Allocate(document.size()));
    std::memcpy(buffer, document.data(), document.size());
    const MutableSpan span(buffer, document.size());

    // Expectation is that large majority of documents are ASCII-only
    if (IsAscii(document)) {
        RunAsciiPipeline(span, output);
    } else {
        RunUnicodePipeline(span, output);
    }
    return output;
}

} // namespace logsearch::analyzer

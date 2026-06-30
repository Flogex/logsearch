#include "analyzer/tokenizer.hpp"

#include "analyzer/mutable_span.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_vector.hpp>
#include <string>
#include <vector>

using Catch::Matchers::Equals;
using logsearch::analyzer::MutableSpan;
using logsearch::analyzer::Tokenizer;

namespace {

struct CaptureSink {
    std::vector<std::string> tokens; // owned copies for content comparison
    std::vector<MutableSpan> spans;  // the spans themselves for pointer / capacity checks

    void PushToken(MutableSpan token) {
        tokens.emplace_back(token.data(), token.size());
        spans.push_back(token);
    }
};

void RunTokenizer(CaptureSink& sink, std::string& doc) {
    Tokenizer<CaptureSink> tokenizer(sink);
    tokenizer.ProcessDocument(MutableSpan(doc));
}

} // namespace

TEST_CASE("Tokenizer emits no tokens for an empty document", "[analyzer][tokenizer]") {
    CaptureSink sink;
    std::string doc;
    RunTokenizer(sink, doc);
    REQUIRE(sink.tokens.empty());
}

TEST_CASE("Tokenizer emits no tokens for an all-whitespace document", "[analyzer][tokenizer]") {
    CaptureSink sink;
    std::string doc = "   \t\n\r\f\v   ";
    RunTokenizer(sink, doc);
    REQUIRE(sink.tokens.empty());
}

TEST_CASE("Tokenizer splits on single ASCII space", "[analyzer][tokenizer]") {
    CaptureSink sink;
    std::string doc = "the quick brown fox";
    RunTokenizer(sink, doc);
    REQUIRE_THAT(sink.tokens, Equals(std::vector<std::string>{"the", "quick", "brown", "fox"}));
}

TEST_CASE("Tokenizer collapses runs of whitespace", "[analyzer][tokenizer]") {
    CaptureSink sink;
    std::string doc = "the    quick\t\tbrown\n\nfox";
    RunTokenizer(sink, doc);
    REQUIRE_THAT(sink.tokens, Equals(std::vector<std::string>{"the", "quick", "brown", "fox"}));
}

TEST_CASE("Tokenizer handles all whitespace classes (space tab nl cr ff vt)", "[analyzer][tokenizer]") {
    CaptureSink sink;
    std::string doc = "a\tb\nc\rd\fe\vf";
    RunTokenizer(sink, doc);
    REQUIRE_THAT(sink.tokens, Equals(std::vector<std::string>{"a", "b", "c", "d", "e", "f"}));
}

TEST_CASE("Tokenizer trims leading whitespace", "[analyzer][tokenizer]") {
    CaptureSink sink;
    std::string doc = "   hello world";
    RunTokenizer(sink, doc);
    REQUIRE_THAT(sink.tokens, Equals(std::vector<std::string>{"hello", "world"}));
}

TEST_CASE("Tokenizer trims trailing whitespace", "[analyzer][tokenizer]") {
    CaptureSink sink;
    std::string doc = "hello world   ";
    RunTokenizer(sink, doc);
    REQUIRE_THAT(sink.tokens, Equals(std::vector<std::string>{"hello", "world"}));
}

TEST_CASE("Tokenizer emits a single token for a single-word document", "[analyzer][tokenizer]") {
    CaptureSink sink;
    std::string doc = "logarithm";
    RunTokenizer(sink, doc);
    REQUIRE_THAT(sink.tokens, Equals(std::vector<std::string>{"logarithm"}));
}

TEST_CASE("Tokenizer emits a single token for whitespace-padded single word", "[analyzer][tokenizer]") {
    CaptureSink sink;
    std::string doc = "  \t logarithm \n ";
    RunTokenizer(sink, doc);
    REQUIRE_THAT(sink.tokens, Equals(std::vector<std::string>{"logarithm"}));
}

TEST_CASE("Tokenizer treats punctuation as part of tokens (no split)", "[analyzer][tokenizer]") {
    // Tokenizer only splits on whitespace; `error,401` stays one token.
    // (Future punctuation-aware tokenizer would change this.)
    CaptureSink sink;
    std::string doc = "error,401 path=/foo";
    RunTokenizer(sink, doc);
    REQUIRE_THAT(sink.tokens, Equals(std::vector<std::string>{"error,401", "path=/foo"}));
}

TEST_CASE("Tokenizer emits tokens that alias the input buffer", "[analyzer][tokenizer]") {
    CaptureSink sink;
    std::string doc = "alpha beta gamma";
    RunTokenizer(sink, doc);
    REQUIRE(sink.spans.size() == 3);
    CHECK(sink.spans[0].data() == doc.data() + 0);  // "alpha"
    CHECK(sink.spans[1].data() == doc.data() + 6);  // "beta"
    CHECK(sink.spans[2].data() == doc.data() + 11); // "gamma"
}

TEST_CASE("Tokenizer handles many tokens", "[analyzer][tokenizer]") {
    CaptureSink sink;
    // 100 single-letter tokens separated by single spaces.
    std::string doc;
    for (int i = 0; i < 100; ++i) {
        if (i > 0) {
            doc.push_back(' ');
        }
        doc.push_back('a');
    }
    RunTokenizer(sink, doc);
    REQUIRE(sink.tokens.size() == 100);
    REQUIRE(sink.tokens.front() == "a");
    REQUIRE(sink.tokens.back() == "a");
}

TEST_CASE("Tokenizer produces correct number of tokens from Lorem Ipsum paragraph", "[analyzer][tokenizer]") {
    CaptureSink sink;
    // Classic Lorem Ipsum first paragraph. Whitespace-split word count: 69.
    std::string doc = "lorem ipsum dolor sit amet, consectetur adipiscing elit, "
                      "sed do eiusmod tempor incididunt ut labore et dolore magna aliqua. "
                      "ut enim ad minim veniam, quis nostrud exercitation ullamco laboris "
                      "nisi ut aliquip ex ea commodo consequat. duis aute irure dolor in "
                      "reprehenderit in voluptate velit esse cillum dolore eu fugiat nulla "
                      "pariatur. excepteur sint occaecat cupidatat non proident, sunt in "
                      "culpa qui officia deserunt mollit anim id est laborum.";
    RunTokenizer(sink, doc);
    REQUIRE(sink.tokens.size() == 69);
}

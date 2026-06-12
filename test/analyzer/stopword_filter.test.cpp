#include "analyzer/stopword_filter.hpp"

#include "analyzer/mutable_span.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_vector.hpp>
#include <string>
#include <vector>

using Catch::Matchers::Equals;
using logsearch::analyzer::MutableSpan;
using logsearch::analyzer::StopwordFilter;

namespace {

struct CaptureSink {
    std::vector<std::string> forwarded;
    void PushToken(MutableSpan token) {
        forwarded.emplace_back(token.data(), token.size());
    }
};

} // namespace

TEST_CASE("StopwordFilter drops known stopwords", "[analyzer][stopword]") {
    CaptureSink sink;
    StopwordFilter<CaptureSink> filter(sink);

    std::array<std::string, 10> stopwords = {"a", "i", "an", "of", "to", "by", "the", "and", "you", "that"};
    for (std::string& sw : stopwords) {
        filter.PushToken(MutableSpan(sw));
    }

    CAPTURE(sink.forwarded);
    REQUIRE(sink.forwarded.empty());
}

TEST_CASE("StopwordFilter forwards non-stopwords", "[analyzer][stopword]") {
    CaptureSink sink;
    StopwordFilter<CaptureSink> filter(sink);

    std::array<std::string, 3> non_stopwords = {"log", "error", "zt"};
    for (std::string& nsw : non_stopwords) {
        filter.PushToken(MutableSpan(nsw));
    }

    REQUIRE_THAT(sink.forwarded, Equals(std::vector<std::string>{"log", "error", "zt"}));
}

TEST_CASE("StopwordFilter forwards tokens longer than 8 bytes", "[analyzer][stopword]") {
    CaptureSink sink;
    StopwordFilter<CaptureSink> filter(sink);

    std::array<std::string, 2> long_tokens = {"logarithm", "exception"};
    for (std::string& lt : long_tokens) {
        filter.PushToken(MutableSpan(lt));
    }

    REQUIRE_THAT(sink.forwarded, Equals(std::vector<std::string>{"logarithm", "exception"}));
}

TEST_CASE("StopwordFilter distinguishes stopword prefixes from full stopwords", "[analyzer][stopword]") {
    // "the" is a stopword; "theme" / "they" / "then" share its prefix but
    // pack to different uint64 values.
    CaptureSink sink;
    StopwordFilter<CaptureSink> filter(sink);

    std::array<std::string, 3> non_stopwords = {"theme", "they", "then"};
    for (std::string& nsw : non_stopwords) {
        filter.PushToken(MutableSpan(nsw));
    }

    REQUIRE_THAT(sink.forwarded, Equals(std::vector<std::string>{"theme", "they", "then"}));
}

TEST_CASE("StopwordFilter forwards mixed stream, dropping only stopwords", "[analyzer][stopword]") {
    CaptureSink sink;
    StopwordFilter<CaptureSink> filter(sink);

    std::array<std::string, 4> mixed_tokens = {"the", "error", "and", "log"};
    for (std::string& token : mixed_tokens) {
        filter.PushToken(MutableSpan(token));
    }

    REQUIRE_THAT(sink.forwarded, Equals(std::vector<std::string>{"error", "log"}));
}

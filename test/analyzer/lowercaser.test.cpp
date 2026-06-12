#include "analyzer/lowercaser.hpp"

#include "analyzer/mutable_span.hpp"
#include "analyzer/stopword_filter.hpp"

#include <catch2/catch_test_macros.hpp>
#include <string>

using logsearch::analyzer::Lowercaser;
using logsearch::analyzer::MutableSpan;

namespace {

struct CaptureSink {
    std::string captured;
    void ProcessDocument(MutableSpan doc) {
        captured.assign(doc.data(), doc.size());
    }
};

void RunLowercaser(std::string& doc, CaptureSink& sink) {
    Lowercaser<CaptureSink> lowercaser(sink);
    lowercaser.ProcessDocument(MutableSpan(doc));
}

} // namespace

TEST_CASE("Lowercaser maps ASCII uppercase to lowercase", "[analyzer][lowercaser]") {
    CaptureSink sink;
    std::string doc = "Hello WORLD";
    RunLowercaser(doc, sink);
    REQUIRE(sink.captured == "hello world");
}

TEST_CASE("Lowercaser forwards an empty string", "[analyzer][lowercaser]") {
    CaptureSink sink;
    sink.captured = "sentinel";
    std::string doc;
    RunLowercaser(doc, sink);
    REQUIRE(sink.captured.empty());
}

TEST_CASE("Lowercaser preserves non-letter ASCII bytes", "[analyzer][lowercaser]") {
    CaptureSink sink;
    std::string doc = "Hello, World! 123 @[`{";
    RunLowercaser(doc, sink);
    REQUIRE(sink.captured == "hello, world! 123 @[`{");
}

TEST_CASE("Lowercaser leaves UTF-8 high bytes untouched", "[analyzer][lowercaser]") {
    CaptureSink sink;
    std::string doc = "H\xC3\xA9llo";
    RunLowercaser(doc, sink);
    REQUIRE(sink.captured == "h\xC3\xA9llo");
}

TEST_CASE("Lowercaser preserves embedded null bytes", "[analyzer][lowercaser]") {
    CaptureSink sink;
    std::string doc("A\0B\0C", 5);
    RunLowercaser(doc, sink);
    REQUIRE(sink.captured == std::string("a\0b\0c", 5));
}

TEST_CASE("Lowercaser maps the full A-Z alphabet", "[analyzer][lowercaser]") {
    CaptureSink sink;
    std::string doc = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    RunLowercaser(doc, sink);
    REQUIRE(sink.captured == "abcdefghijklmnopqrstuvwxyz");
}

TEST_CASE("Lowercaser handles length not divisible by SIMD vector width", "[analyzer][lowercaser]") {
    CaptureSink sink;
    std::string doc(23, 'A');
    RunLowercaser(doc, sink);
    REQUIRE(sink.captured == std::string(23, 'a'));
}

TEST_CASE("Lowercaser handles long input", "[analyzer][lowercaser]") {
    CaptureSink sink;
    std::string doc(1024, 'A');
    RunLowercaser(doc, sink);
    REQUIRE(sink.captured == std::string(1024, 'a'));
}

TEST_CASE("Lowercaser mutates the input buffer in place", "[analyzer][lowercaser]") {
    CaptureSink sink;
    std::string doc(1024, 'A');
    const auto* const buffer_before = doc.data();
    RunLowercaser(doc, sink);
    REQUIRE(doc.data() == buffer_before);
    REQUIRE(doc == std::string(1024, 'a'));
}

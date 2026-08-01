#pragma once

#include <duckdb/storage/arena_allocator.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace duckdb {
class Allocator;
} // namespace duckdb

namespace logsearch::analyzer {

//! Analyzer pipeline that produces normalized terms from a document to be stored in the inverted index.
//! Not thread-safe: Every thread should instantiate its own Pipeline.
class Pipeline final {
public:
    //! `allocator` backs the ArenaAllocator that each document is copied into.
    explicit Pipeline(duckdb::Allocator& allocator);

    //! Analyze `document` end to end and return the surviving normalized terms.
    [[nodiscard]] std::vector<std::string> Run(std::string_view document);

private:
    duckdb::ArenaAllocator arena_;
};

} // namespace logsearch::analyzer

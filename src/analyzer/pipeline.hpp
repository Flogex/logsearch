#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace duckdb {
class ArenaAllocator;
} // namespace duckdb

namespace logsearch::analyzer {

///! Analyzer pipeline that produces normalized terms from a document to be stored in the inverted index.
class Pipeline final {
public:
    explicit Pipeline(duckdb::ArenaAllocator& arena) noexcept;

    //! Analyze `document` end to end and return the surviving normalized terms.
    [[nodiscard]] std::vector<std::string> Run(std::string_view document) const;

private:
    duckdb::ArenaAllocator& arena_;
};

} // namespace logsearch::analyzer

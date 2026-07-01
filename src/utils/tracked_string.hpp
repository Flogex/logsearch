#pragma once

#include "stl_allocator.hpp"

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace logsearch::utils {

//! A std::string whose heap buffer is allocated through a duckdb::Allocator.
using TrackedString = std::basic_string<char, std::char_traits<char>, StlAllocator<char>>;

//! Hash for TrackedString.
// std::hash is only specialized for the default-allocator std::string, so a custom-allocator
// basic_string needs its own.
struct TrackedStringHash {
    std::size_t operator()(const TrackedString& s) const noexcept {
        return std::hash<std::string_view>{}(std::string_view(s.data(), s.size()));
    }
};

//! A std::unordered_map whose nodes, bucket array, and string keys are all allocated through a duckdb::Allocator.
template <class V>
using TrackedStringMap = std::unordered_map<TrackedString, V, TrackedStringHash, std::equal_to<TrackedString>,
                                            StlAllocator<std::pair<const TrackedString, V>>>;

} // namespace logsearch::utils

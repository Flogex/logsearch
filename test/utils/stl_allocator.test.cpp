#include "utils/stl_allocator.hpp"

#include "utils/tracked_string.hpp"

#include <catch2/catch_test_macros.hpp>
#include <duckdb/common/allocator.hpp>
#include <string_view>
#include <vector>

using logsearch::utils::StlAllocator;
using logsearch::utils::TrackedString;
using logsearch::utils::TrackedStringHash;
using logsearch::utils::TrackedStringMap;

namespace {
// No DatabaseInstance in unit tests, so use the global default allocator (production passes Allocator::Get(db)).
duckdb::Allocator& TestAllocator() {
    return duckdb::Allocator::DefaultAllocator();
}

TrackedString MakeKey(const char* str) {
    return TrackedString(str, StlAllocator<char>(TestAllocator()));
}
} // namespace

TEST_CASE("StlAllocator backs a std::vector through reallocations", "[stl_allocator]") {
    const StlAllocator<int> allocator(TestAllocator());
    std::vector<int, StlAllocator<int>> values(allocator);
    for (int i = 0; i < 1000; i++) {
        values.push_back(i); // repeated growth exercises allocate() + deallocate() of the old buffer
    }

    REQUIRE(values.size() == 1000);
    CHECK(values.front() == 0);
    CHECK(values.back() == 999);
}

TEST_CASE("StlAllocator backs a heap-allocated std::string", "[stl_allocator]") {
    const StlAllocator<char> allocator(TestAllocator());
    TrackedString str(allocator);
    // Longer than SSO, so the buffer is heap-allocated through the duckdb::Allocator.
    str = "donaudampfschifffahrtsgesellschaftskapitaen";

    CHECK(str.size() == 43);
    CHECK(std::string_view(str.data(), str.size()) == "donaudampfschifffahrtsgesellschaftskapitaen");
}

TEST_CASE("StlAllocator backs an unordered_map with tracked keys", "[stl_allocator]") {
    StlAllocator<std::pair<const TrackedString, int>> node_allocator(TestAllocator());
    TrackedStringMap<int> map(8, TrackedStringHash{}, std::equal_to<TrackedString>{}, node_allocator);

    map.emplace(MakeKey("alpha"), 1);
    map.emplace(MakeKey("beta"), 2);
    map.emplace(MakeKey("alpha"), 3); // duplicate key: not inserted

    CHECK(map.size() == 2);
    CHECK(map.find(MakeKey("alpha"))->second == 1);
    CHECK(map.find(MakeKey("beta"))->second == 2);
    CHECK(map.find(MakeKey("gamma")) == map.end());
}

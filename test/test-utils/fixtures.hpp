#pragma once

#include <duckdb/common/query_context.hpp>
#include <duckdb/main/database.hpp>
#include <duckdb/storage/buffer_manager.hpp>

struct AllocatorFixture {
    // No DatabaseInstance in unit tests, so use the global default allocator
    duckdb::Allocator& allocator = duckdb::Allocator::DefaultAllocator();
};

// Unit tests have no DatabaseInstance by default, so spin up an in-memory DuckDB and borrow its BufferManager.
struct BufferManagerFixture {
    duckdb::DuckDB db{nullptr};
    // Blocks allocated through this BufferManager stay in memory (no checkpoint / no disk).
    duckdb::BufferManager& bm = duckdb::BufferManager::GetBufferManager(*db.instance);
    duckdb::QueryContext context;
};

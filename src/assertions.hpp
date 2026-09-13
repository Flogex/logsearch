#pragma once

// Defines D_ASSERT_IS_ENABLED, which the test below reads.
#include <duckdb/common/assert.hpp>

//! Defined when D_ASSERT actually checks its condition.
// In debug mode or with DUCKDB_FORCE_ASSERT, D_ASSERT calls DuckDBAssertInternal, otherwise it aliases `assert`.
// `assert` is a no-op when NDEBUG is defined.
#if defined(D_ASSERT_IS_ENABLED) || !defined(NDEBUG)
#define LS_ASSERTS_ENABLED 1
#endif

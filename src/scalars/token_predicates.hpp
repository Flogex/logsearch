#pragma once

namespace duckdb {
class ExtensionLoader;
} // namespace duckdb

namespace logsearch::scalars {

//! Registers contains_token, contains_all_tokens and contains_any_tokens.
//! Each one is a complete predicate that evaluates over the data and needs no index; an index only makes
//! it faster. See ../scalars/syntax.md for the semantics these functions implement.
void RegisterTokenPredicates(duckdb::ExtensionLoader& loader);

} // namespace logsearch::scalars

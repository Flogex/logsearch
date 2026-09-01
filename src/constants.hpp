#pragma once

namespace logsearch {

// Names are defined as constants used across modules because multiple modules rely on them as some kind of "contract".

/***** Token predicate scalar functions *****/
constexpr const char* CONTAINS_TOKEN_NAME = "contains_token";
constexpr const char* CONTAINS_ALL_TOKENS_NAME = "contains_all_tokens";
constexpr const char* CONTAINS_ANY_TOKENS_NAME = "contains_any_tokens";

} // namespace logsearch

# Scalars — TODO

- Add exact match option for scalar functions
- Add phrase matching
- Support non-constant query token arguments (e.g. other expressions, column refs)

New syntax:

```sql
-- wildcards: '%' only, inside a single token, never crossing a token boundary
contains_token(col, 'prefix%')
contains_token(col, 'literal\%', escape := '\')
-- phrases: a VARCHAR analyzed into ordered tokens
contains_phrase(col, 'my phrase')
-- case/diacritic-sensitive matching
contains_token(col, 'ABC', match_type := 'exact')
-- how many list elements must be satisfied, default 1
contains_any_tokens(col, ['a','b','c'], min_match := 2)
```

## Open questions

1. **Does `match_type` stay one opaque `'exact'`**, bundling case folding, ASCII folding and later stemming,
   or split into orthogonal flags the way MongoDB does (`$caseSensitive`, `$diacriticSensitive`)? Defining
   `exact` as "byte-identical token" keeps the door open — and note it has to skip *every* normalization
   stage, stemming included. Match mode belongs to the analyzer, not the predicate: exact mode is the chain
   with its normalization stages removed, and it is now the only such axis.
2. **Surfacing the analyzer configuration.** `duckdb_indexes()` lists logsearch indexes with `expressions` and
   the full `sql` but has no `index_type` column. Its `tags MAP(VARCHAR, VARCHAR)` is the natural place for
   tokenizer and analyzer settings.
3. **Whether the index should exclude high-frequency terms at all**, and if so how the query side learns which
   ones. Resolved for now by excluding nothing, which is what makes a dictionary miss a proof of no matches.
   The full argument is in `../inverted-index/TODO.md`.

## Future

- `min_match` is already expressible without new syntax, since booleans sum:
  `(contains_token(c,'a')::INT + contains_token(c,'b')::INT) >= 2`. The parameter is sugar over that.
- `contains_phrase` needs no positional index to ship: the index supplies all-tokens candidates and the scalar
  implementation rechecks adjacency. Positions become a later speed-up, not a prerequisite.
- PostgreSQL `tsquery` compatibility, if ever, as a separate surface — not by overloading these functions.

## Wildcards, expansion and min_match — deferred, but decided

- **`%` only. No `_`.** Logs are full of snake_case identifiers, so `contains_token(col,'user_id')` would
  silently become a wildcard query — a far worse collision with real data than `%` is.
- **No default escape character**, matching DuckDB `LIKE`, which has none either. An `escape :=` parameter
  mirrors `like_escape(str, pattern, escape)`. Without it there is no way to match a literal `%` — the same
  trade-off `LIKE` already makes.
- **A wildcard never crosses a token boundary.** The sharpest difference from `LIKE`'s `%`, and it follows
  from the function being a *token* predicate.
- **A wildcard expands to a disjunction of dictionary terms**, the rewrite Lucene performs for a
  `MultiTermQuery`. So `contains_all_tokens(col, ['pre%','fixed'])` matches a document holding
  `prefix1 bla fixed`.
- **No expansion cap, and never a truncated expansion.** Elasticsearch's `rewrite` and MongoDB's
  `maxExpansions: 50` are *ranker* behaviour, where dropping terms only loses weak hits. A filter that
  truncates returns wrong rows. Instead a wide expansion falls back to a scan, under the same
  `logsearch_max_match_fraction` budget that gates the character predicates.
- **The budget is checked while walking the dictionary range, not after** — accumulate `CountPostings` and
  abandon the index path as soon as it is exceeded. That bounds plan-time work without capping the expansion.
- **`min_match` counts list elements satisfied, after dedup — never expanded terms.** All the terms one
  element expands to count once, otherwise a wildcard would silently inflate the count.
- **`min_match` must name a satisfiable number of elements**, so the range is `[1, distinct count]`: below one
  the predicate is constant TRUE, above it constant FALSE. The bound is the *deduplicated* count. The empty
  list is exempt — `contains_any_tokens(col, [])` is FALSE whatever `min_match` says, so validating against it
  would turn the documented answer into an error.
- How should wildcards interact with the exact match mode?

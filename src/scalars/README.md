# Token predicates

The user-facing search predicates, registered as DuckDB scalar functions:

```sql
contains_token(string VARCHAR, token VARCHAR, <named_options>) -> BOOLEAN
contains_all_tokens(string VARCHAR, tokens VARCHAR[], <named_options>) -> BOOLEAN
contains_any_tokens(string VARCHAR, tokens VARCHAR[], <named_options>) -> BOOLEAN
```

Every scalar function has a base implementation that runs every row/document in the `DataChunk` through the analyzer
pipeline and checks if the terms in the document contain the analyzed search term.
This works even when there is no Logsearch index defined on the input column; it will just be faster with the index.
It is also nice because views that use these scalars don't break when the index is dropped.

Even with the index, the base implementations will be called

- as a recheck for, for instance, exact matches,
- if the fraction of documents that match the predicate is so high that a full table scan is the better option, and
- in non-filter contexts like projections, join conditions, aggregates.

Additionally, having these implementations is a good test oracle for the optimizer/inverted index lookup.

A simpler alternative would be to just throw a `NotImplementedException` when we don't have a Logsearch index defined on
the input column, but the predicates are way more useful this way since they can be used for all the things mentioned
above.

## Semantics

Let $A(s)$ be the set of terms the analyzer produces for a document/query $s$ (think `Pipeline::Run(s)`).<br>
We define the base predicate as $\texttt{contains_token}(d, t) \iff A(t) \subseteq A(d)$.

Note that

- for an empty or all-whitespace string the analyzer produces no terms, so $A(\varepsilon) = \emptyset$, and
  $\forall d : \emptyset \subseteq A(d)$.
  In other words, an empty or all-whitespace string satisfies `contains_token` for every document.
- a token input with $|A(t)| \ge 2$ raises a `BinderException`.
  `contains_token` only accepts a single query token.
- `contains_token` compares whole tokens, unlike `contains` which does substring matching.
  For example, `contains_token('connection refused','efuse')` is false.
- `contains_token` is case-insensitive on both sides when not in exact matching mode.
  For example, `contains_token('CAFE SERVES LATTE','cafe')` is true.
- the tokenizer decides where to split tokens (currently only on whitespace) and defines the cardinality of $A(s)$.

From here, we can define the other predicates:

- $\texttt{contains_all_tokens}(d,T) \iff \bigcup_{t \in T} A(t) \subseteq A(d)$
- $\texttt{contains_any_tokens}(d,T) \iff \exists t \in T: A(t) \subseteq A(d)$

This implies $\forall d,\; d \neq \mathrm{NULL}$:

- $\texttt{contains_all_tokens}(d,\emptyset) = \mathrm{true}$ (identity of conjunction)
- $\texttt{contains_any_tokens}(d,\emptyset) = \mathrm{false}$ (identity of disjunction)
- $\texttt{contains_all_tokens}(d,\{\varepsilon\}) = \mathrm{true}$
- $\texttt{contains_any_tokens}(d,\{\varepsilon\}) = \mathrm{true}$

All predicates use `DEFAULT_NULL_HANDLING`:
Any NULL argument (document or query) results in a NULL output, as the three-valued logic of SQL implies.
The exception is NULL values in the tokens lists of `contains_all_tokens`/`contains_any_tokens`:
This raises a `BinderException` instead.

If you think of `contains_any_tokens(d, ['a', 'b'])` as `contains_token(d, 'a') OR contains_token(d, 'b')`, then
`contains_any_tokens(NULL, ['', 't'])` should be true following the SQL semantics.
However, when using `DEFAULT_NULL_HANDLING`, DuckDB guarantees NULL whenever any argument is NULL.
Our binding function never sees NULL input.
We kept this semantics just because it is easier, and then we don't have to implement NULL handling ourselves.
(It is also questionable whether a row with unknown document text should match said predicate.)
For the same reason, `contains_token(NULL, '')` is NULL.

In summary:

| Case | `contains_token` | `contains_all_tokens` | `contains_any_tokens` |
|---|---|---|---|
| `col` is NULL | NULL | NULL | NULL |
| token / list argument is NULL | NULL | NULL | NULL |
| NULL element inside the list | — | `BinderException` | `BinderException` |
| empty list `[]` | — | TRUE | FALSE |
| empty token `''` | TRUE | TRUE | TRUE |
| argument tokenizes to > 1 token | `BinderException` | `BinderException` | `BinderException` |

## Goals for the syntax

- Feel SQL-native: No query-string mini-language or completely new grammar (like `search(col, 'pre% OR "a phrase"')`).
  Boolean composition is SQL's job, though we still provide all/any functions as syntactic sugar.
- One predicate covers one column, so we don't need field-scoped term syntax (`level:error`).
- Ability to express what matters for logs: conjunction, disjunction, exclusion, exact match, and later prefix/wildcard,
  phrases, fuzziness.
- Filter, not ranker: We don't do scoring, just boolean retrieval. Every predicate returns `BOOLEAN`.

The reason to add the all/any functions was mainly convenience: It is easier to write

```sql
WHERE contains_any_tokens(msg, ['t1', 't2', 't3'], min_match := 2)
```

than

```sql
WHERE (contains_token(msg, 't1')::INT + contains_token(msg, 't2')::INT + contains_token(msg, 't3')::INT) >= 2
```

## Terminology: token, not term

Generally speaking, a document is first tokenized into _tokens_ (raw instances of text, at a specific location).
In the process of normalization they become _terms_.
Terms are what get inserted into the indicis dictionary.
Similarly, a user query consists of tokens which are run through the analyzer pipeline to produce search terms.
The search terms are what gets looked up in the dictionary.

The basic scalar function is named `contains_token`, not `contains_term`, because it takes a single raw token as input.
Not a query consisting of multiple tokens, just either one token or, in the case of `contains_any_tokens`/
`contains_all_tokens`, a list of tokens.
The tokens are then run through the analyzer pipeline before doing the index lookup.

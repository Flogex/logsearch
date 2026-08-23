# Inverted index — TODO

- Bump DuckDB to build against `main`; absorb the storage-system and other API changes it brings.
- Integrate the index with DuckDB for real: wire the `BoundIndex` / index-type surface (`Append`, lookup/scan,
  `create_instance`, storage info) so it works end-to-end as a secondary index, not just standalone data structures.
- SSTable: persist to the database file (checkpoint the blocks via `PartialBlockManager` / `ConvertToPersistent`,
  `IndexStorageInfo`, `create_instance`). Requires a defined on-disk byte order (endianness) and a golden-image
  backward-compatibility test (serialize via `MemoryStream`, compare bytes). Bump `SSTABLE_VERSION` on layout changes.
- SSTable: zero-copy `PostingsCursor` reading a run in place from a pinned block, plus a k-way merge across partitions.
  `Lookup` currently materializes a `std::vector<row_t>` per partition and the query path concatenates them.
- SSTable: dictionary sparse index (anchor every k-th entry) — needed once front coding breaks direct binary search.
- SSTable: resident dictionary (keep the dict blocks pinned, or decode once into memory) if `Lookup` proves hot.
- Analyzer: cap token length so `term_length` always fits `uint32` (a longer term currently truncates in `Build`).
- Throw (not `D_ASSERT`) if the dynamically configured row group size makes `postings_count` exceed `uint32`
  (see "Dynamic row group size").
- Cap the Memtable size.
- Track memory of dictionary. Use duckdb::string_t and duckdb::OwningStringMap\<PostingsList>.
  This also enables us to not having to allocate a string for lookup because we can construct a non-owning string_t.
- Inline the first few row IDs in `PostingsList` to avoid a segment allocation for the many cold (Zipfian) terms.
  - For PostingsList that only contain one or two terms, there is still an overhead of a 16-slot PostingsSegment.
    On a 64-bit system, we could instead inline the first two row IDs at the position of the head and tail pointer.
    This would help with a fat-tailed Zipfian distribution of terms arriving.
    In a log message workload, the distribution is bimodal: There are many terms repeatedly occurring from templates,
    but identifiers, measurements, etc. only occur very rarely.
    The question then becomes if we should actually store the high-cardinality variable fields in the dictionary.
    When we add positional information, 16 bytes of space will not be enough anymore.
    Furthermore, we expect the Memtable to be relatively short-lived and then be converted to a more space-efficient
    representation.
- Trailing prefix scan (`pre*`).
- Positional index.
- Dynamic row group size (from TableIOManager)
- Dynamic initial hash-table size.
- Postings List compression (delta encoding)
- Dictionary compression (front coding)
- Support deletions (and updates?)
- Consumers of an SSTable probably don't need to know the layout, just that the Lookup functions exist.
  Hence, we might be able to make the .hpp file smaller.
- Endianness of integers on disk

## Dictionary filtering: which terms to store

**Nothing is filtered today, and that is a deliberate position, not an omission.** Every token the analyzer emits
gets a dictionary entry. The `StopwordFilter` that used to drop a hardcoded English word list before the dictionary
has been removed, and no length filter or other exclusion has replaced it.

If filtering comes back it belongs **here**, not in the analyzer, and not as a "stopword filter" — the name imports
assumptions from Lucene that do not hold for us (see *Why the ordering problem disappears*). The analyzer must stay
total, because its output is also the ground truth for the scalar predicates, which have to answer for terms the
index chose not to store. `../analyzer/README.md` has that argument.

### The model: what a filter does to correctness

An analyzer is a function `A : Document → Set(Term)`, and the index answers a predicate exactly when
`P(doc, q) ⟺ A_q(q) ⊆ A(doc)`. A filter can break that biconditional in three ways, and they are not
equally bad:

| kind | example | what breaks | recoverable? |
|---|---|---|---|
| **collapse** (n:1) | case folding, stemming, truncation | false positives | yes — the residual predicate rechecks candidates |
| **expand** (1:n) | n-grams, synonyms, shingles | nothing; costs space only | n/a |
| **delete** (1:0) | stopwords, length filters | false negatives | **only if predictable at query time** |

**A false positive costs one recheck; a false negative is a silently wrong answer.** That asymmetry is the whole
design rule: collapse and expand freely, but every deletion must be predictable from the query token alone.

### Three tiers of deletion

| tier | `ShouldStore(t)` depends on | examples | cost to support |
|---|---|---|---|
| **1 — term-local** | the term plus the index's pinned config | length, codepoint count, character class, static denylist | **free** — the query side recomputes it; nothing is stored |
| **2 — corpus-dependent** | statistics over the whole corpus | frequency-derived stopwords | the excluded set (or a filter over it) must be **persisted** and consulted |
| **3 — document-local** | *where in its document* the token appeared | `LimitTokenCount`, `LimitTokenPosition` | **do not adopt** — see below |

Tier 3 is categorically worse than the others and worth ruling out explicitly. Whether a term is indexed depends on
its position in a document, so no per-term test can predict it. A dictionary *hit* no longer means the postings list
is complete, which yields false negatives per row. A superset can be rechecked; a subset cannot, so the index stops
being usable even as a prune. Every other dropping filter degrades the index to "cannot help with this term"; this
one degrades it to "cannot be trusted at all".

### Why the ordering problem disappears

Lucene must run `StopFilter` *before* stemming, because dropping a term there destroys recall permanently: stem
"beings" to "be", drop "be", and no query can ever find that document again. "Stopwords before stemming" is a
consequence of that, not a law.

Here a term absent from the dictionary costs only speed — the predicate still answers over the data — so there is no
recall to protect and no ordering constraint. The decision belongs at the dictionary boundary, keyed by whatever the
dictionary is keyed by. Note also that once a stemmer exists, "keep `beings` but drop `be`" is not a coherent wish:
they are the *same dictionary entry*. The selectivity of `beings` was destroyed by the stemmer, not by the exclusion,
so excluding after stemming costs nothing that stemming had not already cost.

The general rule that replaces Lucene's: **a storage filter is safe anywhere in the chain, as long as the query side
applies it at the same point.** Tier 1 can therefore be hoisted early as a pure optimization — dropping long tokens
before stemming them saves real CPU — provided build and query test the same form (pre-stem or post-stem,
consistently).

### Candidate policies, and how good each one is

**High-frequency terms ("stopwords").** Buys the most space: a common term has a tiny dictionary entry and an
enormous postings list. Costs the least in query performance, because `logsearch_max_match_fraction` already
declines to *use* the index for a term matching too much of the table — so a very common term costs disk and
nothing else. Exclusion and that gate are the same criterion at different points in the lifecycle, build time
versus plan time, and the plan-time one is strictly better informed. If they ever shared a threshold, exclusion
would be pure compression. But it is Tier 2, so it is the variant that forces the persisted excluded set.

**Very long terms (`RemoveLong`).** Tier 1, so free to support. Tantivy's default chain is roughly
`SimpleTokenizer -> RemoveLong -> LowerCase` with a limit somewhere around 40 characters (exact number unverified),
and Quickwit is a log search engine built on it — so there is direct precedent for our workload. But note the cost
structure is **inverted** from stopwords:

| | dictionary cost | postings cost | index payoff when queried |
|---|---|---|---|
| common term | tiny | enormous | ~none — the plan-time gate declines it anyway |
| very long term | large | ~1 entry; long tokens are nearly always unique | **maximal** — a lookup returns one row |

A term occurring in exactly one row is the most valuable entry an inverted index can hold. So a length filter
excludes precisely the entries with the highest payoff, where stopword exclusion removes the ones with the lowest.
For log search this is pointed: long tokens are trace IDs, request IDs, hashes and base64 blobs, and "find the
request with this trace ID" is a primary use case.

The threshold is therefore the entire feature. A UUID is 36 characters and MD5 hex is 32, both under Tantivy's ~40;
SHA-256 hex at 64 would be dropped. Pick it from the token-length histogram of a real corpus, not from a round
number, and expect the interesting cliff around 32-40. Length is also only a proxy for what we actually want gone —
the high-cardinality variable fields noted in the `PostingsList` item above — and it is a leaky one, since a short
UUID prefix is short and unique while a long error constant may be exactly what someone searches for.

**Truncating long terms instead of dropping them — the better option if space is the goal.** Cap stored terms at N
bytes. This converts a *deletion* into a *collapse*, moving from the fatal failure mode to the cheap one:

- No false negatives are possible: if two tokens are equal then their truncations are equal, so a match can never be
  missed.
- A query token longer than N truncates the same way, the lookup returns every row sharing that N-byte prefix — a
  superset — and the residual predicate finishes the job. An N of ~40 makes that prefix effectively unique, so the
  candidate set stays tiny.
- The bytes saved are the same bytes above N that `RemoveLong` would have saved, but acceleration for identifier
  lookups is kept rather than destroyed.
- Cost: `contains_token` demotes from "the index answers" to "the index prunes, the predicate rechecks" for tokens
  longer than N. That is a Tier 1 term-local test (`len(t) > N`), so the optimizer knows locally, per token, whether
  the residual predicate must be kept. Queries for tokens shorter than N stay exact.

**Expansions** (n-grams, synonyms, shingles) are the structurally safe class: they add terms, so they can only cost
space, never recall. The `store_ngrams=N` idea in `../optimizer/TODO.md` is one of these. The one trap is symmetry —
an expansion must be applied on exactly one side, index or query, never both.

### What filtering costs, in mechanism

**The absent-versus-excluded ambiguity.** Today a dictionary miss is a *proof* that no indexed row contains the
term, so the optimizer can answer zero rows and skip the scan. Introduce any deletion and a miss becomes ambiguous
between "the term does not occur" (zero rows, skip the scan) and "the term was deliberately not stored" (the index
knows nothing, fall back). Those need opposite plans. Getting it wrong reports zero rows for a term that does occur
— the one failure mode that matters, because an index must never change an answer, only the time to get it.

**Test `ShouldStore` before the lookup, not after the miss.** The natural order is wrong:

```text
t = Analyze(q)
if !ShouldStore(t):  decline            # no dictionary probe at all
else:                Lookup(t)          # a miss here now proves zero rows
```

Testing first skips a dictionary probe — potentially SSTable block I/O — for terms known to be absent, and it keeps
"miss implies zero rows" as an unconditional inference at the point of use.

**Declining is not always a full scan.** The index has no scan machinery; it withholds a rewrite and DuckDB's
ordinary plan runs. How much is lost depends on where the unusable term sits: an unusable *conjunct* still leaves
the other conjuncts to narrow the scan, as long as the original predicate stays in the plan to recheck the
survivors, while an unusable *disjunct* is the case that really costs a full scan, because nothing bounds the result
any more.

**Pin the policy, not just its parameter.** `ShouldStore`'s identity *and* its arguments must be stored in the index
— the threshold, and which length (bytes or codepoints). Read either from a global setting and changing a default
later makes every existing index confidently report zero rows for terms it simply never stored, silently, on
upgrade. This is the one real footgun in Tier 1, which is otherwise free.

### If it is built

- Express it as a single named `ShouldStore(term)` predicate with exactly two call sites: the index build, and the
  optimizer at plan time. Two call sites cannot drift the way two analyzer chains would.
- Decide it from the term counts the build already has, rather than from a word list.
- Persist the policy identity and parameters; for Tier 2, persist the excluded set as well.
- Surface it through `duckdb_indexes()`'s `tags` column, so a user can see why a query is slow, and ideally through
  `EXPLAIN` on the plan that declined.
- Measure the win first. The top-k terms' share of total postings entries, and the token-length histogram, are both
  computable from a real corpus with the counts the build already collects. Front coding and the sparse dictionary
  index — both listed above — buy dictionary space without giving up any lookup, and should be exhausted first.

### Not to be confused with the `uint32` term-length cap

The "cap token length so `term_length` always fits `uint32`" item above is a **correctness** bug at a 4 GB format
boundary, not a space policy. A format limit must throw or truncate loudly; a space policy is a tunable that has to
be reproducible at query time. Keep them separate.

One thing that needs no new handling: a document whose every term is excluded contributes nothing to the index,
which is the same situation as an empty document — already covered by the "index stays empty when inserting
documents without any term" test.

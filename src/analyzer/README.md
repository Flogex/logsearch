# Analyzer Pipeline

The analyzer turns a document into a list of normalized terms ready to be written into the inverted index.
The public interface is the `Pipeline::Run` function in `pipeline.hpp`.
Everything else is an "implementation detail".

## High-level Shape

The Unicode path is not implemented yet.

```text
                                ┌── Pipeline::Run(string_view) ──┐
                                │                                │
                                │  arena.Allocate + memcpy       │
                                │            ↓                   │
                  ┌─ ASCII ─────┤  IsAscii(doc)? ─── Unicode ─┐  │
                  │             │                              │  │
                  ▼             │                              ▼  │
        AnalyzeAscii(span)      │              AnalyzeUnicode(span)│
        ────────────────        │              ────────────────────│
        Lowercaser              │              [TODO] NFKC         │
              ↓                 │              [TODO] AsciiFold    │
        Tokenizer               │              [TODO] UnicodeLower │
              ↓                 │              ...                 │
        TermLengthGuard         │              throws              │
              ↓                 │              NotImplemented      │
        TermCollector           │                                  │
                                └──────────────────────────────────┘
                                          ↓
                                vector<string> output
```

Two paths, picked by a single top-level branch on `IsAscii(document)`.
Each path is fully inlined, i.e., there is no runtime indirection inside the stage chain.
We try to do as many modifications as possible on the entire document to avoid overhead from function calls
and give SIMD operations a large contiguous byte range to operate on.
For Unicode specifically, NFKC can change whitespace bytes and therefore needs to run before tokenization.
Therefore, the document is only split into tokens once it has been normalized and casefolded.

The stages generally should be able to assume that previous stages have run.
For instance, the Tokenizer is only ever handed a document the Lowercaser has already folded.

## Usage for both Ingestion and Retrieval

... and why a stopword filter cannot live in the analyzer.

The analyzer is used on documents that are inserted into the inverted index and on query tokens ("search terms").
To get correct results, it is important that the document tokens and query tokens are normalized in the same way.
The analyzer should not make decisions about which terms get stored (stopword filter, maximum length filter), i.e., not
drop any terms.

Generally speaking, a stage of the analyzer can collapse a token, delete it, or expand it into multiple tokens.
Examples for each category, most of them not implemented in our analyzer:

- Collapse (the mapping is not injective because multiple tokens can be mapped to the same output):
  stemming (being -> be), accent/case folding (Ḟøłɖǐṅg -> folding), truncation of long tokens
- Delete: stopword filter or length filter drop the token completely
- Expand: synonyms, CommonGrams, N-grams, ReverseString (indexing the reversed token as well, so that a suffix lookup
  becomes a prefix lookup) emit several tokens for one input

Collapsing can lead to false-positive index matches, so for operations that require strict comparisons (e.g. `contains`,
`=`, or `match_type := 'exact'`) the index only produces a candidate set on which the actual filter is applied.
Expanding is harmless: it adds terms, costing space but never a match.
Deleting is the problem, because the index cannot distinguish "this token was dropped by the analyzer" from "this token
was never seen in the corpus" unless it records what it dropped.
When nothing gets dropped, a term missing from the dictionary proves that no row contains it, and the scan can be
skipped entirely.

Deleting is worse still on the query side, where a token can become empty.
In non-exact mode, `contains_token(col, t)` is defined as "the analyzed value of `col` contains the analyzed `t`", i.e.,
the analyzer runs on both sides and then both sides are matches.
Searching for "the" (a stopword) would therefore return TRUE for every document, because `t` analyzes to no token at all
and containing none of them is trivially true.
By that definition the result is not incorrect, but a filter that silently matches everything is undesired behavior.

There is still value in filters that reduce storage cost.
A maximum length filter could keep garbage such as binary blobs out of the index, and a stopword filter could drop the
most common terms which would be answered by a full table scan anyway.
Truncation is usually the better tool than a length filter, because it collapses instead of deletes.
But this is a policy decision for the index, not the analyzer: it needs corpus-wide term counts that the analyzer never
sees, and the query side must be able to reproduce or look up the decision.
The only cost is just that the analyzer processes such tokens without exiting early.

## Coding Practices

The log-message workload we optimize for implies a high sustained ingest rate, documents that are small
(one log line, typically 100–2000 bytes), and content that is mostly ASCII (IP addresses, error codes, JSON;
English message text only occasionally).
We expect most documents to take the fast ASCII path and only occasionally having to do processing of Unicode documents.
We accept (for now) that we cannot match Lucene's flexibility and that we cannot handle all possible languages.

This is manifested in the following coding practices:

- Avoiding allocations for predictably high throughput:
  We copy the incoming document once at the beginning of the pipeline because we need to take ownership.
  All subsequent operations on the ASCII path happen in-place on this allocated string.
  This model will break once we start doing Unicode processing or adding synonyms to the index.
  For now, we ensure with `noexcept [[clang::nonblocking]]` that the "kernels" of the pipeline stages do not allocate.
- All allocations should be tracked by DuckDB:
  That is, we want to use DuckDB's allocator for everything.
- Push-based pipeline: The document and the tokens are passed through the pipeline as `MutableSpan`.
  After tokenization, every stage can produce zero or more tokens that it pushes directly to the downstream consumer
  without buffering.
- Auto-vectorizable kernels: Iterating over strings in a way that allows the compiler to vectorize the hot loops.
- Parallelizable: No shared state between Pipelines except for a shared `duckdb::Allocator`.
  The goal is to be able to run an analyzer pipeline for many documents in parallel.
- No virtual function calls: The pipeline stages are fixed at compile time and the optimizer can inline them.
- Testing: We have unit tests for each stage.

Build setup:

- Unity build: This allows the compiler to see the analyzer as one translation unit.
- Recompile shielding: The rest of the codebase depends only the _pipeline.hpp_ header file.
  Changes in the _analyzer_ directory do not require recompilation of other parts of the extension.

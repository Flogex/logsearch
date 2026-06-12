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
        StopwordFilter          │              throws              │
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

After tokenization, we should filter out tokens before processing them further and put the cheaper filters first.
For example, if we introduce a length-based filter in the future, this should run before the `StopwordFilter`.
If we add a stemmer, this operation should come last as it is the most CPU-intensive per-token work.

The pipeline stages assume that previous stages have run.
For example, all stopwords are in lowercase.
Also, the `StopwordFilter` needs to run before the stemmer.
Otherwise, a word like "beings" with stem "be" will get filtered out.

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
- Parallelizable: No shared state between Pipelines except for an `ArenaAllocator`.
  The goal is to be able to run an analyzer pipeline for many documents in parallel.
- No virtual function calls: The pipeline stages are fixed at compile time and the optimizer can inline them.
- Testing: We have unit tests for each stage.

Build setup:

- Unity build: This allows the compiler to see the analyzer as one translation unit.
- Recompile shielding: The rest of the codebase depends only the _pipeline.hpp_ header file.
  Changes in the _analyzer_ directory do not require recompilation of other parts of the extension.

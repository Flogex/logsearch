# Analyzer Pipeline — Open Questions & TODOs

- `TermCollector` is a dummy sink. The analyzer pipeline should be integrated into the rest of the extension.
- The `Tokenizer` currently only splits on whitespace. It should also split on punctuation (commas, periods, colons).
- Potentially implement a Porter stemmer. I think this is a modification that can be done in-place: Even though some
  steps grow the string (e.g. at→ate), the original capacity is never exceeded.
- Implement the Unicode path (NFKC normalize → Unicode case fold → ASCII fold → tokenize), probably using utf8proc.
- Add benchmark for end-to-end throughput (rows/sec, MB/sec) and multi-thread scalability.
- Graceful degradation under memory pressure.

## Unicode Implementation

Library choices, according to Claude:

- utf8proc (lighter, in DuckDB's third_party): normalization + case folding only. Sufficient if we don't need full UAX #29 tokenization. First choice.
- ICU: heavy (tens of MB), C-style API, but the only correct option for full Unicode work — BreakIterator (UAX #29), locale-aware casing, transliteration.

### Diacritic / ASCII folding caveats

Diacritic stripping is lossy in ways that vary by language, according to Claude:

- European Latin (French, Spanish, Italian, Portuguese): orthographic accents, lossless for search. `café` → `cafe` is correct.
- Vietnamese: tone marks are phonemic. `má`/`mà`/`mả`/`mã`/`mạ` all collapse to `ma` — distinct words become indistinguishable.
- Turkish: `ı`/`i` and `ş`/`s` are different letters, not decorated variants. Folding merges distinct words.
- German has a convention: `ö`/`ü`/`ä`/`ß` → `oe`/`ue`/`ae`/`ss` rather than `o`/`u`/`a`/`s`. ICU's German-specific transform handles this; generic `Latin-ASCII` doesn't.

Strategy for general multilingual search: **index both original and folded forms** at the same position (Lucene's `ASCIIFoldingFilter` with `preserveOriginal=true`). For ASCII-dominant log search the simpler "fold and discard" approach is acceptable.

For CJK: **don't fold to ASCII at all.** Transliteration to pinyin collapses many distinct characters and destroys searchability. Use a proper segmenter and index the original characters.

## Handling Memory Pressure

If we want to cap the memory usage per thread/pipeline and respond to memory pressure:

1. **Local arena exhausted** (one document is pathological). Other threads fine. Response: truncate this document via an explicit return code, `arena.reset()`, continue with next document. Truncation must be **observable** via a counter — silent partial indexing is the worst outcome.
2. **Process-wide pressure** (cgroup limit approaching). Atomic byte counter + background pressure-level enum (`Normal` / `Elevated` / `High` / `Critical`). Workers consult it per document and adjust: flush sooner at Elevated, refuse new documents at High, truncate aggressively at Critical.
3. **System-wide thrashing**. Same as Critical, plus proactive flush of posting buffers even at suboptimal segment sizes — small segments are recoverable, OOMs aren't.

Furthermore, think about smarter policies for clearing the arena (or continuing to append to it if still enough space).

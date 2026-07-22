# Inverted index — TODO

- Bump DuckDB to build against `main`; absorb the storage-system and other API changes it brings.
- Integrate the index with DuckDB for real: wire the `BoundIndex` / index-type surface (`Append`, lookup/scan,
  `create_instance`, storage info) so it works end-to-end as a secondary index, not just standalone data structures.
- SSTable: persist to the database file (checkpoint the blocks via `PartialBlockManager` / `ConvertToPersistent`,
  `IndexStorageInfo`, `create_instance`). Requires a defined on-disk byte order (endianness) and a golden-image
  backward-compatibility test (serialize via `MemoryStream`, compare bytes). Bump `SSTABLE_VERSION` on layout changes.
- SSTable: zero-copy `PostingsCursor` reading a run in place from a pinned block, plus a k-way merge across partitions.
  `Lookup` currently materializes a `std::vector<row_t>` per partition and the query path concatenates them.
- SSTable: thread `QueryContext`/`ClientContext` through `Lookup` -> `PinnedBlockCache` -> `BufferManager::Pin` for
  per-query I/O attribution and cancellation. No-op while blocks are in memory; relevant once they are on disk/remote.
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

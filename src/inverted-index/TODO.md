# Inverted index — TODO

- Implement SSTable
- Cap the memtable size.
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
    Furthermore, we expect the memtable to be relatively short-lived and then be converted to a more space-efficient
    representation.
- Trailing prefix scan (`pre*`).
- Positional index.
- Dynamic row group size.
- Dynamic initial hash-table size.
- Postings List compression (delta encoding)
- Dictionary compression (front coding)
- Support deletions (and updates?)

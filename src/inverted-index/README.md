# Inverted index

Data structures for the inverted index (dictionary and postings list).
We build one inverted index "partition" per DuckDB row group.
The dictionary contains the terms that occur in the indexed column of the rows in that row group.
The postings lists currently only contain the row IDs (document identifiers) of the rows that contain the term.
That is, it is missing positional information, frequency information and other metadata.

## Ingest path

The Logsearch index is not a Delta index, so `BoundIndex::Append` is called at commit-time, and only one `Append` can
run at a time (DuckDB holds an `IndexLock`).
For each row of the incoming `DataChunk` we run the analyzer pipeline on the indexed `VARCHAR` column.
The terms emitted by the analyzer are as a first step stored in an in-memory dictionary, together with in-memory
postings lists (memtable).
The optimization goal here is ingest throughput, accepting that we use more memory than strictly necessary.
For example, the memtable cannot be evicted.

A memtable becomes full exactly if the corresponding row group is full.
A full memtable gets "sealed" and converted to an immutable, buffer-managed SSTable.

A row can contribute the same term multiple times (the analyzer does not deduplicate within a document), so `Insert`
must tolerate a repeated row ID for a term.
Rows arrive ordered by row ID, so each postings list is append-only and (after dedup) strictly increasing.

We assume ~50K unique terms per partition for now.
This needs to be verified against real data and possibly made dynamic.

## Memtable layout

We store an inverted index for each row group.
The non-sealed inverted index stays in a memtable and gets converted to a SSTable once full.

The dictionary maps `std::string -> PostingsList`.
For the bucket array of the `std::unordered_map`, we use normally malloced memory which gets freed when reallocated.
This memory is currently not tracked by DuckDB.

Each postings list is a forward-linked chain of fixed-size segments bump-allocated from a `duckdb::ArenaAllocator`.
The arena only frees when it is destroyed, so we want to keep reallocations near zero.
When the list grows, we add a new segment to the end without having to reallocate the list (similar to `std::deque`).
The drawback of using fixed-size segments is that we waste space at the end of partially filled lists for each term.
A single term's list is at most 122880 entries * 8 bytes ~= 1 MB, which fits comfortably in memory.

## SSTable layout

A Sorted String Table (SSTable) is an immutable, block-based partition (one row group) of the inverted index.
It gets created when a memtable is sealed after seeing rows from a new row group.
It holds the dictionary and the row-ID postings lists for that partition.

The SSTable is one logical byte image, split every `block_size` bytes across N buffer-managed blocks.
All offsets in the image are logical (byte offsets into the image).
The block size is typically 262,136 bytes (block allocation size of 2^18 bytes minus 8 header bytes).
Unfortunately, this number is not a power of two, so division and modulo are expensive.

```text
logical image:
  +----------+-----------------------+-------------+-----+---------------------------+
  | Header   | SSTableDictEntry[]    | String Pool | pad | Postings (raw row_t)      |
  +----------+-----------------------+-------------+-----+---------------------------+
  0          dict_offset             strings_offset      postings_offset (8-byte-aligned)
physical: the image cut every block_size bytes into blocks_[0..N):
  | block 0 | block 1 | ... | block N-1 |
```

Header / dictionary / strings may straddle a block boundary.
The postings region is 8-byte aligned and `block_size` is a multiple of 8, so a raw 8-byte `row_t` is always aligned and never split across a boundary.

One alternative would be to have one block per postings list, letting the checkpoint manager compact later.
We chose a single logical byte image because of its lower overhead and fragmentation.
Furthermore, it keeps the the data close together both in memory and on-disk.
The cost (dict/strings straddling boundaries) is handled by a small memcpy during binary search in the dictionary.

### Dictionary

A fixed-size `SSTableDictEntry` array, sorted by term, is binary-searched directly in the blocks.
For each dictionary entry looked at during search, it loads the term bytes at the given offset and compares them with the search term.
If a term spans multiple blocks, it is compared chunk-wise with the search term.
The byte-wise string comparison is endianness-independent.

We deliberately do not use `duckdb::string_t` in the on-block dictionary because its non-inlined form stores a raw `char*` that is unstable across a re-pin and unserializable.
Still, the idea of inlining the string is appealing.

### Postings

The document identifier in our inverted index are the row IDs of the rows that contain the term.
They are stored ascending as `duckdb::row_t` (64-bit integer), 8-byte aligned.
This allows reading them directly for, say, a k-way merge.

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

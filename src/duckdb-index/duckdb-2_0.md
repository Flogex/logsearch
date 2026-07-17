# DuckDB main (storage v2.0.0) — changes relevant to logsearch

Pinned commit: `7c0fa265df67f4d8ed5cc4f94910c5677d127afd` (main, 2026-07-15). There is no v1.6
release yet; this is post-v1.5.4 development (~1170 merged PRs). The default on-disk storage
format was bumped to **v2.0.0** (version 69) — files written with it do not open in older DuckDB;
pin `STORAGE_VERSION` if compatibility matters.

## Compile breaks (already fixed in our code)

- `BufferHandle::Ptr()` returns `const_data_ptr_t`; writes go through the new `GetDataMutable()`.
  Motivated by the mmap I/O mode: const `Ptr()` prevents silent writes through mmap-backed
  buffers. [#22404](https://github.com/duckdb/duckdb/pull/22404),
  [#22988](https://github.com/duckdb/duckdb/pull/22988)
- `index_type.hpp` was de-hubbed (commit `b958b718c`,
  [#23579](https://github.com/duckdb/duckdb/pull/23579)): it only forward-declares
  `PhysicalPlanGenerator` now, so a `create_plan` callback must include
  `duckdb/execution/physical_plan_generator.hpp` itself.
- `duckdb/function/function.hpp` includes `fmt/core.h` (commit `4b97434e1`), so targets compiling
  against DuckDB headers need `third_party/fmt/include` on the include path (fixed in
  `test/CMakeLists.txt`).

## Row IDs and row groups

- **Gaps in the rowid space are first-class** ([#22822](https://github.com/duckdb/duckdb/pull/22822)):
  vacuum may drop a fully-empty mid-table row group and persist the gap (v2.0.0 files only).
  `total_rows` (live rows) and `next_row_id` (next id to assign) are now separate quantities;
  WAL replay assigns appended rowids from `next_row_id`. Never assume `row_t` values are dense or
  that `max(row_t) == total_rows`.
- **Rowid stability works in our favor**: vacuum may only renumber rows
  (`can_change_row_ids`) when the table's *only* index type is ART
  (`CanRebuildExistingIndexesAfterVacuum` in `row_group_collection.cpp`). With our custom index
  attached, surviving `row_t` values are never remapped — persisted postings stay valid.
  Partially-deleted row groups are skipped by vacuum; fully-live adjacent groups may still be
  merged, which preserves rowids but changes *physical row-group boundaries* (they are then no
  longer aligned to multiples of the row-group size).
- Mapping a `row_t` to its physical row group: `DataTable::GetRowGroupCollection()` →
  `GetRowGroups()` (a `SegmentTree<RowGroup>`) → `GetSegment(row_number)`; the returned
  `SegmentNode` carries the row group's ordinal (`GetIndex()`) — the same ordinal space that
  `LogicalGet::SetPartitionsToScan` and `get_partition_stats` use. `Index`/`BoundIndex` has no
  back-pointer to the table; capture table access at index-build time (all `IndexBuild*Input`
  structs carry `DuckTableEntry &table`).

## Index API (matters once we implement a real BoundIndex)

- `CommitDrop(IndexLock&)` was renamed to `ResetStorage(IndexLock&)` (commit `e4b94f123`);
  same semantics (free all index storage).
- `Identifier` (case-insensitive name type) replaces `string` across index/catalog surfaces
  ([#23161](https://github.com/duckdb/duckdb/pull/23161)): `BoundIndex` ctor and
  `CreateIndexInput::name` take `const Identifier&`; `CreateIndexInfo::index_name` is gone
  (use `GetIndexName()`). Implicit string conversions are being deprecated
  ([#23350](https://github.com/duckdb/duckdb/pull/23350)). We currently have no members that
  need to change; `IndexType::name` itself is still a `string`.
- `CREATE INDEX ... WITH (...)` options now survive checkpoint+reload
  ([#22317](https://github.com/duckdb/duckdb/pull/22317)) — we can rely on options in
  `create_instance`.
- Still **no index-scan extension point**: `IndexType` only covers build/create. Query-time index
  access goes through an optimizer extension / table function (see below).

## Checkpoints

- A failed checkpoint now scope-invalidates the attached database — our future
  persist-at-checkpoint step must fail cleanly.
- Vacuum runs only on a full checkpoint, never a concurrent one (commit `063de8d55`).
- `BoundIndex::SerializeToDisk/SerializeToWAL`, `IndexStorageInfo`, and the partial block manager
  are unchanged since v1.5.4.

## QueryContext plumbing (adopted in our code)

`BufferManager::Allocate`, `Pin`, and `Prefetch` gained `QueryContext` overloads (commits
`eb5f01047`, `8f86e05ae`, `0352107da`) that attribute eviction/spill/read-back I/O to the running
query. Our `MultiBlockWriter`, `SSTableBuilder::Build`, `SSTable::Lookup`, `Memtable::Seal`, and
`InvertedIndex::Insert/Lookup` take an optional trailing `QueryContext`. `QueryContext` wraps an
`optional_ptr<ClientContext>`; never store it in objects that outlive the query (SSTable,
InvertedIndex).

`Prefetch(context, handles)` batch-reads not-yet-loaded blocks (it existed before; only the
context parameter is new). Potential use in `SSTable::Lookup`: prefetch the dictionary-region
blocks before the binary search instead of paying a serial pin-miss per probe — only worthwhile
once blocks can actually be cold (temp-spilled or persisted), and note it skips single-block
batches.

## Query-side integration options (do not forget)

Two mechanisms for "scan only what the index says", plus churn to watch:

1. **Rowid filter + row-group pruning**: an `OptimizerExtension` injects an `ExpressionFilter` on
   `rowid` wrapping a scalar function whose `filter_prune` callback sees per-row-group min/max
   stats and can skip whole row groups
   ([#20633](https://github.com/duckdb/duckdb/pull/20633), pruning fix
   [#23628](https://github.com/duckdb/duckdb/pull/23628), child stats
   [#23540](https://github.com/duckdb/duckdb/pull/23540)). Worked demo upstream:
   `test/extension/loadable_extension_row_id_filter_demo.cpp`. Row-level precision; also the
   late-materialization building block.
2. **`LogicalGet::SetPartitionsToScan(vector<idx_t>)`** (commit `1b9c0190d`, follow-up to
   [#21831](https://github.com/duckdb/duckdb/pull/21831)): restricts a table scan to an explicit
   subset of row groups (indices in `get_partition_stats` order); skipped groups are never read.
   Caveats: an *empty* vector means scan all (zero matches needs an EMPTY_RESULT rewrite),
   indices are positional (not stable ids), and skipping is only correct if the index is complete.
3. **TableFilter → ExpressionFilter rework**: filters were unified onto `ExpressionFilter` with
   `ProjectionIndex` ([#21481](https://github.com/duckdb/duckdb/pull/21481)); the old
   constant/in/null filters are renamed `Legacy...`
   ([#22617](https://github.com/duckdb/duckdb/pull/22617)). Any code we write that constructs or
   inspects `TableFilter` subclasses should target `ExpressionFilter`, not the legacy classes.
4. Supporting pieces: partial pushdown with residual recheck via `TryPushdownRelaxedFilter`
   ([#21350](https://github.com/duckdb/duckdb/pull/21350)), scalar filter functions writing the
   selection vector directly ([#21914](https://github.com/duckdb/duckdb/pull/21914)), table
   functions replacing themselves with a custom plan via `bind_operator`
   ([#21309](https://github.com/duckdb/duckdb/pull/21309)).

## Unchanged assumptions (verified)

- Block geometry: `GetBlockSize()` = 262136 (262144 alloc − 8 B header), still not a power of two.
- `MemoryTag::EXTENSION` (= 11) still exists and remains the right tag for our allocations.
- `Allocate(MemoryTag, size, can_destroy=false)` spill-to-temp-file semantics and
  `ArenaAllocator` are unchanged.

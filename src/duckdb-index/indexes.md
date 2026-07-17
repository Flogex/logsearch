# DuckDB custom index interface: build, persist, load

Reference for how DuckDB's pluggable index system works, so the logsearch index type can
hook into it correctly. Written against DuckDB **main** (post the `CommitDrop`→`ResetStorage`
rename and the `Identifier` index-name change).

Scope: the DuckDB mechanics only — the two core types (`IndexType`, `BoundIndex`), the build
pipeline, and the persistence round-trip. The query-time read path (filter pushdown, custom
scan) and the logsearch-specific design are covered elsewhere.

Header paths below are relative to DuckDB's `src/` tree.

---

## 1. Two planes: `IndexType` vs `BoundIndex`

DuckDB separates the *kind* of index from an *instance* of it.

- **`IndexType`** (`include/duckdb/execution/index/index_type.hpp`) — a plain struct of function
  pointers. One value per index kind (`"ART"`, `"logsearch"`). It is the registration handle:
  a set of factory/build callbacks plus optional `IndexTypeInfo`. It owns no per-table data.
  Registered into `DatabaseInstance.config.GetIndexTypes()` (an `IndexTypeSet`). ART registers
  itself in the `IndexTypeSet` constructor; an extension registers its own type in
  `Extension::Load` via `IndexTypeSet::RegisterIndexType`.

- **`BoundIndex`** (`include/duckdb/execution/index/bound_index.hpp`, extends `Index`) — an
  abstract base for a live index instance on one table. This is where all runtime work happens:
  maintenance under DML, merge, vacuum, verify, serialize. ART subclasses it; a custom index
  must too. Every factory callback on `IndexType` ultimately yields a `unique_ptr<BoundIndex>`.

- **`Index`** (`include/duckdb/storage/index.hpp`) — the unbound base of `BoundIndex`. Its other
  subclass is **`UnboundIndex`** (`include/duckdb/execution/index/unbound_index.hpp`): the
  catalog-level placeholder that exists after a database is loaded from disk but before the index
  has been materialized into memory. An `UnboundIndex` holds the `CreateInfo` and the serialized
  `IndexStorageInfo`; it becomes a `BoundIndex` lazily on first use (see §5).

So there are three states: the *type* (`IndexType`, one per kind), an *unbound* instance
(`UnboundIndex`, loaded-but-cold), and a *bound* instance (`BoundIndex`, live in memory).

---

## 2. `IndexType`: the type handle and its callbacks

```cpp
class IndexType {
    string name;

    // Build pipeline callbacks (CREATE INDEX)
    index_build_bind_t         build_bind         = nullptr;
    index_build_sort_t         build_sort         = nullptr;
    index_build_global_init_t  build_global_init  = nullptr;
    index_build_local_init_t   build_local_init   = nullptr;
    index_build_sink_t         build_sink         = nullptr;
    index_build_combine_t      build_combine      = nullptr;
    index_build_finalize_t     build_finalize     = nullptr;

    shared_ptr<IndexTypeInfo>  index_info         = nullptr;

    index_build_plan_t         create_plan        = nullptr; // escape hatch for the physical plan
    index_create_function_t    create_instance    = nullptr; // instantiate (load / rebind)
};
```

There are two ways a `BoundIndex` comes into existence: the **build pipeline** (fresh
`CREATE INDEX`) and **`create_instance`** (materialize from storage). They are independent —
a fully working index type needs both.

### 2a. Build pipeline — fired by `CREATE INDEX`

`PhysicalPlanGenerator::CreatePlan(LogicalCreateIndex)`
(`execution/physical_plan/plan_create_index.cpp`) looks up the type by name and branches:

```
if (index_type.create_plan)  -> hand the whole physical plan to the type (escape hatch)
else                          -> generic pipeline:
                                 SCAN -> PROJECTION -> [FILTER not-null] -> [SORT] -> PhysicalCreateIndex
```

In the generic path, the projection narrows the table scan to the indexed columns plus the
`rowid`; the optional not-null filter and sort are added based on the callbacks below. The
`PhysicalCreateIndex` operator (`execution/operator/schema/physical_create_index.cpp`) is a
standard sink, so the build runs **parallel** across threads for free.

Callbacks, in fire order:

| Callback | When / thread | Purpose |
|---|---|---|
| `build_bind` | plan time, once | Always called (generic path). Returns an `IndexBuildBindData` carried through the rest of the pipeline. ART uses it to record whether keys should be sorted. |
| `build_sort` | plan time, once | Optional. Returns `bool` → if true, a `PhysicalOrder` is inserted before the sink. ART sorts numeric keys but not `VARCHAR`. |
| `build_global_init` | execute, once | Builds the shared `IndexBuildGlobalState` (ART: holds the global index being assembled). |
| `build_local_init` | execute, per worker | Builds per-thread `IndexBuildLocalState` (ART: a thread-local index + key scratch). |
| `build_sink` | execute, per chunk per thread | The ingest. Receives `(key_chunk, row_chunk)` — indexed-column values and the matching `rowid` column. Inserts them into the local state. |
| `build_combine` | execute, per thread at end | Merge each thread-local state into the global state (ART: `MergeIndexes`). |
| `build_finalize` | execute, once | Returns the finished `unique_ptr<BoundIndex>` from the global state. |

If `create_plan` is null, all of `build_global_init`, `build_local_init`, `build_sink`,
`build_combine`, and `build_finalize` are required (called unconditionally by the operator), and
`build_bind` is required (called unconditionally by the planner). `build_sort` is optional.

After `build_finalize`, `PhysicalCreateIndex::Finalize` runs a fixed contract on the returned
index before installing it (see §3, "Finalize contract"), then creates the catalog entry
(`schema.CreateIndex`) and attaches it to storage (`DataTable::AddIndex`).

### 2b. `create_plan` — the escape hatch (and its consequence)

`create_plan` (`index_build_plan_t`) lets a type replace the entire physical subtree with its
own operators. When set, the generic pipeline and `PhysicalCreateIndex` are bypassed completely.

Consequence worth calling out: because `PhysicalCreateIndex::Finalize` is what creates the
catalog entry and calls `AddIndex`, a `create_plan` that returns e.g. a dummy scan produces an
index that **never registers** — it will not appear in `duckdb_indexes()` and is not persisted.
That is the behavior of a stub. A real index either uses the generic pipeline or performs the
catalog registration itself inside its custom plan.

### 2c. `create_instance` — instantiate from storage

`create_instance` (`index_create_function_t`) builds a `BoundIndex` from an
`IndexStorageInfo`, i.e. it materializes an already-existing index rather than building one from
table data. It receives a `CreateIndexInput`:

```cpp
struct CreateIndexInput {
    ClientContext &context;
    TableIOManager &table_io_manager;
    AttachedDatabase &db;
    IndexConstraintType constraint_type;
    const Identifier &name;
    const vector<column_t> &column_ids;
    const vector<unique_ptr<Expression>> &unbound_expressions;
    const IndexStorageInfo &storage_info;   // block pointers / allocator metadata to rehydrate from
    const case_insensitive_map_t<Value> &options;
};
```

ART's is a one-liner: construct the `ART` forwarding `storage_info`, whose constructor
deserializes from the referenced blocks. Called on load and on WAL replay (see §5). Without a
`create_instance`, an index cannot survive a reload.

Note: the index **name is an `Identifier`**, not a `string` — a case-insensitive wrapper
(`include/duckdb/common/identifier.hpp`). Construction is implicit from a `const char*` literal
(`"logsearch"` just works) but explicit from a runtime `string` (`Identifier(s)`); recover the
raw string with `.GetIdentifierName()`. This type flows through `CreateIndexInput::name`,
`BoundIndex::name`/`GetIndexName()`, and `IndexStorageInfo::name`.

---

## 3. `BoundIndex`: the live instance interface

A concrete index subclasses `BoundIndex` and implements its virtuals. Public entry points come
in a locked form (take an `IndexLock`) and an unlocked convenience form that acquires the lock
first via `InitializeLock`. Grouped by role:

**Append / insert (DML maintenance).** Called whenever committed rows are added to the table.

- `Append(IndexLock&, DataChunk&, Vector &row_ids)` — pure virtual. The primary ingest path.
- `Append(..., IndexAppendInfo&)` — variant carrying append mode and delete-index references.
- `Insert(IndexLock&, DataChunk&, Vector &row_ids[, IndexAppendInfo&])` — pure virtual.
- `VerifyAppend` / `VerifyConstraint` — constraint pre-checks (only meaningful for
  unique/primary/foreign indexes; a non-constraint index can no-op these).

**Delete.** Called on `DELETE`/`UPDATE` (update = delete + insert).

- `TryDelete(IndexLock&, DataChunk&, Vector &row_ids, deleted_sel, non_deleted_sel)` — returns
  the count actually removed; optional selection vectors report which rows were (not) deleted.
- `Delete(IndexLock&, DataChunk&, Vector &row_ids)` — throws if not all rows were deleted.

**Drop / reset storage.** *Renamed on main from `CommitDrop`.*

- `ResetStorage(IndexLock&)` — pure virtual. Frees all index storage, clearing it entirely.
- `ResetStorage()` — unlocked override of `Index::ResetStorage()`.

**Merge.** Used by parallel build (`build_combine`) and checkpoint delta merges.

- `MergeIndexes(IndexLock&, BoundIndex &other)` — pure virtual. Fold another instance in.

**Maintenance / introspection.** Several of these are invoked by the finalize contract.

- `Vacuum(IndexLock&)` — pure virtual. Reclaim excess memory.
- `GetInMemorySize(IndexLock&)` — pure virtual. Reported to the catalog as the index size.
- `Verify(IndexLock&)` — pure virtual. Structural self-check.
- `VerifyAllocations(IndexLock&)` — pure virtual. Allocation counts match node counts.
- `VerifyBuffers(IndexLock&)` — virtual, default no-op.
- `ToString(IndexLock&, bool display_ascii)` — pure virtual. Must return a **non-empty** string.
- `GetConstraintViolationMessage(VerifyExistenceType, idx_t failed_index, DataChunk&)` — pure
  virtual. Message for a constraint failure (trivial for non-constraint indexes).

**Delta indexes (optional).** `SupportsDeltaIndexes()` / `CreateDeltaIndex(DeltaIndexType)` —
default to unsupported; used by checkpointing to stage inserts/deletes against a copy.

**Serialize (persistence).** Virtual (not pure — defaults exist). Covered in §5.

- `SerializeToDisk(QueryContext, const case_insensitive_map_t<Value> &options) -> IndexStorageInfo`
- `SerializeToWAL(const case_insensitive_map_t<Value> &options) -> IndexStorageInfo`

**WAL replay.** `ApplyBufferedReplays(table_types, BufferedIndexReplays&, mapped_column_ids)` —
replays insert/delete chunks buffered during WAL replay, applied at bind time (see §5).

### Finalize contract

After a fresh build, `PhysicalCreateIndex::Finalize` calls the following on the returned index,
in order, before it is registered. A custom `BoundIndex` must survive all of them:

1. `Vacuum()`
2. `Verify()`
3. `ToString(true)` — asserted **non-empty**
4. `VerifyAllocations()`
5. `GetInMemorySize()` — recorded as `initial_index_size`

then `schema.CreateIndex(...)` + `DataTable::AddIndex(std::move(index))`.

---

## 5. Persistence: serialize and load

An index is persisted at checkpoint (to the database file) or to the WAL (between checkpoints),
and rehydrated on load. Everything hinges on one struct.

### 5a. `IndexStorageInfo`

`include/duckdb/storage/index_storage_info.hpp` — the serialized description of an index. Move-only
(copy explicitly deleted); its lifetime is managed deliberately.

```cpp
struct IndexStorageInfo {
    Identifier name;
    idx_t root;                                   // storage root handle (index-defined; ART: tree root)
    case_insensitive_map_t<Value> options;        // index-specific settings, round-tripped
    vector<FixedSizeAllocatorInfo> allocator_infos;   // allocator metadata (block pointers etc.) — disk path
    vector<vector<IndexBufferInfo>> buffers;          // raw buffer pointers + sizes — WAL path
    BlockPointer root_block_ptr;                  // legacy single-root pointer (older storage files)

    bool IsValid() const { return root_block_ptr.IsValid() || !allocator_infos.empty(); }
};
```

`IsValid()` distinguishes "there is persisted data to deserialize" from "empty index". An empty
index legitimately has neither a root block pointer nor allocator infos, so a custom index's
constructor must initialize an empty instance correctly when handed an invalid `IndexStorageInfo`
(do not assume these fields are populated).

The two payload channels matter: **`allocator_infos`** carries block pointers into the database
file (checkpoint path — the data already lives in on-disk blocks), whereas **`buffers`** carries
raw in-memory buffer pointers + sizes to be written into the WAL (WAL path — the bytes are copied
into the log).

### 5b. Checkpoint — `SerializeToDisk`

Driven from the table data writer at checkpoint:
`TableDataWriter` → `TableIndexList::SerializeToDisk` → `BoundIndex::SerializeToDisk` per index.
The returned `IndexStorageInfo` is serialized into the table's checkpoint metadata.

ART's implementation (the pattern a block-based index mirrors):

1. `PrepareSerialize` — set `info.root` and options.
2. `WritePartialBlocks` — open a `PartialBlockManager` on the index block manager
   (`table_io_manager.GetIndexBlockManager()`) with `PartialBlockType::FULL_CHECKPOINT`, ask each
   allocator to `SerializeBuffers` into it, then `FlushPartialBlocks`. This is what turns
   in-memory buffers into on-disk blocks and records their block pointers.
3. Collect each allocator's metadata via `GetInfo()` into `info.allocator_infos`.

A block-based index (e.g. one already assembled into buffer-managed blocks) writes its blocks
through the same `PartialBlockManager` / index block manager and records the resulting pointers
in `IndexStorageInfo`.

### 5c. WAL — `SerializeToWAL`

Between checkpoints, index state is logged. `WriteAheadLog` calls
`BoundIndex::SerializeToWAL`, which (ART) calls `InitSerializationToWAL` on each allocator to
gather `buffers` (raw pointers + allocation sizes) plus `allocator_infos`, and the WAL copies
those bytes into the log. On replay the buffers are restored directly rather than read from
database blocks.

Separately, DML that occurs during WAL replay before an index is bound is **buffered** on the
`UnboundIndex` (`BufferChunk` records insert/delete chunks and their ordering in
`BufferedIndexReplays`) and applied when the index is bound, via
`BoundIndex::ApplyBufferedReplays`.

### 5d. Load — `UnboundIndex` → `create_instance`

On database open, each persisted index is reconstructed as an **`UnboundIndex`**: it holds the
`CreateInfo` (name, type, columns, parsed expressions) and the deserialized `IndexStorageInfo`,
but no live index structure. It is cold and cheap.

Binding to a live `BoundIndex` is **lazy**, triggered the first time something needs the index
(a scan, a DML statement, a checkpoint). `DataTable`/`DataTableInfo::BindIndexes` →
`IndexBinder::BindIndex(UnboundIndex&)`:

1. Look up the `IndexType` by name in `config.GetIndexTypes()`.
2. Rebuild the bound expressions from the stored parsed expressions.
3. Assemble a `CreateIndexInput` — crucially carrying the `UnboundIndex`'s `IndexStorageInfo`.
4. Call `index_type.create_instance(input)` → `unique_ptr<BoundIndex>`.

The index type's constructor deserializes from the `IndexStorageInfo`: if `IsValid()`, read back
the allocator/block state (checkpoint) or restore buffers (WAL); otherwise start empty. WAL replay
uses the same `create_instance` path (`WriteAheadLogDeserializer` builds a `CreateIndexInput` and
calls it directly).

### Round-trip summary

```
build (§2a)  ──►  BoundIndex (live in memory)
                       │  checkpoint: SerializeToDisk ──► IndexStorageInfo ──► blocks + catalog metadata
                       │  wal:        SerializeToWAL  ──► IndexStorageInfo ──► WAL
                       ▼
database file / WAL
                       │  load
                       ▼
UnboundIndex (cold: CreateInfo + IndexStorageInfo)
                       │  first use: BindIndexes ─► IndexBinder::BindIndex ─► create_instance (§2c)
                       ▼
BoundIndex (live again)
```

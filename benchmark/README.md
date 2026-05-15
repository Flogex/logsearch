# logsearch benchmarks

End-to-end SQL benchmarks comparing **vanilla DuckDB** (`ILIKE` /
`regexp_matches`) against **DuckDB + the built-in `fts` extension**
(BM25 index on the `message` column, used as a boolean retrieval matcher
via `fts_main_logs.match_bm25(...) IS NOT NULL`).

> The logsearch extension under development is not benchmarked yet —
> this baseline establishes the target it has to beat. Drop in a new
> system by extending `SYSTEMS` in `run.py` and adding an SQL variant on
> every `Query` in `workloads.py`.

## What gets measured

For each `(dataset, system)` pair, in a fresh file-backed DuckDB
database:

1. The prepared parquet is loaded into a `logs` table with rows ordered
   by `ts` (`CREATE TABLE logs AS SELECT * FROM read_parquet(...) ORDER BY ts`).
2. For the `fts` system: `INSTALL fts; LOAD fts; PRAGMA
   create_fts_index('logs', 'line_no', 'message')` — the build time is
   measured and recorded as a separate CSV row (`query = 'build_index'`).
3. `CHECKPOINT` flushes the buffer manager.
4. Profiling is enabled (`SET enable_profiling='json'; SET
   profiling_mode='detailed'`).
5. Each query is executed **3 times**. Each iteration captures:
   - elapsed wall time via `time.perf_counter_ns` (sub-microsecond
     resolution; matters because most queries land sub-second);
   - a per-iteration DuckDB JSON profile under `profiles/`;
   - row count returned (sanity check).

There is **no cold-start measurement**. Each iteration runs in the same
process against the same connection — what gets measured is steady-state
query latency. Visualizations report per-(dataset, query, system) **min,
max, average** across the three iterations.

There is **no RSS sampling**. For memory diagnosis, read the per-query
profile JSONs.

## Datasets

Three LogHub datasets, all prepared by `benchmark/data/prepare.py` into
typed parquet under `data/prepared/`:

| Dataset       | Rows (approx.) | Schema                                                  |
|---------------|----------------|---------------------------------------------------------|
| `hdfs`        | ~11M           | `line_no, ts, pid, level, component, message`           |
| `windows`     | ~114M          | `line_no, ts, level, component, message`                |
| `thunderbird` | ~211M          | `line_no, ts, label, node, address, message`            |

Common columns across every dataset: `line_no BIGINT`, `ts TIMESTAMP`,
`message VARCHAR`. The other columns differ per dataset; queries that
filter on structured fields are therefore per-dataset (see
`workloads.py`).

### Attribution

HDFS_v1, Windows, and Thunderbird are released by the LogPAI / LogHub
project — https://github.com/logpai/loghub. Free for research / academic
use on the condition that the LogHub URL is cited and the relevant paper
credited: Zhu et al., *"Loghub: A Large Collection of System Log
Datasets for AI-driven Log Analytics"*.

## Queries

Each dataset defines the same 9 query categories so the visualizations
align across datasets. The exact terms differ because each log corpus
has its own characteristic vocabulary.

| Name                        | Pattern                                | FTS supported? |
|-----------------------------|-----------------------------------------|----------------|
| `q01_keyword`               | single keyword in `message`             | yes            |
| `q02_rare_keyword`          | rare keyword (stemmed in fts)           | yes            |
| `q03_and`                   | boolean AND of two keywords             | yes (`conjunctive := 1`) |
| `q04_or`                    | boolean OR of two keywords              | yes (default)  |
| `q05_not`                   | AND NOT                                 | **no**         |
| `q06_phrase`                | multi-word phrase                       | **no**         |
| `q07_wildcard`              | regex pattern (`regexp_matches`)        | **no**         |
| `q08_keyword_plus_*`        | keyword + structured equality           | yes            |
| `q09_keyword_plus_time`     | keyword + 1-hour `ts` window            | yes            |

Built-in DuckDB FTS limitations make q05–q07 vanilla-only — see *FTS
caveats* below. Unsupported cells are recorded in the CSV with empty
`elapsed_s` and surface as `N/A` annotations on the charts. We do
**not** emulate them with hybrid BM25 + post-SQL filtering — that would
measure something different.

## Output layout

```
benchmark/results/<run_id>/
├── meta.json                  # run_id, commit, host, platform, etc.
├── measurements.csv           # one row per (dataset, query, system, iter)
├── profiles/
│   ├── hdfs__q01_keyword__vanilla__iter0.json
│   ├── hdfs__q01_keyword__vanilla__iter1.json
│   ├── hdfs__q01_keyword__fts__iter0.json
│   └── ...
└── viz/
    ├── hdfs.png
    ├── windows.png
    └── thunderbird.png
```

The default `<results_dir>` is `$TMPDIR/logsearch-bench/`. The
orchestrator prints the absolute run-dir path on stdout (CI uploads it
to R2; locally you can `aws s3 cp --recursive ...`).

### CSV schema

```
run_id, ts_utc, id,
dataset, query, system, iter,
elapsed_s, row_count, profile
```

`id` is the unique row id, formatted as
`<dataset>__<query>__<system>__iter<n>` for measured runs,
`<dataset>__build_index__fts__iter0` for index-build rows, and
`<dataset>__<query>__fts__na` for unsupported cells.

`run_id` and `ts_utc` are repeated on every row so the CSV is
self-sufficient for cross-run aggregation (no join against `meta.json`
needed).

## FTS caveats to know

The built-in `fts` extension is BM25 over a tokenizer pipeline of
lowercasing + porter stemming + stop-word removal. Important
consequences:

- **No `NOT`** — only `AND` (`conjunctive := 1`) and `OR` (the default).
- **No phrase queries** — every input is treated as a bag of stemmed
  tokens.
- **No wildcards / prefix queries.** Use regex/`ILIKE` on top of the
  base rows instead.
- **Tokenizer transforms the query** — search for `fsnamesystem`, not
  `FSNamesystem`. Stop words (`the`, `a`, …) are dropped from queries
  too.
- **BM25 scores get computed even for boolean retrieval.** That scoring
  cost is included in the measured elapsed time, since it's part of
  what the extension does.

## Local quickstart

```sh
python -m venv .venv && source .venv/bin/activate
pip install -r benchmark/requirements.txt

# Prepare all three datasets (downloads ~5GB, may take a while):
python benchmark/data/prepare.py

# Run the benchmark — every dataset, every system, 3 iters per query.
# Prints the run-dir path on stdout.
python benchmark/run.py

# Or just one dataset / pin memory:
python benchmark/run.py --dataset hdfs --memory-limit 4GB
```

Useful flags:

| Flag              | Effect                                                       |
|-------------------|--------------------------------------------------------------|
| `--dataset NAME`  | Run only this dataset (repeatable).                          |
| `--results-dir D` | Override the run-dir parent. Default: `$TMPDIR/logsearch-bench`. |
| `--db-dir D`      | Keep the per-(dataset, system) DuckDB files (otherwise a tempdir cleaned up at exit). |
| `--memory-limit X`| Pin DuckDB memory cap (e.g. `4GB`). Default = DuckDB default (~80% RAM). Pin this for stable cross-host comparison. |
| `--skip-viz`      | Skip rendering PNGs.                                         |

## Visualization

`viz.py` renders one PNG per dataset: grouped bar chart with vanilla and
fts bars side by side, error bars spanning min..max across the 3
iterations. `N/A` annotations mark queries the FTS extension cannot
express. The FTS index-build time is annotated as text in the
top-right rather than plotted as a bar (it dwarfs query times by orders
of magnitude).

Re-render from an existing CSV without re-running the benchmark:

```sh
python benchmark/viz.py path/to/measurements.csv
```

## CI

`.github/workflows/benchmark.yml` runs daily at 04:00 UTC. Three
persistence paths:

1. Workflow artifact, 90-day retention (always on).
2. R2 `s3://$R2_BUCKET/results/<run_id>/` — full directory upload.
3. Cross-run trend analysis from R2 via DuckDB httpfs — see
   `viz/rolling_avg.sql`.

Repo history stays clean — only code is committed; data and results live
in R2.

### Dataset cache (R2)

The workflow lists datasets with `python benchmark/data/prepare.py
--list-datasets`. For each:

- If `s3://$R2_BUCKET/fixtures/<dataset>.parquet` exists → download it.
- Otherwise → build locally and upload the result.

The R2 fixture is **not** auto-invalidated when the schema changes. Run

```sh
aws s3 rm "s3://$R2_BUCKET/fixtures/<dataset>.parquet" \
    --endpoint-url "https://$R2_ACCOUNT_ID.r2.cloudflarestorage.com"
```

after any schema-affecting edit to `prepare.py` so the next CI run
rebuilds and re-uploads.

### R2 setup

1. Cloudflare → R2 → enable.
2. Create bucket `logsearch-bench`.
3. Create an R2 API token (Object Read & Write, scoped to the bucket).
   Save: Access Key ID, Secret Access Key, Account ID.
4. GitHub repo secrets (Settings → Secrets → Actions):
   - `R2_ACCOUNT_ID`
   - `R2_ACCESS_KEY_ID`
   - `R2_SECRET_ACCESS_KEY`
   - `R2_BUCKET` = `logsearch-bench`

## Adding a query

Append a `Query(...)` to the appropriate dataset list in
`workloads.py`. Both `vanilla_sql` and `fts_sql` must produce the same
result on the dataset schema. Set `fts_sql=None` when the built-in
extension can't express the operation.

## Adding a dataset

1. Add `fetch_<name>(parquet_path)` + a `convert_<name>(...)` in
   `data/prepare.py`. The parquet must include `line_no BIGINT`,
   `ts TIMESTAMP`, and `message VARCHAR` (other columns optional).
2. Register `"<name>"` in `DATASETS`.
3. Add `<NAME>_QUERIES: list[Query]` in `workloads.py` and register it
   in `DATASETS`.

## Adding a system

1. Extend `SYSTEMS` in `run.py`.
2. Hook the system-specific prepare path inside `run_dataset` (currently
   the `fts` branch installs the extension and builds the BM25 index).
3. Add a third SQL string on each `Query`.

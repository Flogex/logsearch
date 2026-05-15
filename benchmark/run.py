"""Benchmark orchestrator.

For each dataset declared in `workloads.DATASETS`:

  1. Open a file-backed DuckDB database and load the prepared parquet into
     a `logs` table, rows ordered by `ts`.
  2. For the `fts` system: install/load fts and build a BM25 index on the
     `message` column. The build time is captured (a CSV row with
     `query = 'build_index'`).
  3. CHECKPOINT to flush the buffer manager.
  4. Enable profiling (`profile_output` JSON per query).
  5. Run each query 3 times. Record per-iteration elapsed time
     (`time.perf_counter_ns`, sub-microsecond resolution) and persist
     the DuckDB JSON profile.

Output:

    <results_dir>/<run_id>/
        meta.json
        measurements.csv
        profiles/<dataset>__<query>__<system>__iter<n>.json
        viz/<dataset>.png

CSV row id: `<dataset>__<query>__<system>__iter<n>`.

The absolute path of `<results_dir>/<run_id>` is printed on stdout.
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import json
import os
import platform
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import duckdb

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from workloads import DATASETS, Query  # noqa: E402

ITERS = 3
SYSTEMS = ("vanilla", "fts")


def git_commit() -> str:
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=HERE.parent, text=True
        ).strip()
    except Exception:
        return "unknown"


def open_db(db_path: Path, memory_limit: str | None) -> duckdb.DuckDBPyConnection:
    if db_path.exists():
        db_path.unlink()
    con = duckdb.connect(str(db_path))
    if memory_limit is not None:
        con.execute(f"SET memory_limit = '{memory_limit}'")
    return con


def load_dataset(con: duckdb.DuckDBPyConnection, parquet_path: Path) -> None:
    # CTAS preserves the SELECT order, so this lands rows on disk sorted
    # by ts — relevant for the time-window queries that benefit from
    # row-group min/max pruning.
    con.execute(
        "CREATE OR REPLACE TABLE logs AS "
        "SELECT * FROM read_parquet(?) ORDER BY ts",
        [str(parquet_path)],
    )


def build_fts_index(con: duckdb.DuckDBPyConnection) -> float:
    con.execute("INSTALL fts")
    con.execute("LOAD fts")
    t0 = time.perf_counter_ns()
    con.execute("PRAGMA create_fts_index('logs', 'line_no', 'message')")
    return (time.perf_counter_ns() - t0) / 1e9


def configure_profiling(con: duckdb.DuckDBPyConnection) -> None:
    con.execute("SET enable_profiling = 'json'")
    con.execute("SET profiling_mode = 'detailed'")


def run_query(
    con: duckdb.DuckDBPyConnection, sql: str, profile_path: Path
) -> tuple[float, int]:
    con.execute(f"PRAGMA profile_output = '{profile_path}'")
    t0 = time.perf_counter_ns()
    rows = con.execute(sql).fetchall()
    elapsed_s = (time.perf_counter_ns() - t0) / 1e9
    return elapsed_s, len(rows)


def write_row(writer: csv.DictWriter, **kwargs) -> None:
    writer.writerow(kwargs)


def run_dataset(
    dataset: str,
    parquet: Path,
    queries: list[Query],
    db_dir: Path,
    profiles_dir: Path,
    writer: csv.DictWriter,
    common: dict,
    memory_limit: str | None,
) -> None:
    for system in SYSTEMS:
        db_path = db_dir / f"{dataset}-{system}.duckdb"
        print(f"[load] {dataset} / {system}", file=sys.stderr)
        con = open_db(db_path, memory_limit)
        try:
            load_dataset(con, parquet)

            index_build_s: float | None = None
            if system == "fts":
                print(f"[idx ] {dataset} / fts build_index", file=sys.stderr)
                index_build_s = build_fts_index(con)
                print(
                    f"[idx ] {dataset} / fts build_index = {index_build_s:.2f}s",
                    file=sys.stderr,
                )

            con.execute("CHECKPOINT")
            configure_profiling(con)

            if index_build_s is not None:
                write_row(
                    writer, **common,
                    id=f"{dataset}__build_index__{system}__iter0",
                    dataset=dataset, query="build_index", system=system,
                    iter=0, elapsed_s=f"{index_build_s:.6f}",
                    row_count="", profile="",
                )

            for q in queries:
                sql = q.fts_sql if system == "fts" else q.vanilla_sql
                if sql is None:
                    write_row(
                        writer, **common,
                        id=f"{dataset}__{q.name}__{system}__na",
                        dataset=dataset, query=q.name, system=system,
                        iter="", elapsed_s="", row_count="", profile="",
                    )
                    print(
                        f"[skip] {dataset} / {q.name} / {system} (not supported)",
                        file=sys.stderr,
                    )
                    continue

                for i in range(ITERS):
                    profile = profiles_dir / f"{dataset}__{q.name}__{system}__iter{i}.json"
                    elapsed_s, row_count = run_query(con, sql, profile)
                    write_row(
                        writer, **common,
                        id=f"{dataset}__{q.name}__{system}__iter{i}",
                        dataset=dataset, query=q.name, system=system,
                        iter=i, elapsed_s=f"{elapsed_s:.6f}",
                        row_count=row_count, profile=profile.name,
                    )
                    print(
                        f"[run ] {dataset} / {q.name} / {system} / iter{i} "
                        f"= {elapsed_s * 1000:.2f}ms",
                        file=sys.stderr,
                    )
        finally:
            con.close()


def main() -> int:
    ap = argparse.ArgumentParser(
        description=(
            "Run logsearch benchmarks: vanilla DuckDB (ILIKE / regexp) vs "
            "DuckDB + the built-in `fts` extension (BM25 index on `message`). "
            "Produces a results directory with measurements.csv, per-query "
            "DuckDB JSON profiles, meta.json, and per-dataset visualizations."
        ),
    )
    ap.add_argument(
        "--data-dir",
        type=Path,
        default=HERE / "data" / "prepared",
        help="Directory holding <dataset>.parquet files. Default: %(default)s",
    )
    ap.add_argument(
        "--dataset",
        action="append",
        choices=list(DATASETS.keys()),
        default=None,
        help="Run only this dataset (repeatable). Default: all of them.",
    )
    ap.add_argument(
        "--results-dir",
        type=Path,
        default=Path(tempfile.gettempdir()) / "logsearch-bench",
        help="Parent directory for <run_id>. Default: %(default)s",
    )
    ap.add_argument(
        "--db-dir",
        type=Path,
        default=None,
        help=(
            "Directory in which per-(dataset, system) DuckDB files are "
            "materialized. Default: a tempdir cleaned up at exit."
        ),
    )
    ap.add_argument(
        "--memory-limit",
        default=None,
        help=(
            "Value passed to `SET memory_limit`. Unset = DuckDB default "
            "(~80%% of RAM). Pin this (e.g. '4GB') for stable comparison "
            "across hosts."
        ),
    )
    ap.add_argument(
        "--skip-viz",
        action="store_true",
        help="Skip the matplotlib visualization step.",
    )
    args = ap.parse_args()

    targets = args.dataset or list(DATASETS.keys())
    missing = [
        ds for ds in targets
        if not (args.data_dir / f"{ds}.parquet").exists()
    ]
    if missing:
        print(
            f"missing parquet: {missing}\nrun: python benchmark/data/prepare.py",
            file=sys.stderr,
        )
        return 2

    run_id = dt.datetime.now(dt.UTC).strftime("%Y%m%dT%H%M%SZ")
    run_dir = (args.results_dir / run_id).resolve()
    profiles_dir = run_dir / "profiles"
    profiles_dir.mkdir(parents=True, exist_ok=True)

    meta = {
        "run_id": run_id,
        "ts_utc": dt.datetime.now(dt.UTC).isoformat(),
        "commit": git_commit(),
        "host": socket.gethostname(),
        "platform": platform.platform(),
        "python": platform.python_version(),
        "cpu_count": os.cpu_count(),
        "iters_per_query": ITERS,
        "systems": list(SYSTEMS),
        "datasets": targets,
        "memory_limit": args.memory_limit,
    }
    (run_dir / "meta.json").write_text(json.dumps(meta, indent=2))

    csv_path = run_dir / "measurements.csv"
    # `run_id` and `ts_utc` are repeated on every row so the CSV is
    # self-sufficient for cross-run aggregation (no join against meta.json
    # needed). Same shape as a SQL fact table.
    fieldnames = [
        "run_id", "ts_utc", "id",
        "dataset", "query", "system", "iter",
        "elapsed_s", "row_count", "profile",
    ]
    common = {"run_id": meta["run_id"], "ts_utc": meta["ts_utc"]}

    db_root: Path
    cleanup_db: tempfile.TemporaryDirectory | None = None
    if args.db_dir is not None:
        args.db_dir.mkdir(parents=True, exist_ok=True)
        db_root = args.db_dir
    else:
        cleanup_db = tempfile.TemporaryDirectory(prefix="logsearch-bench-db-")
        db_root = Path(cleanup_db.name)

    try:
        with open(csv_path, "w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=fieldnames)
            writer.writeheader()
            for ds in targets:
                parquet = args.data_dir / f"{ds}.parquet"
                run_dataset(
                    ds, parquet, DATASETS[ds], db_root, profiles_dir,
                    writer, common, args.memory_limit,
                )
                f.flush()
    finally:
        if cleanup_db is not None:
            cleanup_db.cleanup()

    if not args.skip_viz:
        from viz import render_all
        render_all(csv_path, run_dir / "viz")

    print(f"results written to: {run_dir}", file=sys.stderr)
    print(run_dir)
    return 0


if __name__ == "__main__":
    sys.exit(main())

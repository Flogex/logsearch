"""Render one PNG per dataset summarizing a benchmark run.

Each PNG is a grouped bar chart:
    x  = query name
    y  = average elapsed time across iterations (ms)
    bars: vanilla (gray) and fts (blue), side by side
    error bars: min..max across iterations
    N/A annotation when a query is not supported by fts

The fts index-build time is recorded in `measurements.csv` as a row with
`query = 'build_index'`. It's plotted as text in the top-right corner
of each chart rather than as a bar — it dwarfs query times by orders of
magnitude and would compress the rest of the chart.
"""

from __future__ import annotations

import argparse
import csv
from collections import defaultdict
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402


def _aggregate(csv_path: Path):
    """Return (queries, index_build) where:
        queries[dataset][query][system] = [elapsed_s, ...]
        index_build[dataset][system]    = elapsed_s
    """
    queries: dict = defaultdict(lambda: defaultdict(lambda: defaultdict(list)))
    index_build: dict = defaultdict(dict)
    with open(csv_path) as f:
        for row in csv.DictReader(f):
            if not row["elapsed_s"]:
                continue
            elapsed = float(row["elapsed_s"])
            if row["query"] == "build_index":
                index_build[row["dataset"]][row["system"]] = elapsed
            else:
                queries[row["dataset"]][row["query"]][row["system"]].append(elapsed)
    return queries, index_build


def _render_dataset(
    dataset: str, qmap: dict, index_build: dict, out_path: Path
) -> None:
    query_names = sorted(qmap.keys())
    systems = ["vanilla", "fts"]
    colors = {"vanilla": "#888888", "fts": "#1f77b4"}

    width = 0.38
    fig, ax = plt.subplots(figsize=(max(8.0, len(query_names) * 1.1), 5.0))
    x_indices = list(range(len(query_names)))

    for i, system in enumerate(systems):
        means: list[float] = []
        err_low: list[float] = []
        err_high: list[float] = []
        present: list[bool] = []
        for q in query_names:
            runs = qmap[q].get(system, [])
            if runs:
                mean = sum(runs) / len(runs)
                means.append(mean * 1000)
                err_low.append((mean - min(runs)) * 1000)
                err_high.append((max(runs) - mean) * 1000)
                present.append(True)
            else:
                means.append(0.0)
                err_low.append(0.0)
                err_high.append(0.0)
                present.append(False)

        offset = (i - 0.5) * width
        positions = [x + offset for x in x_indices]
        ax.bar(
            positions, means, width,
            yerr=[err_low, err_high], capsize=3,
            label=system, color=colors[system],
        )
        if system == "fts":
            for xi, ok in zip(positions, present):
                if not ok:
                    ax.text(
                        xi, ax.get_ylim()[1] * 0.02 if ax.get_ylim()[1] else 0,
                        "N/A", ha="center", va="bottom",
                        fontsize=8, color="#666",
                    )

    ax.set_xticks(x_indices)
    ax.set_xticklabels(query_names, rotation=30, ha="right")
    ax.set_ylabel("avg elapsed (ms) — error bars span min..max")
    ax.set_title(f"{dataset} — vanilla DuckDB vs DuckDB FTS")
    ax.legend(loc="upper left")
    ax.grid(axis="y", linestyle=":", alpha=0.5)

    fts_build = index_build.get("fts")
    note = (
        f"FTS index build: {fts_build:.2f}s"
        if fts_build is not None
        else "FTS index build: (not recorded)"
    )
    ax.text(
        0.99, 0.97, note,
        transform=ax.transAxes, ha="right", va="top", fontsize=9, color="#333",
        bbox={"boxstyle": "round,pad=0.3", "facecolor": "white", "edgecolor": "#ccc"},
    )

    fig.tight_layout()
    fig.savefig(out_path, dpi=140)
    plt.close(fig)


def render_all(csv_path: Path, out_dir: Path) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    queries, index_build = _aggregate(csv_path)
    for dataset, qmap in queries.items():
        out_path = out_dir / f"{dataset}.png"
        _render_dataset(dataset, qmap, index_build.get(dataset, {}), out_path)
        print(f"[viz ] wrote {out_path}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("csv", type=Path, help="path to measurements.csv")
    ap.add_argument(
        "--out", type=Path, default=None,
        help="output directory (default: <csv parent>/viz)",
    )
    args = ap.parse_args()
    out = args.out or args.csv.parent / "viz"
    render_all(args.csv, out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

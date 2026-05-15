"""Benchmark queries per dataset.

Each `Query` is a pair (vanilla, fts) of SQL strings that run against the
same `logs` table inside a per-(dataset, system) DuckDB file. Schemas of
`logs` differ per dataset — see `benchmark/data/prepare.py` — but every
dataset has `line_no BIGINT`, `ts TIMESTAMP`, and `message VARCHAR`,
which is what the queries lean on.

`fts_sql=None` means the built-in DuckDB `fts` extension can't express
the operation natively (no phrase, no NOT, no wildcards / regex). The
runner skips those (dataset, query, fts) cells and records them as
N/A in the CSV. We intentionally do NOT emulate via hybrid BM25 +
post-filter SQL — that would measure something else.

Query names (`q##_<category>`) are stable across datasets so results
align by name in the visualization.
"""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class Query:
    name: str
    description: str
    vanilla_sql: str
    fts_sql: str | None


def _v(term: str) -> str:
    return f"message ILIKE '%{term}%'"


def _fts(expr: str, conjunctive: int = 0) -> str:
    extra = f", conjunctive := {conjunctive}" if conjunctive else ""
    return f"fts_main_logs.match_bm25(line_no, '{expr}'{extra}) IS NOT NULL"


# Time-window predicates use a scalar subquery on min(ts) so the same SQL
# works for every dataset (HDFS=2008, Windows=2016, Thunderbird=2005).
# DuckDB evaluates the scalar subquery once and folds it; the cost shows
# up in vanilla and fts equally, so the relative comparison is fair.
def _first_hour(extra_predicate: str) -> str:
    return (
        "SELECT count(*) FROM logs "
        "WHERE ts >= (SELECT min(ts) FROM logs) "
        "AND ts <  (SELECT min(ts) FROM logs) + INTERVAL 1 HOUR "
        f"AND {extra_predicate}"
    )


# ----- HDFS (schema: line_no, ts, pid, level, component, message) -----

HDFS_QUERIES: list[Query] = [
    Query(
        name="q01_keyword",
        description="single keyword in message body",
        vanilla_sql=f"SELECT count(*) FROM logs WHERE {_v('block')}",
        fts_sql=f"SELECT count(*) FROM logs WHERE {_fts('block')}",
    ),
    Query(
        name="q02_rare_keyword",
        description="rare keyword (porter-stemmed in fts)",
        vanilla_sql=f"SELECT count(*) FROM logs WHERE {_v('fsnamesystem')}",
        fts_sql=f"SELECT count(*) FROM logs WHERE {_fts('fsnamesystem')}",
    ),
    Query(
        name="q03_and",
        description="boolean AND: 'block' AND 'received'",
        vanilla_sql=f"SELECT count(*) FROM logs WHERE {_v('block')} AND {_v('received')}",
        fts_sql=f"SELECT count(*) FROM logs WHERE {_fts('block received', conjunctive=1)}",
    ),
    Query(
        name="q04_or",
        description="boolean OR: 'block' OR 'exception'",
        vanilla_sql=f"SELECT count(*) FROM logs WHERE {_v('block')} OR {_v('exception')}",
        fts_sql=f"SELECT count(*) FROM logs WHERE {_fts('block exception')}",
    ),
    Query(
        name="q05_not",
        description="NOT: 'block' AND NOT 'exception' (fts: N/A)",
        vanilla_sql=f"SELECT count(*) FROM logs WHERE {_v('block')} AND NOT {_v('exception')}",
        fts_sql=None,
    ),
    Query(
        name="q06_phrase",
        description="phrase: 'Received block' (fts: N/A)",
        vanilla_sql="SELECT count(*) FROM logs WHERE message ILIKE '%Received block%'",
        fts_sql=None,
    ),
    Query(
        name="q07_wildcard",
        description="regex: 'blk_-?[0-9]+' (fts: N/A)",
        vanilla_sql="SELECT count(*) FROM logs WHERE regexp_matches(message, 'blk_-?[0-9]+')",
        fts_sql=None,
    ),
    Query(
        name="q08_keyword_plus_level",
        description="'block' AND level = 'ERROR'",
        vanilla_sql=f"SELECT count(*) FROM logs WHERE level = 'ERROR' AND {_v('block')}",
        fts_sql=f"SELECT count(*) FROM logs WHERE level = 'ERROR' AND {_fts('block')}",
    ),
    Query(
        name="q09_keyword_plus_time",
        description="'block' in the first-hour window of the dataset",
        vanilla_sql=_first_hour(_v("block")),
        fts_sql=_first_hour(_fts("block")),
    ),
]


# ----- Windows (schema: line_no, ts, level, component, message) -----

WINDOWS_QUERIES: list[Query] = [
    Query(
        name="q01_keyword",
        description="single keyword in message body",
        vanilla_sql=f"SELECT count(*) FROM logs WHERE {_v('package')}",
        fts_sql=f"SELECT count(*) FROM logs WHERE {_fts('package')}",
    ),
    Query(
        name="q02_rare_keyword",
        description="rare keyword (porter-stemmed in fts)",
        vanilla_sql=f"SELECT count(*) FROM logs WHERE {_v('superseded')}",
        fts_sql=f"SELECT count(*) FROM logs WHERE {_fts('superseded')}",
    ),
    Query(
        name="q03_and",
        description="boolean AND: 'package' AND 'installed'",
        vanilla_sql=f"SELECT count(*) FROM logs WHERE {_v('package')} AND {_v('installed')}",
        fts_sql=f"SELECT count(*) FROM logs WHERE {_fts('package installed', conjunctive=1)}",
    ),
    Query(
        name="q04_or",
        description="boolean OR: 'failed' OR 'error'",
        vanilla_sql=f"SELECT count(*) FROM logs WHERE {_v('failed')} OR {_v('error')}",
        fts_sql=f"SELECT count(*) FROM logs WHERE {_fts('failed error')}",
    ),
    Query(
        name="q05_not",
        description="NOT: 'package' AND NOT 'failed' (fts: N/A)",
        vanilla_sql=f"SELECT count(*) FROM logs WHERE {_v('package')} AND NOT {_v('failed')}",
        fts_sql=None,
    ),
    Query(
        name="q06_phrase",
        description="phrase: 'Loaded Servicing Stack' (fts: N/A)",
        vanilla_sql="SELECT count(*) FROM logs WHERE message ILIKE '%Loaded Servicing Stack%'",
        fts_sql=None,
    ),
    Query(
        name="q07_wildcard",
        description="regex: 'KB[0-9]+' (fts: N/A)",
        vanilla_sql="SELECT count(*) FROM logs WHERE regexp_matches(message, 'KB[0-9]+')",
        fts_sql=None,
    ),
    Query(
        name="q08_keyword_plus_level",
        description="'package' AND level = 'Error'",
        vanilla_sql=f"SELECT count(*) FROM logs WHERE level = 'Error' AND {_v('package')}",
        fts_sql=f"SELECT count(*) FROM logs WHERE level = 'Error' AND {_fts('package')}",
    ),
    Query(
        name="q09_keyword_plus_time",
        description="'package' in the first-hour window of the dataset",
        vanilla_sql=_first_hour(_v("package")),
        fts_sql=_first_hour(_fts("package")),
    ),
]


# ----- Thunderbird (schema: line_no, ts, label, node, address, message) -----

THUNDERBIRD_QUERIES: list[Query] = [
    Query(
        name="q01_keyword",
        description="single keyword in message body",
        vanilla_sql=f"SELECT count(*) FROM logs WHERE {_v('kernel')}",
        fts_sql=f"SELECT count(*) FROM logs WHERE {_fts('kernel')}",
    ),
    Query(
        name="q02_rare_keyword",
        description="rare keyword (porter-stemmed in fts)",
        vanilla_sql=f"SELECT count(*) FROM logs WHERE {_v('segfault')}",
        fts_sql=f"SELECT count(*) FROM logs WHERE {_fts('segfault')}",
    ),
    Query(
        name="q03_and",
        description="boolean AND: 'kernel' AND 'error'",
        vanilla_sql=f"SELECT count(*) FROM logs WHERE {_v('kernel')} AND {_v('error')}",
        fts_sql=f"SELECT count(*) FROM logs WHERE {_fts('kernel error', conjunctive=1)}",
    ),
    Query(
        name="q04_or",
        description="boolean OR: 'error' OR 'fail'",
        vanilla_sql=f"SELECT count(*) FROM logs WHERE {_v('error')} OR {_v('fail')}",
        fts_sql=f"SELECT count(*) FROM logs WHERE {_fts('error fail')}",
    ),
    Query(
        name="q05_not",
        description="NOT: 'kernel' AND NOT 'error' (fts: N/A)",
        vanilla_sql=f"SELECT count(*) FROM logs WHERE {_v('kernel')} AND NOT {_v('error')}",
        fts_sql=None,
    ),
    Query(
        name="q06_phrase",
        description="phrase: 'unable to allocate' (fts: N/A)",
        vanilla_sql="SELECT count(*) FROM logs WHERE message ILIKE '%unable to allocate%'",
        fts_sql=None,
    ),
    Query(
        name="q07_wildcard",
        description="regex: 'pid [0-9]+' (fts: N/A)",
        vanilla_sql="SELECT count(*) FROM logs WHERE regexp_matches(message, 'pid [0-9]+')",
        fts_sql=None,
    ),
    Query(
        name="q08_keyword_plus_label",
        description="'kernel' AND label != '-' (alert-tagged lines only)",
        vanilla_sql=f"SELECT count(*) FROM logs WHERE label != '-' AND {_v('kernel')}",
        fts_sql=f"SELECT count(*) FROM logs WHERE label != '-' AND {_fts('kernel')}",
    ),
    Query(
        name="q09_keyword_plus_time",
        description="'kernel' in the first-hour window of the dataset",
        vanilla_sql=_first_hour(_v("kernel")),
        fts_sql=_first_hour(_fts("kernel")),
    ),
]


DATASETS: dict[str, list[Query]] = {
    "hdfs": HDFS_QUERIES,
    "windows": WINDOWS_QUERIES,
    "thunderbird": THUNDERBIRD_QUERIES,
}

"""Download + prepare benchmark datasets.

One `fetch_<dataset>()` per dataset, each self-contained. `main` picks
the function from `--dataset`. Add a new dataset by writing one more
`fetch_*` and extending the if/else.

Output: data/prepared/<dataset>.parquet
        Schema is dataset-specific (each `<dataset>_log_to_parquet`
        defines its own columns). Common: every dataset has
        `line_no BIGINT` and `message VARCHAR`; structured fields
        (timestamp, level, component, …) vary.

Attribution: HDFS_v1 (and other LogHub datasets) are released by the
LogPAI / LogHub project. See https://github.com/logpai/loghub. Cite:
"Loghub: A Large Collection of System Log Datasets for AI-driven Log
Analytics" where applicable.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import tarfile
import urllib.request
import zipfile
from pathlib import Path

import duckdb

HERE = Path(__file__).resolve().parent
RAW = HERE / "raw"
PREPARED = HERE / "prepared"


# ---------------------------------------------------------------------- utils

def http_get(url: str, dest: Path) -> None:
    """Stream `url` to `dest`. No-op if `dest` already exists."""
    if dest.exists():
        print(f"[skip] already downloaded: {dest}", file=sys.stderr)
        return
    dest.parent.mkdir(parents=True, exist_ok=True)
    print(f"[get]  {url} -> {dest}", file=sys.stderr)
    with urllib.request.urlopen(url) as r, open(dest, "wb") as f:  # noqa: S310
        while chunk := r.read(1 << 20):
            f.write(chunk)


def unzip_member(archive: Path, member: str, dest: Path) -> None:
    if dest.exists():
        print(f"[skip] already extracted: {dest}", file=sys.stderr)
        return
    print(f"[ext]  {archive} :: {member} -> {dest}", file=sys.stderr)
    dest.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(archive) as zf, zf.open(member) as src, open(dest, "wb") as out:
        while chunk := src.read(1 << 20):
            out.write(chunk)


def untar_member(archive: Path, member: str, dest: Path) -> None:
    """Stream a single file out of a .tar.gz.

    `member` matches by exact name or basename — LogHub archives sometimes
    wrap the log in a top-level directory (e.g. `Thunderbird/Thunderbird.log`)
    and sometimes don't, so accept either layout.
    """
    if dest.exists():
        print(f"[skip] already extracted: {dest}", file=sys.stderr)
        return
    print(f"[ext]  {archive} :: {member} -> {dest}", file=sys.stderr)
    dest.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive, "r:*") as tf:
        info = None
        for ti in tf:
            if ti.name == member or Path(ti.name).name == member:
                info = ti
                break
        if info is None:
            raise FileNotFoundError(f"{member!r} not found inside {archive}")
        src = tf.extractfile(info)
        if src is None:
            raise RuntimeError(f"{member!r} in {archive} is not a regular file")
        with src, open(dest, "wb") as out:
            shutil.copyfileobj(src, out, length=1 << 20)


def parse_with_perl(input_path: Path, csv_path: Path, script: str) -> None:
    """Stream `input_path` through a perl one-liner and write CSV to `csv_path`.

    Why a shell pipeline instead of DuckDB's `regexp_extract`: at ~30GB of
    Thunderbird logs the SQL approach materialises every raw line in the
    engine's memory model. A line-at-a-time perl pass keeps memory flat and
    decouples parsing (whose failures we want to surface loudly) from the
    columnar write step (which is then a straight typed CSV → Parquet copy).

    `script` reads STDIN and writes CSV to STDOUT. It MUST `die` on any line
    it can't parse — we surface that as a RuntimeError carrying perl's stderr
    message (the raw CalledProcessError repr would dump the entire one-liner
    into the traceback, which buries the actual parse-failure line).
    """
    if csv_path.exists():
        print(f"[skip] already parsed: {csv_path}", file=sys.stderr)
        return
    csv_path.parent.mkdir(parents=True, exist_ok=True)
    print(f"[csv]  {input_path} -> {csv_path}", file=sys.stderr)
    with open(input_path, "rb") as src, open(csv_path, "wb") as out:
        proc = subprocess.run(
            ["perl", "-e", script],
            stdin=src,
            stdout=out,
            stderr=subprocess.PIPE,
        )
    if proc.returncode != 0:
        msg = proc.stderr.decode(errors="replace").strip()
        raise RuntimeError(msg or f"perl exited with status {proc.returncode}")


def convert_hdfs(input_path: Path, output_path: Path) -> None:
    """Parse HDFS_v1 log lines into a Parquet file.

    HDFS_v1 format (example):
        081109 203536 153 INFO dfs.DataNode$DataXceiver: <body>

    Output schema:
        line_no    BIGINT
        ts         TIMESTAMP   -- date + time, e.g. 2008-11-09 20:35:36
        pid        INTEGER     -- thread / process id
        level      VARCHAR     -- INFO / WARN / ERROR / ...
        component  VARCHAR     -- Java logger / class name
        message    VARCHAR     -- everything after the ': ' separator
    """
    output_path.parent.mkdir(parents=True, exist_ok=True)
    con = duckdb.connect()
    con.execute(
        r"""
        COPY (
            WITH raw AS (
                SELECT row_number() OVER () AS line_no, line
                FROM read_csv($1, columns={'line': 'VARCHAR'}, delim=NULL,
                              header=false, quote='')
            ),
            parsed AS (
                SELECT line_no,
                       regexp_extract(
                           line,
                           '^(\d{6}) (\d{6}) (\d+) (\w+) ([^:\s]+): (.*)$',
                           ['date', 'time', 'pid', 'level', 'component', 'message']
                       ) AS parts
                FROM raw
            )
            SELECT
                line_no,
                strptime('20' || parts.date || ' ' || parts.time, '%Y%m%d %H%M%S') AS ts,
                TRY_CAST(parts.pid AS INTEGER) AS pid,
                parts.level     AS level,
                parts.component AS component,
                parts.message   AS message,
            FROM parsed
        ) TO $2 (FORMAT PARQUET, PARQUET_VERSION 'V2', COMPRESSION ZSTD, COMPRESSION_LEVEL 5)
        """,
        [str(input_path), str(output_path)],
    )

    bad, total = con.execute(
        """
        SELECT count(*) FILTER (WHERE ts IS NULL), count(*)
        FROM read_parquet(?)
        """,
        [str(output_path)],
    ).fetchone()
    con.close()

    print(f"Created Parquet file for HDFS_v1 at {output_path}", file=sys.stderr)
    print(f"[stat] {bad}/{total} lines failed to parse", file=sys.stderr)

def fetch_hdfs(parquet_path: Path) -> None:
    """LogHub HDFS_v1 (~1.5GB uncompressed, ~11M lines).

    Source: https://github.com/logpai/loghub
    Mirror: Zenodo record 8196385
    Cite: Zhu et al., "Loghub: A Large Collection of System Log Datasets
          for AI-driven Log Analytics".
    """
    url = "https://zenodo.org/records/8196385/files/HDFS_v1.zip"
    archive = RAW / "HDFS_v1.zip"
    log = RAW / "HDFS.log"
    http_get(url, archive)
    unzip_member(archive, "HDFS.log", log)
    convert_hdfs(log, parquet_path)


WINDOWS_PERL = r"""
use strict;
use warnings;
while (my $line = <STDIN>) {
    chomp $line;
    $line =~ s/\r$//;
    if ($. == 1) { $line =~ s/^\xef\xbb\xbf//; }
    next if $line eq "";
    if ($line =~ /^(\d{4}-\d{2}-\d{2}) (\d{2}:\d{2}:\d{2}), (\S+)\s+(\S+)\s+(.*)$/) {
        my ($d, $t, $lvl, $comp, $msg) = ($1, $2, $3, $4, $5);
        for ($lvl, $comp, $msg) { s/"/""/g; }
        print qq($.,"$d $t","$lvl","$comp","$msg"\n);
    } else {
        # Continuation / payload line emitted by the prior record (e.g. CSI
        # perf trace data immediately after "...CSI perf trace:"). Keep the
        # raw line in `message`, NULL the structured columns. Unquoted empty
        # fields become SQL NULL via read_csv defaults.
        my $msg = $line;
        $msg =~ s/"/""/g;
        print qq($.,,,,"$msg"\n);
    }
}
"""


def convert_windows(input_path: Path, output_path: Path) -> None:
    """Parse Windows CBS log lines into a Parquet file.

    Windows format (example):
        2016-09-28 04:30:30, Info                  CBS    Loaded Servicing Stack ...

    Layout: ISO date + time, comma, padded level (Info/Warning/Error), padded
    component (CBS/CSI/DPX/…), then free-form message.

    Pipeline: perl extracts fields per line and emits a 5-column CSV; DuckDB
    only types the columns and writes Parquet. Any unparseable line aborts
    the perl pass with an error.

    Output schema:
        line_no    BIGINT
        ts         TIMESTAMP
        level      VARCHAR
        component  VARCHAR
        message    VARCHAR
    """
    csv_path = input_path.with_suffix(".csv")
    parse_with_perl(input_path, csv_path, WINDOWS_PERL)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    con = duckdb.connect()
    con.execute(
        """
        COPY (
            SELECT
                line_no,
                CAST(ts_str AS TIMESTAMP) AS ts,
                level,
                component,
                message,
            FROM read_csv($1,
                columns={
                    'line_no': 'BIGINT',
                    'ts_str': 'VARCHAR',
                    'level': 'VARCHAR',
                    'component': 'VARCHAR',
                    'message': 'VARCHAR'
                },
                header=false)
        ) TO $2 (FORMAT PARQUET, PARQUET_VERSION 'V2', COMPRESSION ZSTD, COMPRESSION_LEVEL 5)
        """,
        [str(csv_path), str(output_path)],
    )
    con.close()

    print(f"Created Parquet file for Windows at {output_path}", file=sys.stderr)


def fetch_windows(parquet_path: Path) -> None:
    """LogHub Windows (~27GB uncompressed, ~114M lines).

    Source: https://github.com/logpai/loghub
    Mirror: Zenodo record 8196385
    Cite: Zhu et al., "Loghub: A Large Collection of System Log Datasets
          for AI-driven Log Analytics".
    """
    url = "https://zenodo.org/records/8196385/files/Windows.tar.gz"
    archive = RAW / "Windows.tar.gz"
    log = RAW / "Windows.log"
    http_get(url, archive)
    untar_member(archive, "Windows.log", log)
    convert_windows(log, parquet_path)


THUNDERBIRD_PERL = r"""
use strict;
use warnings;
use Encode qw(decode_utf8 encode_utf8);
while (my $line = <STDIN>) {
    chomp $line;
    $line =~ s/\r$//;
    if ($. == 1) { $line =~ s/^\xef\xbb\xbf//; }
    if ($line =~ /^(\S+) (\d+) \d{4}\.\d{2}\.\d{2} (\S+) \w{3} \d{1,2} \d{2}:\d{2}:\d{2} (\S+)(?: (.*))?$/) {
        my ($label, $epoch, $node, $addr) = ($1, $2, $3, $4);
        my $msg = defined($5) ? $5 : "";
        # Some Thunderbird messages contain stray non-UTF-8 bytes (e.g. line
        # 69248721 has ": (\x80"). Replace invalid sequences with U+FFFD so
        # the resulting CSV is strict UTF-8 and DuckDB doesn't reject it.
        for ($label, $node, $addr, $msg) { $_ = encode_utf8(decode_utf8($_)); s/"/""/g; }
        print qq($.,$epoch,"$label","$node","$addr","$msg"\n);
    } else {
        die "Thunderbird parse failed at line $.: $line\n";
    }
}
"""


def convert_thunderbird(input_path: Path, output_path: Path) -> None:
    """Parse Thunderbird supercomputer log lines into a Parquet file.

    Thunderbird format (LogHub template, example):
        - 1131566461 2005.11.09 dn228 Nov 9 12:01:01 dn228/dn228 ...

    Output schema:
        line_no    BIGINT
        ts         TIMESTAMP
        label      VARCHAR   -- '-' for normal, otherwise an alert tag
        node       VARCHAR   -- admin node (e.g. dn228)
        address    VARCHAR   -- admin address (e.g. dn228/dn228)
        message    VARCHAR   -- everything after the address
    """
    csv_path = input_path.with_suffix(".csv")
    parse_with_perl(input_path, csv_path, THUNDERBIRD_PERL)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    con = duckdb.connect()
    con.execute(
        """
        COPY (
            SELECT
                line_no,
                CAST(to_timestamp(epoch) AS TIMESTAMP) AS ts,
                label,
                node,
                address,
                message,
            FROM read_csv($1,
                columns={
                    'line_no': 'BIGINT',
                    'epoch': 'BIGINT',
                    'label': 'VARCHAR',
                    'node': 'VARCHAR',
                    'address': 'VARCHAR',
                    'message': 'VARCHAR'
                },
                header=false)
        ) TO $2 (FORMAT PARQUET, PARQUET_VERSION 'V2', COMPRESSION ZSTD, COMPRESSION_LEVEL 5)
        """,
        [str(csv_path), str(output_path)],
    )
    con.close()

    print(f"Created Parquet file for Thunderbird at {output_path}", file=sys.stderr)


def fetch_thunderbird(parquet_path: Path) -> None:
    """LogHub Thunderbird (~30GB raw, ~211M lines).

    Source: https://github.com/logpai/loghub
    Mirror: Zenodo record 8196385
    Cite: Zhu et al., "Loghub: A Large Collection of System Log Datasets
          for AI-driven Log Analytics".
    """
    url = "https://zenodo.org/records/8196385/files/Thunderbird.tar.gz"
    archive = RAW / "Thunderbird.tar.gz"
    log = RAW / "Thunderbird.log"
    http_get(url, archive)
    untar_member(archive, "Thunderbird.log", log)
    convert_thunderbird(log, parquet_path)


# Add more datasets here by writing one more fetch_<name>() and registering
# it in DATASETS / prepare_one() below. `--list-datasets` reads DATASETS,
# so anything you add appears in CI automatically.


DATASETS = ("hdfs", "windows", "thunderbird")


# ----------------------------------------------------------------- entry

def prepare_one(name: str, force: bool) -> Path:
    parquet_path = PREPARED / f"{name}.parquet"
    if parquet_path.exists() and not force:
        print(f"[skip] already prepared: {parquet_path}", file=sys.stderr)
        return parquet_path

    if name == "hdfs":
        fetch_hdfs(parquet_path)
    elif name == "windows":
        fetch_windows(parquet_path)
    elif name == "thunderbird":
        fetch_thunderbird(parquet_path)
    else:
        # unreachable: argparse choices guards this
        raise SystemExit(f"no fetcher wired up for {name}")

    return parquet_path


def main() -> int:
    ap = argparse.ArgumentParser(
        description=(
            "Download + prepare benchmark datasets. Writes "
            "data/prepared/<dataset>.parquet. Schema is dataset-specific "
            "— see each `<dataset>_log_to_parquet` function for details."
        ),
    )
    ap.add_argument(
        "--dataset",
        choices=DATASETS,
        default=None,
        help=(
            "Dataset to prepare. If unset, prepare every dataset in "
            f"{DATASETS}."
        ),
    )
    ap.add_argument(
        "--force",
        action="store_true",
        help="Rebuild the parquet even if it already exists.",
    )
    ap.add_argument(
        "--list-datasets",
        action="store_true",
        help=(
            "Print one dataset name per line and exit. Canonical source "
            "of truth — CI uses this to drive its cache-fetch-or-build loop."
        ),
    )
    args = ap.parse_args()

    if args.list_datasets:
        for ds in DATASETS:
            print(ds)
        return 0

    targets = (args.dataset,) if args.dataset is not None else DATASETS
    for name in targets:
        path = prepare_one(name, args.force)
        print(path)
    return 0


if __name__ == "__main__":
    sys.exit(main())

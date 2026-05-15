-- Rolling 7-day average per (dataset, query, system), reading directly
-- from R2.
--
-- One-time setup in a DuckDB session:
--
--   INSTALL httpfs;
--   LOAD httpfs;
--   CREATE SECRET r2_logsearch (
--       TYPE r2,
--       KEY_ID '...',
--       SECRET '...',
--       ACCOUNT_ID '...'
--   );
--
-- Results layout in R2:
--   r2://logsearch-bench/results/<run_id>/measurements.csv
--   r2://logsearch-bench/results/<run_id>/profiles/<dataset>__<query>__<system>__iter<n>.json
--   r2://logsearch-bench/results/<run_id>/viz/<dataset>.png

WITH raw AS (
    SELECT
        run_id,
        CAST(ts_utc AS TIMESTAMP)        AS ts_utc,
        dataset,
        query,
        system,
        TRY_CAST(elapsed_s AS DOUBLE)    AS elapsed_s,
    FROM read_csv(
        'r2://logsearch-bench/results/*/measurements.csv',
        union_by_name = true,
        header = true
    )
    WHERE elapsed_s IS NOT NULL
      AND query <> 'build_index'
),
per_run AS (
    -- Collapse the 3 iters of a (run, dataset, query, system) cell.
    SELECT
        run_id,
        date_trunc('day', ts_utc) AS day,
        dataset,
        query,
        system,
        avg(elapsed_s) AS avg_s,
        min(elapsed_s) AS min_s,
        max(elapsed_s) AS max_s,
    FROM raw
    GROUP BY ALL
),
daily AS (
    -- One row per day in case multiple runs land on the same day.
    SELECT
        day,
        dataset,
        query,
        system,
        median(avg_s) AS daily_avg_s,
        min(min_s)    AS daily_min_s,
        max(max_s)    AS daily_max_s,
    FROM per_run
    GROUP BY ALL
)
SELECT
    day,
    dataset,
    query,
    system,
    daily_avg_s,
    daily_min_s,
    daily_max_s,
    avg(daily_avg_s) OVER w7 AS avg_7d_s,
FROM daily
WINDOW w7 AS (
    PARTITION BY dataset, query, system
    ORDER BY day
    RANGE BETWEEN INTERVAL 6 DAY PRECEDING AND CURRENT ROW
)
ORDER BY dataset, query, system, day;

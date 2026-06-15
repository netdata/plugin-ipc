## Benchmark Notes

- 2026-06-15: `snapshot-shm,go,go,0` missed the enforced Go SHM snapshot floor in the first full POSIX run at `520442` req/s. The POSIX floor-retry mechanism replaced it with the validated retry median `804650` req/s. Retry details are recorded in `benchmarks-posix.floor-retries.csv`.
- 2026-06-15: `uds-pipeline-batch-d16,go,rust,0` used a targeted rerun after the full-suite row measured `13133928` item/s, which was inconsistent with repeated isolated runs of the same row. The isolated runs measured `65534043`, `70190250`, and `65409370` item/s; the published row uses the CPU-accounted targeted rerun at `66261510` item/s with `p50=99us`, `p95=205us`, `p99=279us`, client CPU `84.1%`, server CPU `89.797%`, and total CPU `173.897%`.

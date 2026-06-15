## Benchmark Notes

- 2026-06-15: `snapshot-baseline,rust,c,0` used a targeted rerun after the full Windows matrix failed the stability policy twice for that row. The two failed attempts showed warm-up ramp ratios of `2.572475` and `1.689472` against the `1.35` limit. The published row uses the validated targeted rerun at `71467` req/s with `p50=14.600us`, `p95=29.500us`, `p99=74.200us`, and stable ratio `1.006983`.

# POSIX Benchmark Results

Generated: 2026-06-15 18:15:33 UTC

Machine: local benchmark host (x86_64, 24 cores)

CSV: benchmarks-posix.csv

Complete matrix rows expected: 345

## Validation Summary

| Scenario | Target RPS | Expected Rows | Actual Rows |
|----------|-----------:|--------------:|------------:|
| uds-ping-pong | 0 | 9 | 9 |
| uds-ping-pong | 100000 | 9 | 9 |
| uds-ping-pong | 10000 | 9 | 9 |
| uds-ping-pong | 1000 | 9 | 9 |
| shm-ping-pong | 0 | 9 | 9 |
| shm-ping-pong | 100000 | 9 | 9 |
| shm-ping-pong | 10000 | 9 | 9 |
| shm-ping-pong | 1000 | 9 | 9 |
| uds-batch-ping-pong | 0 | 9 | 9 |
| uds-batch-ping-pong | 100000 | 9 | 9 |
| uds-batch-ping-pong | 10000 | 9 | 9 |
| uds-batch-ping-pong | 1000 | 9 | 9 |
| shm-batch-ping-pong | 0 | 9 | 9 |
| shm-batch-ping-pong | 100000 | 9 | 9 |
| shm-batch-ping-pong | 10000 | 9 | 9 |
| shm-batch-ping-pong | 1000 | 9 | 9 |
| snapshot-baseline | 0 | 9 | 9 |
| snapshot-baseline | 1000 | 9 | 9 |
| snapshot-shm | 0 | 9 | 9 |
| snapshot-shm | 1000 | 9 | 9 |
| lookup | 0 | 3 | 3 |
| cgroups-lookup-known-16 | 0 | 3 | 3 |
| cgroups-lookup-known-16 | 100000 | 3 | 3 |
| cgroups-lookup-known-16 | 10000 | 3 | 3 |
| cgroups-lookup-known-16 | 1000 | 3 | 3 |
| cgroups-lookup-unknown-16 | 0 | 3 | 3 |
| cgroups-lookup-unknown-16 | 100000 | 3 | 3 |
| cgroups-lookup-unknown-16 | 10000 | 3 | 3 |
| cgroups-lookup-unknown-16 | 1000 | 3 | 3 |
| cgroups-lookup-mixed-16 | 0 | 3 | 3 |
| cgroups-lookup-mixed-16 | 100000 | 3 | 3 |
| cgroups-lookup-mixed-16 | 10000 | 3 | 3 |
| cgroups-lookup-mixed-16 | 1000 | 3 | 3 |
| cgroups-lookup-mixed-256 | 0 | 3 | 3 |
| cgroups-lookup-mixed-256 | 100000 | 3 | 3 |
| cgroups-lookup-mixed-256 | 10000 | 3 | 3 |
| cgroups-lookup-mixed-256 | 1000 | 3 | 3 |
| cgroups-lookup-known-8192 | 0 | 3 | 3 |
| cgroups-lookup-known-8192 | 100000 | 3 | 3 |
| cgroups-lookup-known-8192 | 10000 | 3 | 3 |
| cgroups-lookup-known-8192 | 1000 | 3 | 3 |
| cgroups-lookup-known-32768 | 0 | 3 | 3 |
| cgroups-lookup-known-32768 | 100000 | 3 | 3 |
| cgroups-lookup-known-32768 | 10000 | 3 | 3 |
| cgroups-lookup-known-32768 | 1000 | 3 | 3 |
| apps-lookup-known-16 | 0 | 3 | 3 |
| apps-lookup-known-16 | 100000 | 3 | 3 |
| apps-lookup-known-16 | 10000 | 3 | 3 |
| apps-lookup-known-16 | 1000 | 3 | 3 |
| apps-lookup-unknown-16 | 0 | 3 | 3 |
| apps-lookup-unknown-16 | 100000 | 3 | 3 |
| apps-lookup-unknown-16 | 10000 | 3 | 3 |
| apps-lookup-unknown-16 | 1000 | 3 | 3 |
| apps-lookup-mixed-16 | 0 | 3 | 3 |
| apps-lookup-mixed-16 | 100000 | 3 | 3 |
| apps-lookup-mixed-16 | 10000 | 3 | 3 |
| apps-lookup-mixed-16 | 1000 | 3 | 3 |
| apps-lookup-mixed-256 | 0 | 3 | 3 |
| apps-lookup-mixed-256 | 100000 | 3 | 3 |
| apps-lookup-mixed-256 | 10000 | 3 | 3 |
| apps-lookup-mixed-256 | 1000 | 3 | 3 |
| apps-lookup-known-8192 | 0 | 3 | 3 |
| apps-lookup-known-8192 | 100000 | 3 | 3 |
| apps-lookup-known-8192 | 10000 | 3 | 3 |
| apps-lookup-known-8192 | 1000 | 3 | 3 |
| apps-lookup-known-32768 | 0 | 3 | 3 |
| apps-lookup-known-32768 | 100000 | 3 | 3 |
| apps-lookup-known-32768 | 10000 | 3 | 3 |
| apps-lookup-known-32768 | 1000 | 3 | 3 |
| uds-pipeline-d16 | 0 | 9 | 9 |
| uds-pipeline-batch-d16 | 0 | 9 | 9 |

## Benchmark Notes

- 2026-06-15: `snapshot-shm,go,go,0` missed the enforced Go SHM snapshot floor in the first full POSIX run at `520442` req/s. The POSIX floor-retry mechanism replaced it with the validated retry median `804650` req/s. Retry details are recorded in `benchmarks-posix.floor-retries.csv`.
- 2026-06-15: `uds-pipeline-batch-d16,go,rust,0` used a targeted rerun after the full-suite row measured `13133928` item/s, which was inconsistent with repeated isolated runs of the same row. The isolated runs measured `65534043`, `70190250`, and `65409370` item/s; the published row uses the CPU-accounted targeted rerun at `66261510` item/s with `p50=99us`, `p95=205us`, `p99=279us`, client CPU `84.1%`, server CPU `89.797%`, and total CPU `173.897%`.

## UDS Ping-Pong

### Max throughput

| Client | Server | Throughput | p50 (us) | p95 (us) | p99 (us) | Client CPU | Server CPU | Total CPU |
|--------|--------|-----------|----------|----------|----------|------------|------------|-----------|
| c      | c      |    156.4k |        4 |        8 |       20 |       42.8% |     48.742% |    91.542% |
| c      | go     |    134.7k |        5 |       11 |       24 |       40.0% |     51.348% |    91.348% |
| c      | rust   |    159.5k |        4 |        8 |       18 |       45.5% |     48.145% |    93.645% |
| go     | c      |    143.3k |        5 |        9 |       22 |       50.9% |     46.863% |    97.763% |
| go     | go     |    133.3k |        5 |       11 |       24 |       48.1% |     49.729% |    97.829% |
| go     | rust   |    146.9k |        5 |        9 |       22 |       51.9% |     44.551% |    96.451% |
| rust   | c      |    145.4k |        4 |        9 |       23 |       39.0% |     46.968% |    85.968% |
| rust   | go     |    154.5k |        4 |        9 |       21 |       40.1% |     54.035% |    94.135% |
| rust   | rust   |    170.0k |        4 |        8 |       18 |       43.7% |     49.114% |    92.814% |

### 100000 req/s target

| Client | Server | Throughput | p50 (us) | p95 (us) | p99 (us) | Client CPU | Server CPU | Total CPU |
|--------|--------|-----------|----------|----------|----------|------------|------------|-----------|
| c      | c      |    100.0k |        7 |       12 |       25 |       34.4% |     38.667% |    73.067% |
| c      | go     |    100.0k |        7 |       13 |       26 |       33.3% |     42.437% |    75.737% |
| c      | rust   |    100.0k |        6 |       11 |       24 |       33.0% |     34.560% |    67.560% |
| go     | c      |    100.0k |        7 |       11 |       25 |       39.4% |     36.103% |    75.503% |
| go     | go     |    100.0k |        7 |       13 |       27 |       40.8% |     41.801% |    82.601% |
| go     | rust   |     99.9k |        5 |       14 |       29 |       39.1% |     32.636% |    71.736% |
| rust   | c      |    100.0k |        6 |        9 |       22 |       29.9% |     35.217% |    65.117% |
| rust   | go     |    100.0k |        7 |       12 |       25 |       30.6% |     41.234% |    71.834% |
| rust   | rust   |    100.0k |        6 |       11 |       24 |       31.6% |     35.400% |    67.000% |

### 10000 req/s target

| Client | Server | Throughput | p50 (us) | p95 (us) | p99 (us) | Client CPU | Server CPU | Total CPU |
|--------|--------|-----------|----------|----------|----------|------------|------------|-----------|
| c      | c      |     10.0k |        9 |       27 |       39 |        6.8% |      6.471% |    13.271% |
| c      | go     |     10.0k |       10 |       29 |       51 |        7.2% |      8.682% |    15.882% |
| c      | rust   |     10.0k |        9 |       27 |       48 |        6.8% |      6.200% |    13.000% |
| go     | c      |     10.0k |        7 |       39 |      321 |        7.6% |      4.955% |    12.555% |
| go     | go     |     10.0k |        8 |       31 |       68 |        6.9% |      6.735% |    13.635% |
| go     | rust   |     10.0k |        7 |       27 |       47 |        6.5% |      4.728% |    11.228% |
| rust   | c      |     10.0k |        8 |       25 |       37 |        5.9% |      5.739% |    11.639% |
| rust   | go     |     10.0k |        9 |       28 |       54 |        6.3% |      8.053% |    14.353% |
| rust   | rust   |     10.0k |        9 |       26 |       42 |        6.4% |      6.048% |    12.448% |

### 1000 req/s target

| Client | Server | Throughput | p50 (us) | p95 (us) | p99 (us) | Client CPU | Server CPU | Total CPU |
|--------|--------|-----------|----------|----------|----------|------------|------------|-----------|
| c      | c      |      1.0k |       23 |       43 |      108 |        1.6% |      1.411% |     3.011% |
| c      | go     |      1.0k |       23 |       45 |       64 |        1.4% |      2.351% |     3.751% |
| c      | rust   |      1.0k |       24 |       43 |      107 |        1.7% |      1.447% |     3.147% |
| go     | c      |      1.0k |       27 |       50 |      107 |        2.8% |      1.427% |     4.227% |
| go     | go     |      1.0k |       39 |      805 |     2750 |        3.9% |      2.763% |     6.663% |
| go     | rust   |      1.0k |       28 |       55 |      161 |        3.0% |      1.456% |     4.456% |
| rust   | c      |      1.0k |       26 |       48 |      218 |        1.8% |      1.542% |     3.342% |
| rust   | go     |      1.0k |       27 |       74 |      884 |        1.7% |      2.775% |     4.475% |
| rust   | rust   |      1.0k |       23 |       43 |       89 |        1.7% |      1.451% |     3.151% |

## SHM Ping-Pong

### Max throughput

| Client | Server | Throughput | p50 (us) | p95 (us) | p99 (us) | Client CPU | Server CPU | Total CPU |
|--------|--------|-----------|----------|----------|----------|------------|------------|-----------|
| c      | c      |     3.14M |        0 |        0 |        0 |       98.5% |     98.531% |   197.031% |
| c      | go     |     2.63M |        0 |        0 |        0 |       97.3% |     99.888% |   197.188% |
| c      | rust   |     3.33M |        0 |        0 |        0 |       99.0% |     99.051% |   198.051% |
| go     | c      |     2.27M |        0 |        0 |        0 |       95.6% |     94.722% |   190.322% |
| go     | go     |     2.15M |        0 |        0 |        0 |       96.9% |     98.512% |   195.412% |
| go     | rust   |     2.27M |        0 |        0 |        0 |       98.0% |     97.310% |   195.310% |
| rust   | c      |     3.17M |        0 |        0 |        0 |       98.2% |     98.168% |   196.368% |
| rust   | go     |     2.43M |        0 |        0 |        0 |       96.2% |     99.047% |   195.247% |
| rust   | rust   |     2.88M |        0 |        0 |        0 |       98.1% |     98.024% |   196.124% |

### 100000 req/s target

| Client | Server | Throughput | p50 (us) | p95 (us) | p99 (us) | Client CPU | Server CPU | Total CPU |
|--------|--------|-----------|----------|----------|----------|------------|------------|-----------|
| c      | c      |    100.0k |        0 |        3 |       12 |       11.2% |     14.244% |    25.444% |
| c      | go     |     99.9k |        0 |        8 |       16 |       13.5% |     18.788% |    32.288% |
| c      | rust   |     90.0k |        7 |       14 |       28 |       32.4% |     32.365% |    64.765% |
| go     | c      |     90.1k |        7 |       14 |       64 |       34.2% |     29.903% |    64.103% |
| go     | go     |    100.0k |        0 |        0 |       10 |        7.8% |      8.265% |    16.065% |
| go     | rust   |    100.0k |        0 |        0 |       14 |        9.5% |      8.262% |    17.762% |
| rust   | c      |    100.0k |        0 |        3 |       13 |       11.5% |     14.442% |    25.942% |
| rust   | go     |    100.0k |        0 |        5 |       14 |       12.6% |     17.585% |    30.185% |
| rust   | rust   |     99.7k |        7 |       13 |       29 |       32.7% |     32.512% |    65.212% |

### 10000 req/s target

| Client | Server | Throughput | p50 (us) | p95 (us) | p99 (us) | Client CPU | Server CPU | Total CPU |
|--------|--------|-----------|----------|----------|----------|------------|------------|-----------|
| c      | c      |     10.0k |        3 |       13 |       23 |        4.8% |      6.153% |    10.953% |
| c      | go     |     10.0k |        3 |       17 |       31 |        5.7% |      8.960% |    14.660% |
| c      | rust   |     10.0k |        2 |       12 |       21 |        4.9% |      6.504% |    11.404% |
| go     | c      |     10.0k |        0 |       10 |       18 |        2.1% |      1.669% |     3.769% |
| go     | go     |     10.0k |        0 |       13 |       21 |        2.4% |      2.946% |     5.346% |
| go     | rust   |     10.0k |        0 |       12 |       23 |        2.6% |      1.931% |     4.531% |
| rust   | c      |     10.0k |        2 |       12 |       19 |        5.1% |      6.312% |    11.412% |
| rust   | go     |     10.0k |        3 |       17 |       34 |        6.0% |      9.378% |    15.378% |
| rust   | rust   |     10.0k |        3 |       16 |       35 |        5.4% |      6.798% |    12.198% |

### 1000 req/s target

| Client | Server | Throughput | p50 (us) | p95 (us) | p99 (us) | Client CPU | Server CPU | Total CPU |
|--------|--------|-----------|----------|----------|----------|------------|------------|-----------|
| c      | c      |      1.0k |       14 |       29 |      174 |        1.5% |      1.376% |     2.876% |
| c      | go     |      1.0k |       12 |       24 |       38 |        1.2% |      2.290% |     3.490% |
| c      | rust   |      1.0k |       13 |       29 |      209 |        1.3% |      1.329% |     2.629% |
| go     | c      |      1.0k |       14 |       31 |      156 |        2.2% |      1.281% |     3.481% |
| go     | go     |      1.0k |       13 |       27 |       47 |        1.9% |      2.271% |     4.171% |
| go     | rust   |      1.0k |        8 |       22 |       40 |        1.6% |      1.082% |     2.682% |
| rust   | c      |      1.0k |       13 |       27 |      193 |        1.4% |      1.319% |     2.719% |
| rust   | go     |      1.0k |       12 |       23 |       35 |        1.1% |      2.184% |     3.284% |
| rust   | rust   |      1.0k |       15 |       35 |      106 |        1.1% |      0.994% |     2.094% |

## UDS Batch Ping-Pong

### Max throughput

| Client | Server | Throughput | p50 (us) | p95 (us) | p99 (us) | Client CPU | Server CPU | Total CPU |
|--------|--------|-----------|----------|----------|----------|------------|------------|-----------|
| c      | c      |    28.20M |       12 |       28 |       44 |       46.3% |     46.285% |    92.585% |
| c      | go     |    21.99M |       17 |       41 |       57 |       37.0% |     57.547% |    94.547% |
| c      | rust   |    26.76M |       13 |       35 |       48 |       42.8% |     50.547% |    93.347% |
| go     | c      |    24.65M |       13 |       31 |       47 |       53.8% |     41.805% |    95.605% |
| go     | go     |    18.87M |       18 |       45 |       65 |       43.6% |     51.484% |    95.084% |
| go     | rust   |    22.55M |       14 |       39 |       55 |       50.0% |     45.531% |    95.531% |
| rust   | c      |    27.74M |       13 |       26 |       41 |       50.1% |     44.100% |    94.200% |
| rust   | go     |    18.04M |       18 |       44 |       83 |       35.5% |     49.872% |    85.372% |
| rust   | rust   |    25.08M |       14 |       34 |       49 |       46.0% |     46.287% |    92.287% |

### 100000 req/s target

| Client | Server | Throughput | p50 (us) | p95 (us) | p99 (us) | Client CPU | Server CPU | Total CPU |
|--------|--------|-----------|----------|----------|----------|------------|------------|-----------|
| c      | c      |    26.93M |       13 |       29 |       46 |       45.1% |     45.310% |    90.410% |
| c      | go     |    21.83M |       16 |       41 |       59 |       37.0% |     56.770% |    93.770% |
| c      | rust   |    24.97M |       13 |       35 |       52 |       42.6% |     47.043% |    89.643% |
| go     | c      |    24.43M |       13 |       32 |       51 |       53.0% |     40.543% |    93.543% |
| go     | go     |    18.70M |       19 |       45 |       63 |       44.0% |     52.152% |    96.152% |
| go     | rust   |    20.60M |       15 |       41 |       63 |       48.7% |     42.817% |    91.517% |
| rust   | c      |    24.49M |       14 |       31 |       51 |       46.9% |     42.380% |    89.280% |
| rust   | go     |    21.67M |       17 |       40 |       56 |       40.3% |     55.203% |    95.503% |
| rust   | rust   |    23.02M |       14 |       36 |       54 |       45.3% |     45.901% |    91.201% |

### 10000 req/s target

| Client | Server | Throughput | p50 (us) | p95 (us) | p99 (us) | Client CPU | Server CPU | Total CPU |
|--------|--------|-----------|----------|----------|----------|------------|------------|-----------|
| c      | c      |     5.04M |       19 |       44 |       63 |       13.0% |     11.878% |    24.878% |
| c      | go     |     5.04M |       24 |       52 |       71 |       12.8% |     18.691% |    31.491% |
| c      | rust   |     5.04M |       21 |       49 |       71 |       13.1% |     14.502% |    27.602% |
| go     | c      |     5.04M |       18 |       46 |       74 |       15.2% |     11.011% |    26.211% |
| go     | go     |     5.04M |       26 |       60 |      105 |       16.8% |     19.061% |    35.861% |
| go     | rust   |     5.04M |       17 |       45 |       67 |       13.9% |     11.548% |    25.448% |
| rust   | c      |     5.04M |       20 |       45 |       73 |       14.6% |     12.215% |    26.815% |
| rust   | go     |     5.04M |       23 |       51 |       69 |       13.6% |     17.600% |    31.200% |
| rust   | rust   |     5.04M |       19 |       45 |       61 |       13.4% |     13.136% |    26.536% |

### 1000 req/s target

| Client | Server | Throughput | p50 (us) | p95 (us) | p99 (us) | Client CPU | Server CPU | Total CPU |
|--------|--------|-----------|----------|----------|----------|------------|------------|-----------|
| c      | c      |    505.3k |       31 |       57 |       77 |        2.2% |      1.908% |     4.108% |
| c      | go     |    505.0k |       41 |       82 |      307 |        2.3% |      3.729% |     6.029% |
| c      | rust   |    505.3k |       34 |       62 |       87 |        2.3% |      2.147% |     4.447% |
| go     | c      |    505.2k |       33 |       62 |       84 |        3.2% |      1.873% |     5.073% |
| go     | go     |    505.3k |       43 |       81 |      118 |        3.5% |      3.521% |     7.021% |
| go     | rust   |    505.2k |       35 |       64 |       82 |        3.2% |      2.087% |     5.287% |
| rust   | c      |    505.3k |       30 |       56 |       72 |        2.3% |      1.884% |     4.184% |
| rust   | go     |    505.3k |       41 |       74 |       99 |        2.5% |      3.703% |     6.203% |
| rust   | rust   |    505.3k |       31 |       58 |       73 |        2.2% |      2.059% |     4.259% |

## SHM Batch Ping-Pong

### Max throughput

| Client | Server | Throughput | p50 (us) | p95 (us) | p99 (us) | Client CPU | Server CPU | Total CPU |
|--------|--------|-----------|----------|----------|----------|------------|------------|-----------|
| c      | c      |    47.88M |        6 |       17 |       26 |       78.9% |     80.033% |   158.933% |
| c      | go     |    34.24M |       11 |       25 |       35 |       64.0% |     84.127% |   148.127% |
| c      | rust   |    40.56M |        9 |       20 |       32 |       72.2% |     76.991% |   149.191% |
| go     | c      |    37.10M |        8 |       20 |       31 |       79.8% |     69.622% |   149.422% |
| go     | go     |    27.71M |       13 |       28 |       42 |       66.7% |     74.909% |   141.609% |
| go     | rust   |    36.70M |        9 |       21 |       31 |       78.6% |     72.950% |   151.550% |
| rust   | c      |    38.41M |        8 |       20 |       31 |       77.3% |     71.332% |   148.632% |
| rust   | go     |    27.12M |       13 |       29 |       45 |       60.9% |     74.036% |   134.936% |
| rust   | rust   |    36.20M |       10 |       21 |       32 |       74.5% |     72.835% |   147.335% |

### 100000 req/s target

| Client | Server | Throughput | p50 (us) | p95 (us) | p99 (us) | Client CPU | Server CPU | Total CPU |
|--------|--------|-----------|----------|----------|----------|------------|------------|-----------|
| c      | c      |    47.27M |        6 |       17 |       25 |       79.2% |     79.632% |   158.832% |
| c      | go     |    32.98M |       11 |       26 |       37 |       63.2% |     82.421% |   145.621% |
| c      | rust   |    40.16M |        9 |       20 |       32 |       72.3% |     76.734% |   149.034% |
| go     | c      |    36.17M |        8 |       20 |       32 |       79.2% |     68.641% |   147.841% |
| go     | go     |    26.90M |       13 |       29 |       43 |       65.6% |     73.721% |   139.321% |
| go     | rust   |    34.12M |       10 |       22 |       34 |       76.5% |     69.540% |   146.040% |
| rust   | c      |    39.08M |        8 |       19 |       29 |       78.3% |     72.097% |   150.397% |
| rust   | go     |    28.67M |       13 |       28 |       42 |       63.3% |     76.433% |   139.733% |
| rust   | rust   |    35.00M |       10 |       22 |       33 |       73.4% |     71.550% |   144.950% |

### 10000 req/s target

| Client | Server | Throughput | p50 (us) | p95 (us) | p99 (us) | Client CPU | Server CPU | Total CPU |
|--------|--------|-----------|----------|----------|----------|------------|------------|-----------|
| c      | c      |     5.04M |       13 |       27 |       49 |       12.4% |     12.097% |    24.497% |
| c      | go     |     5.04M |       19 |       45 |      162 |       13.6% |     18.865% |    32.465% |
| c      | rust   |     5.04M |       14 |       31 |       49 |       12.5% |     13.482% |    25.982% |
| go     | c      |     5.04M |       12 |       27 |       46 |       13.8% |     10.993% |    24.793% |
| go     | go     |     5.04M |       16 |       35 |       47 |       14.1% |     16.401% |    30.501% |
| go     | rust   |     5.04M |       15 |       36 |      182 |       14.7% |     12.446% |    27.146% |
| rust   | c      |     5.04M |       14 |       28 |       49 |       13.8% |     12.152% |    25.952% |
| rust   | go     |     5.04M |       24 |       98 |      787 |       14.7% |     19.383% |    34.083% |
| rust   | rust   |     5.04M |       16 |       34 |       55 |       14.0% |     14.007% |    28.007% |

### 1000 req/s target

| Client | Server | Throughput | p50 (us) | p95 (us) | p99 (us) | Client CPU | Server CPU | Total CPU |
|--------|--------|-----------|----------|----------|----------|------------|------------|-----------|
| c      | c      |    505.3k |       32 |       52 |       88 |        2.1% |      1.597% |     3.697% |
| c      | go     |    505.3k |       23 |       46 |       65 |        1.8% |      3.064% |     4.864% |
| c      | rust   |    505.3k |       19 |       38 |       48 |        1.7% |      1.824% |     3.524% |
| go     | c      |    505.2k |       20 |       38 |       64 |        2.6% |      1.667% |     4.267% |
| go     | go     |    505.3k |       24 |       49 |       68 |        2.6% |      3.115% |     5.715% |
| go     | rust   |    505.2k |       21 |       42 |       57 |        2.6% |      1.876% |     4.476% |
| rust   | c      |    505.3k |       19 |       35 |       50 |        1.9% |      1.677% |     3.577% |
| rust   | go     |    505.3k |       24 |       47 |       67 |        1.9% |      3.139% |     5.039% |
| rust   | rust   |    505.3k |       21 |       40 |       60 |        2.0% |      1.887% |     3.887% |

## Snapshot Baseline Refresh

### Max throughput

| Client | Server | Throughput | p50 (us) | p95 (us) | p99 (us) | Client CPU | Server CPU | Total CPU |
|--------|--------|-----------|----------|----------|----------|------------|------------|-----------|
| c      | c      |    141.5k |        5 |       10 |       21 |       46.5% |     50.295% |    96.795% |
| c      | go     |    109.2k |        6 |       13 |       30 |       37.9% |     49.188% |    87.088% |
| c      | rust   |    147.4k |        5 |        9 |       21 |       47.9% |     48.685% |    96.585% |
| go     | c      |    124.3k |        5 |       12 |       26 |       54.4% |     45.238% |    99.638% |
| go     | go     |    110.1k |        6 |       14 |       29 |       50.2% |     49.174% |    99.374% |
| go     | rust   |    132.8k |        5 |       11 |       23 |       57.1% |     44.566% |   101.666% |
| rust   | c      |    142.9k |        5 |       10 |       21 |       46.1% |     50.431% |    96.531% |
| rust   | go     |    125.9k |        5 |       12 |       26 |       42.1% |     54.315% |    96.415% |
| rust   | rust   |    143.8k |        5 |       10 |       21 |       47.3% |     48.543% |    95.843% |

### 1000 req/s target

| Client | Server | Throughput | p50 (us) | p95 (us) | p99 (us) | Client CPU | Server CPU | Total CPU |
|--------|--------|-----------|----------|----------|----------|------------|------------|-----------|
| c      | c      |      1.0k |       30 |       55 |      227 |        2.2% |      1.783% |     3.983% |
| c      | go     |      1.0k |       25 |       50 |       75 |        1.6% |      2.500% |     4.100% |
| c      | rust   |      1.0k |       21 |       41 |       52 |        1.5% |      1.312% |     2.812% |
| go     | c      |      1.0k |       30 |       64 |      333 |        3.1% |      1.492% |     4.592% |
| go     | go     |      1.0k |       27 |       54 |       77 |        2.4% |      2.385% |     4.785% |
| go     | rust   |      1.0k |       25 |       50 |       72 |        2.5% |      1.366% |     3.866% |
| rust   | c      |      1.0k |       32 |       60 |      440 |        2.2% |      1.842% |     4.042% |
| rust   | go     |      1.0k |       26 |       49 |       69 |        1.6% |      2.489% |     4.089% |
| rust   | rust   |      1.0k |       23 |       42 |       59 |        1.6% |      1.392% |     2.992% |

## Snapshot SHM Refresh

### Max throughput

| Client | Server | Throughput | p50 (us) | p95 (us) | p99 (us) | Client CPU | Server CPU | Total CPU |
|--------|--------|-----------|----------|----------|----------|------------|------------|-----------|
| c      | c      |     1.26M |        0 |        0 |        0 |       90.2% |     90.280% |   180.480% |
| c      | go     |    935.7k |        0 |        1 |        1 |       93.3% |     96.969% |   190.269% |
| c      | rust   |     1.46M |        0 |        0 |        0 |       97.1% |     97.138% |   194.238% |
| go     | c      |     1.11M |        0 |        0 |        1 |       97.6% |     96.987% |   194.587% |
| go     | go     |    804.7k |    1.000 |    1.000 |    2.000 |     93.900% |     96.311% |   190.211% |
| go     | rust   |     1.18M |        0 |        0 |        1 |       97.7% |     96.973% |   194.673% |
| rust   | c      |     1.31M |        0 |        0 |        0 |       97.2% |     97.207% |   194.407% |
| rust   | go     |    895.8k |        1 |        1 |        1 |       93.4% |     97.086% |   190.486% |
| rust   | rust   |     1.48M |        0 |        0 |        0 |       98.1% |     98.119% |   196.219% |

### 1000 req/s target

| Client | Server | Throughput | p50 (us) | p95 (us) | p99 (us) | Client CPU | Server CPU | Total CPU |
|--------|--------|-----------|----------|----------|----------|------------|------------|-----------|
| c      | c      |      1.0k |       15 |       34 |      352 |        1.6% |      1.454% |     3.054% |
| c      | go     |      1.0k |       16 |       32 |       59 |        1.4% |      2.716% |     4.116% |
| c      | rust   |      1.0k |       13 |       27 |       48 |        0.6% |      1.086% |     1.686% |
| go     | c      |      1.0k |       17 |       37 |      192 |        2.4% |      1.368% |     3.768% |
| go     | go     |      1.0k |       17 |       32 |       51 |        2.1% |      2.422% |     4.522% |
| go     | rust   |      1.0k |       13 |       29 |       51 |        2.0% |      1.246% |     3.246% |
| rust   | c      |      1.0k |       14 |       28 |      103 |        1.4% |      1.349% |     2.749% |
| rust   | go     |      1.0k |       17 |       31 |       76 |        1.4% |      2.635% |     4.035% |
| rust   | rust   |      1.0k |       12 |       24 |       38 |        1.2% |      1.233% |     2.433% |

## UDS Pipeline

### Max throughput

| Client | Server | Throughput | p50 (us) | p95 (us) | p99 (us) | Client CPU | Server CPU | Total CPU |
|--------|--------|-----------|----------|----------|----------|------------|------------|-----------|
| c      | c      |    470.2k |       30 |       48 |       71 |       74.9% |     87.989% |   162.889% |
| c      | go     |    144.3k |       65 |      155 |     1049 |       28.0% |     42.895% |    70.895% |
| c      | rust   |    340.5k |       35 |      106 |      148 |       55.9% |     60.538% |   116.438% |
| go     | c      |    383.8k |       35 |       72 |      130 |       77.2% |     73.514% |   150.714% |
| go     | go     |    377.1k |       36 |       67 |      111 |       77.3% |     84.153% |   161.453% |
| go     | rust   |    194.8k |       56 |      143 |      470 |       45.1% |     36.646% |    81.746% |
| rust   | c      |    487.2k |       27 |       48 |       71 |       69.7% |     86.707% |   156.407% |
| rust   | go     |    321.3k |       37 |       89 |      176 |       53.0% |     77.000% |   130.000% |
| rust   | rust   |    511.8k |       25 |       50 |      107 |       68.8% |     81.398% |   150.198% |

## UDS Pipeline+Batch

### Max throughput

| Client | Server | Throughput | p50 (us) | p95 (us) | p99 (us) | Client CPU | Server CPU | Total CPU |
|--------|--------|-----------|----------|----------|----------|------------|------------|-----------|
| c      | c      |    54.35M |      135 |      265 |      486 |       66.9% |     77.912% |   144.812% |
| c      | go     |    13.15M |      319 |     1941 |     5985 |       17.4% |     36.358% |    53.758% |
| c      | rust   |    43.50M |      144 |      361 |      782 |       54.6% |     67.875% |   122.475% |
| go     | c      |    49.99M |      145 |      295 |      470 |       79.3% |     75.184% |   154.484% |
| go     | go     |    37.66M |      201 |      350 |      505 |       63.0% |     87.223% |   150.223% |
| go     | rust   |    66.26M |       99 |      205 |      279 |       84.1% |     89.797% |   173.897% |
| rust   | c      |    51.44M |      137 |      286 |      556 |       64.5% |     74.097% |   138.597% |
| rust   | go     |    40.06M |      178 |      335 |      470 |       52.1% |     88.315% |   140.415% |
| rust   | rust   |    51.66M |      138 |      288 |      449 |       64.5% |     79.995% |   144.495% |

## Local Cache Lookup

| Language | Throughput | Client CPU | Total CPU |
|----------|-----------|------------|-----------|
| c        |   162.12M |       99.1% |      99.1% |
| go       |   126.27M |       99.3% |      99.3% |
| rust     |   135.89M |       98.1% |      98.1% |

## Lookup Method Codec And Dispatch

| Scenario | Target RPS | Language | Throughput | p50 (us) | p95 (us) | p99 (us) | CPU |
|----------|-----------:|----------|-----------:|---------:|---------:|---------:|----:|
| cgroups-lookup-known-16 | 0 | c        |     711.1k |        1 |        2 |        2 | 99.2% |
| cgroups-lookup-known-16 | 0 | go       |     437.7k |        1 |        3 |        4 | 99.0% |
| cgroups-lookup-known-16 | 0 | rust     |     559.0k |        1 |        2 |        3 | 99.1% |
| cgroups-lookup-known-16 | 100000 | c        |     100.0k |        2 |        2 |        3 | 22.0% |
| cgroups-lookup-known-16 | 100000 | go       |     100.0k |        3 |        4 |        6 | 31.5% |
| cgroups-lookup-known-16 | 100000 | rust     |     100.0k |        2 |        3 |        5 | 28.2% |
| cgroups-lookup-known-16 | 10000 | c        |      10.0k |        2 |        4 |        5 | 4.5% |
| cgroups-lookup-known-16 | 10000 | go       |      10.0k |        3 |        9 |       19 | 5.6% |
| cgroups-lookup-known-16 | 10000 | rust     |      10.0k |        3 |        5 |        9 | 5.7% |
| cgroups-lookup-known-16 | 1000 | c        |       1.0k |        4 |        5 |        7 | 0.8% |
| cgroups-lookup-known-16 | 1000 | go       |       1.0k |        7 |       11 |       17 | 1.7% |
| cgroups-lookup-known-16 | 1000 | rust     |       1.0k |        6 |       13 |       21 | 0.6% |
| cgroups-lookup-unknown-16 | 0 | c        |      1.36M |        0 |        1 |        1 | 99.2% |
| cgroups-lookup-unknown-16 | 0 | go       |     917.6k |        0 |        1 |        2 | 98.9% |
| cgroups-lookup-unknown-16 | 0 | rust     |      1.10M |        0 |        1 |        1 | 99.0% |
| cgroups-lookup-unknown-16 | 100000 | c        |     100.0k |        1 |        1 |        2 | 12.6% |
| cgroups-lookup-unknown-16 | 100000 | go       |     100.0k |        1 |        2 |        5 | 17.7% |
| cgroups-lookup-unknown-16 | 100000 | rust     |     100.0k |        1 |        1 |        3 | 16.1% |
| cgroups-lookup-unknown-16 | 10000 | c        |      10.0k |        1 |        2 |        4 | 3.6% |
| cgroups-lookup-unknown-16 | 10000 | go       |      10.0k |        1 |        6 |       10 | 3.7% |
| cgroups-lookup-unknown-16 | 10000 | rust     |      10.0k |        1 |        3 |        6 | 4.4% |
| cgroups-lookup-unknown-16 | 1000 | c        |       1.0k |        2 |        4 |        5 | 0.8% |
| cgroups-lookup-unknown-16 | 1000 | go       |       1.0k |        4 |        8 |       13 | 1.5% |
| cgroups-lookup-unknown-16 | 1000 | rust     |       1.0k |        3 |        5 |        6 | 0.8% |
| cgroups-lookup-mixed-16 | 0 | c        |     965.8k |        0 |        1 |        1 | 99.1% |
| cgroups-lookup-mixed-16 | 0 | go       |     546.2k |        1 |        2 |        3 | 98.9% |
| cgroups-lookup-mixed-16 | 0 | rust     |     738.1k |        1 |        2 |        2 | 99.0% |
| cgroups-lookup-mixed-16 | 100000 | c        |     100.0k |        1 |        2 |        3 | 17.3% |
| cgroups-lookup-mixed-16 | 100000 | go       |     100.0k |        2 |        3 |        5 | 25.6% |
| cgroups-lookup-mixed-16 | 100000 | rust     |     100.0k |        2 |        2 |        4 | 22.1% |
| cgroups-lookup-mixed-16 | 10000 | c        |      10.0k |        2 |        3 |        4 | 4.1% |
| cgroups-lookup-mixed-16 | 10000 | go       |      10.0k |        2 |        7 |       11 | 4.2% |
| cgroups-lookup-mixed-16 | 10000 | rust     |      10.0k |        2 |        4 |        5 | 4.5% |
| cgroups-lookup-mixed-16 | 1000 | c        |       1.0k |        3 |        5 |        7 | 0.8% |
| cgroups-lookup-mixed-16 | 1000 | go       |       1.0k |        6 |       11 |       16 | 1.6% |
| cgroups-lookup-mixed-16 | 1000 | rust     |       1.0k |        4 |        7 |        9 | 1.0% |
| cgroups-lookup-mixed-256 | 0 | c        |      66.1k |       13 |       22 |       26 | 99.2% |
| cgroups-lookup-mixed-256 | 0 | go       |      41.2k |       21 |       39 |       45 | 98.8% |
| cgroups-lookup-mixed-256 | 0 | rust     |      51.8k |       17 |       27 |       32 | 99.2% |
| cgroups-lookup-mixed-256 | 100000 | c        |      63.1k |       13 |       24 |       27 | 99.2% |
| cgroups-lookup-mixed-256 | 100000 | go       |      41.7k |       21 |       37 |       42 | 99.3% |
| cgroups-lookup-mixed-256 | 100000 | rust     |      50.2k |       18 |       28 |       32 | 99.1% |
| cgroups-lookup-mixed-256 | 10000 | c        |      10.0k |       26 |       31 |       38 | 25.2% |
| cgroups-lookup-mixed-256 | 10000 | go       |      10.0k |       38 |       47 |       62 | 35.4% |
| cgroups-lookup-mixed-256 | 10000 | rust     |      10.0k |       31 |       41 |       48 | 30.7% |
| cgroups-lookup-mixed-256 | 1000 | c        |       1.0k |       29 |       41 |       53 | 2.6% |
| cgroups-lookup-mixed-256 | 1000 | go       |       1.0k |       42 |       55 |       69 | 4.9% |
| cgroups-lookup-mixed-256 | 1000 | rust     |       1.0k |       35 |       44 |       56 | 3.7% |
| cgroups-lookup-known-8192 | 0 | c        |       1.5k |      614 |      901 |     1152 | 98.6% |
| cgroups-lookup-known-8192 | 0 | go       |       1.1k |      855 |     1498 |     1754 | 98.3% |
| cgroups-lookup-known-8192 | 0 | rust     |       1.2k |      796 |     1201 |     1453 | 98.3% |
| cgroups-lookup-known-8192 | 100000 | c        |       1.4k |      635 |     1145 |     1212 | 98.1% |
| cgroups-lookup-known-8192 | 100000 | go       |       1.0k |      865 |     1431 |     1675 | 98.9% |
| cgroups-lookup-known-8192 | 100000 | rust     |       1.2k |      810 |     1188 |     1258 | 98.5% |
| cgroups-lookup-known-8192 | 10000 | c        |       1.3k |      656 |     1186 |     1271 | 97.9% |
| cgroups-lookup-known-8192 | 10000 | go       |       1.0k |      873 |     1508 |     1819 | 98.8% |
| cgroups-lookup-known-8192 | 10000 | rust     |       1.1k |      844 |     1229 |     1380 | 98.6% |
| cgroups-lookup-known-8192 | 1000 | c        |       1.0k |      740 |     1210 |     1294 | 82.9% |
| cgroups-lookup-known-8192 | 1000 | go       |        996 |      886 |     1532 |     1778 | 97.8% |
| cgroups-lookup-known-8192 | 1000 | rust     |        995 |      855 |     1492 |     1554 | 97.6% |
| cgroups-lookup-known-32768 | 0 | c        |        352 |     2616 |     4402 |     4772 | 98.5% |
| cgroups-lookup-known-32768 | 0 | go       |        259 |     3452 |     6772 |     7037 | 99.0% |
| cgroups-lookup-known-32768 | 0 | rust     |        297 |     3236 |     4078 |     5723 | 98.9% |
| cgroups-lookup-known-32768 | 100000 | c        |        350 |     2681 |     3921 |     4847 | 98.2% |
| cgroups-lookup-known-32768 | 100000 | go       |        261 |     3517 |     5592 |     5974 | 99.0% |
| cgroups-lookup-known-32768 | 100000 | rust     |        282 |     3356 |     4606 |     5713 | 98.8% |
| cgroups-lookup-known-32768 | 10000 | c        |        334 |     2819 |     3968 |     4781 | 98.5% |
| cgroups-lookup-known-32768 | 10000 | go       |        203 |     4130 |     7227 |     7543 | 98.7% |
| cgroups-lookup-known-32768 | 10000 | rust     |        242 |     3755 |     6008 |     6211 | 97.2% |
| cgroups-lookup-known-32768 | 1000 | c        |        303 |     2968 |     4884 |     5120 | 97.5% |
| cgroups-lookup-known-32768 | 1000 | go       |        256 |     3645 |     5475 |     6032 | 98.7% |
| cgroups-lookup-known-32768 | 1000 | rust     |        261 |     3668 |     4914 |     5921 | 96.2% |
| apps-lookup-known-16 | 0 | c        |     825.7k |        1 |        1 |        1 | 99.3% |
| apps-lookup-known-16 | 0 | go       |     453.1k |        1 |        3 |        4 | 92.0% |
| apps-lookup-known-16 | 0 | rust     |     490.4k |        1 |        2 |        2 | 93.1% |
| apps-lookup-known-16 | 100000 | c        |     100.0k |        1 |        2 |        3 | 19.9% |
| apps-lookup-known-16 | 100000 | go       |     100.0k |        2 |        3 |        5 | 26.3% |
| apps-lookup-known-16 | 100000 | rust     |     100.0k |        2 |        3 |        4 | 25.7% |
| apps-lookup-known-16 | 10000 | c        |      10.0k |        2 |        3 |        4 | 4.0% |
| apps-lookup-known-16 | 10000 | go       |      10.0k |        2 |        6 |       10 | 4.3% |
| apps-lookup-known-16 | 10000 | rust     |      10.0k |        3 |        4 |        5 | 4.7% |
| apps-lookup-known-16 | 1000 | c        |       1.0k |        3 |        5 |        7 | 0.8% |
| apps-lookup-known-16 | 1000 | go       |       1.0k |        6 |        9 |       15 | 1.6% |
| apps-lookup-known-16 | 1000 | rust     |       1.0k |        4 |        6 |        8 | 0.9% |
| apps-lookup-unknown-16 | 0 | c        |      1.31M |        0 |        1 |        1 | 98.7% |
| apps-lookup-unknown-16 | 0 | go       |      1.00M |        0 |        1 |        1 | 98.8% |
| apps-lookup-unknown-16 | 0 | rust     |      1.56M |        0 |        0 |        0 | 99.0% |
| apps-lookup-unknown-16 | 100000 | c        |     100.0k |        0 |        1 |        2 | 12.1% |
| apps-lookup-unknown-16 | 100000 | go       |     100.0k |        1 |        1 |        3 | 13.6% |
| apps-lookup-unknown-16 | 100000 | rust     |     100.0k |        0 |        1 |        1 | 10.9% |
| apps-lookup-unknown-16 | 10000 | c        |      10.0k |        1 |        2 |        3 | 3.4% |
| apps-lookup-unknown-16 | 10000 | go       |      10.0k |        1 |        3 |        6 | 2.4% |
| apps-lookup-unknown-16 | 10000 | rust     |      10.0k |        1 |        1 |        2 | 2.8% |
| apps-lookup-unknown-16 | 1000 | c        |       1.0k |        2 |        3 |        5 | 0.7% |
| apps-lookup-unknown-16 | 1000 | go       |       1.0k |        3 |        6 |       11 | 1.3% |
| apps-lookup-unknown-16 | 1000 | rust     |       1.0k |        2 |        3 |        4 | 0.7% |
| apps-lookup-mixed-16 | 0 | c        |      1.02M |        0 |        1 |        1 | 99.1% |
| apps-lookup-mixed-16 | 0 | go       |     673.6k |        1 |        2 |        2 | 99.1% |
| apps-lookup-mixed-16 | 0 | rust     |     830.3k |        1 |        1 |        2 | 99.1% |
| apps-lookup-mixed-16 | 100000 | c        |     100.0k |        1 |        1 |        3 | 16.5% |
| apps-lookup-mixed-16 | 100000 | go       |     100.0k |        2 |        2 |        5 | 20.5% |
| apps-lookup-mixed-16 | 100000 | rust     |     100.0k |        1 |        2 |        3 | 18.9% |
| apps-lookup-mixed-16 | 10000 | c        |      10.0k |        1 |        3 |        4 | 3.5% |
| apps-lookup-mixed-16 | 10000 | go       |      10.0k |        2 |        7 |       11 | 4.0% |
| apps-lookup-mixed-16 | 10000 | rust     |      10.0k |        2 |        3 |        4 | 4.3% |
| apps-lookup-mixed-16 | 1000 | c        |       1.0k |        3 |        5 |        6 | 0.8% |
| apps-lookup-mixed-16 | 1000 | go       |       1.0k |        5 |        9 |       15 | 1.6% |
| apps-lookup-mixed-16 | 1000 | rust     |       1.0k |        3 |        5 |        7 | 0.8% |
| apps-lookup-mixed-256 | 0 | c        |      68.6k |       13 |       21 |       24 | 99.2% |
| apps-lookup-mixed-256 | 0 | go       |      50.8k |       16 |       31 |       35 | 99.3% |
| apps-lookup-mixed-256 | 0 | rust     |      56.0k |       16 |       25 |       28 | 98.9% |
| apps-lookup-mixed-256 | 100000 | c        |      61.3k |       13 |       24 |       27 | 98.7% |
| apps-lookup-mixed-256 | 100000 | go       |      50.2k |       17 |       31 |       36 | 99.3% |
| apps-lookup-mixed-256 | 100000 | rust     |      47.0k |       20 |       27 |       34 | 99.0% |
| apps-lookup-mixed-256 | 10000 | c        |      10.0k |       23 |       28 |       37 | 23.1% |
| apps-lookup-mixed-256 | 10000 | go       |      10.0k |       30 |       38 |       52 | 28.9% |
| apps-lookup-mixed-256 | 10000 | rust     |      10.0k |       25 |       37 |       47 | 26.0% |
| apps-lookup-mixed-256 | 1000 | c        |       1.0k |       25 |       30 |       37 | 2.8% |
| apps-lookup-mixed-256 | 1000 | go       |       1.0k |       33 |       42 |       57 | 4.1% |
| apps-lookup-mixed-256 | 1000 | rust     |       1.0k |       27 |       34 |       42 | 3.1% |
| apps-lookup-known-8192 | 0 | c        |       1.6k |      559 |      879 |     1021 | 98.9% |
| apps-lookup-known-8192 | 0 | go       |       1.2k |      738 |     1283 |     1471 | 98.8% |
| apps-lookup-known-8192 | 0 | rust     |       1.2k |      801 |     1250 |     1345 | 98.9% |
| apps-lookup-known-8192 | 100000 | c        |       1.5k |      596 |      857 |     1007 | 98.6% |
| apps-lookup-known-8192 | 100000 | go       |        976 |      865 |     1496 |     1585 | 98.6% |
| apps-lookup-known-8192 | 100000 | rust     |        922 |     1076 |     1365 |     1438 | 98.3% |
| apps-lookup-known-8192 | 10000 | c        |       1.6k |      561 |      849 |      884 | 98.9% |
| apps-lookup-known-8192 | 10000 | go       |       1.2k |      744 |     1291 |     1458 | 98.8% |
| apps-lookup-known-8192 | 10000 | rust     |       1.1k |      809 |     1291 |     1338 | 98.8% |
| apps-lookup-known-8192 | 1000 | c        |       1.0k |      982 |     1070 |     1137 | 83.9% |
| apps-lookup-known-8192 | 1000 | go       |        917 |     1021 |     1546 |     1726 | 97.2% |
| apps-lookup-known-8192 | 1000 | rust     |        993 |      859 |     1344 |     1457 | 96.7% |
| apps-lookup-known-32768 | 0 | c        |        402 |     2335 |     3267 |     4055 | 98.4% |
| apps-lookup-known-32768 | 0 | go       |        276 |     3257 |     5627 |     6105 | 98.6% |
| apps-lookup-known-32768 | 0 | rust     |        277 |     3384 |     5163 |     5421 | 98.2% |
| apps-lookup-known-32768 | 100000 | c        |        397 |     2341 |     3391 |     4149 | 98.6% |
| apps-lookup-known-32768 | 100000 | go       |        264 |     3376 |     5839 |     6070 | 96.5% |
| apps-lookup-known-32768 | 100000 | rust     |        289 |     3285 |     4434 |     5257 | 98.8% |
| apps-lookup-known-32768 | 10000 | c        |        392 |     2374 |     3552 |     4095 | 97.4% |
| apps-lookup-known-32768 | 10000 | go       |        281 |     3091 |     5797 |     6076 | 99.0% |
| apps-lookup-known-32768 | 10000 | rust     |        275 |     3333 |     5157 |     5371 | 98.8% |
| apps-lookup-known-32768 | 1000 | c        |        193 |     4195 |    12037 |    22332 | 70.5% |
| apps-lookup-known-32768 | 1000 | go       |        194 |     3874 |    12035 |    19694 | 83.5% |
| apps-lookup-known-32768 | 1000 | rust     |         96 |     8140 |    26023 |    44940 | 44.2% |

## Methodology

- Fixed-rate rows use 5s samples by default.
- Max-throughput rows use 10s samples by default.
- The script CLI duration controls fixed-rate rows; `NIPC_BENCH_MAX_DURATION` controls max-throughput rows.
- Rows that miss an enforced max-throughput floor are retried before publication: 7 samples x 20s by default, published only when the retry median meets the same floor and retry max/min throughput ratio is <= 1.35.
- Retry diagnostics, when used, are written next to the CSV as `*.floor-retries.csv`.
- Lookup method scale rows at 8192 and 32768 items are codec+dispatch stress gates: the generator requires complete positive-throughput rows, while stable floors are introduced only after baseline data is collected.

## Performance Floors

| Metric | Floor | Status |
|--------|-------|--------|
| SHM ping-pong max | >= 1M req/s | PASS |
| SHM snapshot refresh max | >= 1M req/s for C/Rust pairs, >= 800k req/s for Go pairs | PASS |
| UDS ping-pong max | >= 120k req/s | PASS |
| UDS snapshot refresh max | >= 100k req/s | PASS |
| Local cache lookup | >= 10M lookups/s | PASS |
| cgroups-lookup known-16 max | >= 250k req/s | PASS |
| cgroups-lookup unknown-16 max | >= 500k req/s | PASS |
| cgroups-lookup mixed-16 max | >= 350k req/s | PASS |
| cgroups-lookup mixed-256 max | >= 25k req/s | PASS |
| apps-lookup known-16 max | >= 300k req/s | PASS |
| apps-lookup unknown-16 max | >= 500k req/s | PASS |
| apps-lookup mixed-16 max | >= 350k req/s | PASS |
| apps-lookup mixed-256 max | >= 25k req/s | PASS |


# POSIX Benchmark Results

Generated: 2026-03-15 11:38:50 UTC

Machine: costa-desktop (x86_64, 24 cores)

Duration per run: extracted from CSV data.

## UDS Ping-Pong (max throughput)

| Client | Server | Throughput | p50 (us) | p95 (us) | p99 (us) | Client CPU | Server CPU |
|--------|--------|-----------|----------|----------|----------|------------|------------|
| c      | c      |    184.7k |        4 |        7 |       11 |       48.0% |          0% |
| rust   | c      |    183.1k |        4 |        8 |       11 |       48.6% |          0% |
| go     | c      |    160.4k |        4 |        8 |       14 |       53.8% |          0% |
| c      | rust   |    190.2k |        4 |        7 |       10 |       48.1% |          0% |
| rust   | rust   |    186.4k |        4 |        8 |       11 |       48.7% |          0% |
| go     | rust   |    170.1k |        4 |        8 |       13 |       54.2% |          0% |
| c      | go     |    165.1k |        4 |        8 |       15 |       45.2% |          0% |
| rust   | go     |    182.9k |        4 |        8 |       12 |       46.7% |          0% |
| go     | go     |    160.6k |        4 |        9 |       15 |       51.9% |          0% |

## SHM Ping-Pong (max throughput)

| Client | Server | Throughput | p50 (us) | p95 (us) | p99 (us) | Client CPU | Server CPU |
|--------|--------|-----------|----------|----------|----------|------------|------------|
| c      | c      |     2.92M |        0 |        0 |        0 |       99.2% |          0% |
| rust   | c      |     3.04M |        0 |        0 |        0 |       99.1% |          0% |
| go     | c      |     2.59M |        0 |        0 |        0 |       99.4% |          0% |
| c      | rust   |     3.22M |        0 |        0 |        0 |       99.2% |          0% |
| rust   | rust   |     3.12M |        0 |        0 |        0 |       99.1% |          0% |
| go     | rust   |     2.60M |        0 |        0 |        0 |       99.5% |          0% |
| c      | go     |     2.74M |        0 |        0 |        0 |       96.2% |          0% |
| rust   | go     |     2.73M |        0 |        0 |        0 |       96.1% |          0% |
| go     | go     |     2.45M |        0 |        0 |        0 |       96.3% |          0% |

## Snapshot Baseline Refresh (max throughput)

| Client | Server | Throughput | p50 (us) | p95 (us) | p99 (us) | Client CPU | Server CPU |
|--------|--------|-----------|----------|----------|----------|------------|------------|
| c      | c      |    127.4k |        6 |       11 |       17 |       38.5% |          0% |
| rust   | c      |    123.3k |        6 |       11 |       19 |       51.0% |          0% |
| go     | c      |    110.4k |        7 |       15 |       29 |       94.8% |          0% |
| c      | rust   |    126.2k |        6 |       12 |       17 |       37.1% |          0% |
| rust   | rust   |    123.6k |        6 |       12 |       18 |       47.9% |          0% |
| go     | rust   |    112.3k |        6 |       15 |       27 |       92.7% |          0% |
| c      | go     |     69.8k |       11 |       27 |       46 |       23.2% |          0% |
| rust   | go     |     72.0k |       10 |       27 |       45 |       30.2% |          0% |
| go     | go     |     67.0k |       11 |       28 |       44 |       66.0% |          0% |

## Snapshot SHM Refresh (max throughput)

| Client | Server | Throughput | p50 (us) | p95 (us) | p99 (us) | Client CPU | Server CPU |
|--------|--------|-----------|----------|----------|----------|------------|------------|
| c      | c      |    522.2k |        1 |        2 |        2 |       99.3% |          0% |
| rust   | c      |    546.0k |        1 |        2 |        2 |       99.2% |          0% |
| go     | c      |    164.5k |        5 |        8 |       19 |       99.0% |          0% |
| c      | rust   |    439.4k |        2 |        2 |        3 |       98.8% |          0% |
| rust   | rust   |    467.2k |        1 |        2 |        3 |       99.2% |          0% |
| go     | rust   |    166.1k |        5 |        8 |       19 |       98.8% |          0% |
| c      | go     |    128.2k |        5 |       16 |       28 |       68.0% |          0% |
| rust   | go     |    145.6k |        4 |       16 |       24 |       76.3% |          0% |
| go     | go     |    122.0k |        5 |       18 |       32 |       86.8% |          0% |

## Local Cache Lookup

| Language | Throughput | CPU |
|----------|-----------|-----|
| c        |    78.85M | 99.4% |
| rust     |   184.90M | 99.3% |
| go       |   106.76M | 99.4% |

## Rate-Limited Results

### UDS Ping-Pong at 100k/s

| Client | Server | Throughput | p50 (us) | p95 (us) | p99 (us) |
|--------|--------|-----------|----------|----------|----------|
| c      | c      |    100.0k |        5 |        8 |       14 |
| rust   | c      |    100.0k |        5 |        8 |       14 |
| go     | c      |    100.0k |        5 |        8 |       15 |
| c      | rust   |    100.0k |        7 |        8 |       16 |
| rust   | rust   |    100.0k |        5 |        8 |       14 |
| go     | rust   |    100.0k |        4 |        8 |       14 |
| c      | go     |    100.0k |        6 |        9 |       16 |
| rust   | go     |    100.0k |        6 |        9 |       18 |
| go     | go     |    100.0k |        5 |        9 |       16 |

## UDS Pipeline (C client, C server, max rate)

| Depth | Throughput | p50 (us) | p95 (us) | p99 (us) | Client CPU |
|-------|-----------|----------|----------|----------|------------|
|     1 |    183.4k |        4 |        7 |       11 |       47.7% |
|     4 |    441.2k |        8 |       13 |       16 |       68.4% |
|     8 |    552.5k |       13 |       22 |       26 |       73.4% |
|    16 |    605.4k |       23 |       41 |       47 |       77.6% |
|    32 |    669.1k |       42 |       74 |       83 |       81.0% |

## Performance Floors

| Metric | Floor | Status |
|--------|-------|--------|
| SHM ping-pong max | >= 1M req/s | PASS |
| UDS ping-pong max | >= 150k req/s | PASS |
| Local cache lookup | >= 10M lookups/s | PASS |


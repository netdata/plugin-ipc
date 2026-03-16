# POSIX Benchmark Results

Generated: 2026-03-16 12:34:18 UTC

Machine: costa-desktop (x86_64, 24 cores)

Duration per run: extracted from CSV data.

## UDS Ping-Pong (max throughput)

| Client | Server | Throughput | p50 (us) | p95 (us) | p99 (us) | Client CPU | Server CPU |
|--------|--------|-----------|----------|----------|----------|------------|------------|
| c      | c      |    165.3k |        4 |        8 |       14 |       46.5% |          0% |
| rust   | c      |    165.6k |        4 |        8 |       14 |       48.6% |          0% |
| go     | c      |    158.4k |        4 |        8 |       16 |       53.4% |          0% |
| c      | rust   |    172.0k |        4 |        8 |       13 |       46.5% |          0% |
| rust   | rust   |    173.6k |        4 |        8 |       13 |       48.0% |          0% |
| go     | rust   |    158.0k |        4 |        8 |       16 |       53.0% |          0% |
| c      | go     |    161.2k |        4 |        8 |       14 |       44.3% |          0% |
| rust   | go     |    161.4k |        4 |        8 |       15 |       45.8% |          0% |
| go     | go     |    146.4k |        5 |        9 |       17 |       50.7% |          0% |

## SHM Ping-Pong (max throughput)

| Client | Server | Throughput | p50 (us) | p95 (us) | p99 (us) | Client CPU | Server CPU |
|--------|--------|-----------|----------|----------|----------|------------|------------|
| c      | c      |     3.10M |        0 |        0 |        0 |       98.2% |          0% |
| rust   | c      |     3.07M |        0 |        0 |        0 |       98.7% |          0% |
| go     | c      |     2.53M |        0 |        0 |        0 |       99.3% |          0% |
| c      | rust   |     3.23M |        0 |        0 |        0 |       98.9% |          0% |
| rust   | rust   |     2.94M |        0 |        0 |        0 |       98.8% |          0% |
| go     | rust   |     2.45M |        0 |        0 |        0 |       98.5% |          0% |
| c      | go     |     2.57M |        0 |        0 |        0 |       88.4% |          0% |
| rust   | go     |     2.46M |        0 |        0 |        0 |       86.5% |          0% |
| go     | go     |     2.18M |        0 |        0 |        0 |       86.5% |          0% |

## Snapshot Baseline Refresh (max throughput)

| Client | Server | Throughput | p50 (us) | p95 (us) | p99 (us) | Client CPU | Server CPU |
|--------|--------|-----------|----------|----------|----------|------------|------------|
| c      | c      |    123.7k |        6 |       11 |       21 |       38.3% |          0% |
| rust   | c      |    121.9k |        6 |       11 |       20 |       52.5% |          0% |
| go     | c      |    100.2k |        8 |       19 |       32 |       94.4% |          0% |
| c      | rust   |    112.3k |        6 |       13 |       23 |       35.4% |          0% |
| rust   | rust   |    115.3k |        6 |       13 |       21 |       47.8% |          0% |
| go     | rust   |     99.0k |        7 |       17 |       31 |       90.2% |          0% |
| c      | go     |     57.0k |       14 |       34 |       60 |       20.6% |          0% |
| rust   | go     |     59.0k |       12 |       34 |       59 |       27.8% |          0% |
| go     | go     |     50.3k |       15 |       40 |       67 |       58.0% |          0% |

## Snapshot SHM Refresh (max throughput)

| Client | Server | Throughput | p50 (us) | p95 (us) | p99 (us) | Client CPU | Server CPU |
|--------|--------|-----------|----------|----------|----------|------------|------------|
| c      | c      |    563.4k |        1 |        2 |        2 |       98.6% |          0% |
| rust   | c      |    560.5k |        1 |        2 |        2 |       98.8% |          0% |
| go     | c      |    153.7k |        5 |        9 |       22 |       97.7% |          0% |
| c      | rust   |    444.9k |        2 |        2 |        3 |       97.9% |          0% |
| rust   | rust   |    434.9k |        2 |        2 |        3 |       98.4% |          0% |
| go     | rust   |    151.9k |        5 |       10 |       22 |       97.9% |          0% |
| c      | go     |     95.7k |        7 |       20 |       37 |       52.4% |          0% |
| rust   | go     |     99.2k |        5 |       20 |       38 |       57.6% |          0% |
| go     | go     |     83.0k |        7 |       26 |       47 |       73.7% |          0% |

## Local Cache Lookup

| Language | Throughput | CPU |
|----------|-----------|-----|
| c        |    69.76M | 99.1% |
| rust     |   196.43M | 98.8% |
| go       |   145.14M | 99.1% |

## Rate-Limited Results

### UDS Ping-Pong at 100k/s

| Client | Server | Throughput | p50 (us) | p95 (us) | p99 (us) |
|--------|--------|-----------|----------|----------|----------|
| c      | c      |    100.0k |        7 |        8 |       17 |
| rust   | c      |    100.0k |        7 |        8 |       16 |
| go     | c      |    100.0k |        5 |        9 |       17 |
| c      | rust   |    100.0k |        7 |        8 |       15 |
| rust   | rust   |    100.0k |        5 |        8 |       16 |
| go     | rust   |    100.0k |        5 |        9 |       17 |
| c      | go     |    100.0k |        7 |        9 |       17 |
| rust   | go     |    100.0k |        7 |        9 |       17 |
| go     | go     |    100.0k |        6 |        9 |       18 |

## UDS Pipeline (C client, C server, max rate)

| Depth | Throughput | p50 (us) | p95 (us) | p99 (us) | Client CPU |
|-------|-----------|----------|----------|----------|------------|
|     1 |    158.1k |        5 |        8 |       17 |       46.0% |
|     4 |    378.3k |        8 |       14 |       23 |       65.4% |
|     8 |    479.5k |       13 |       24 |       33 |       71.4% |
|    16 |    558.3k |       23 |       41 |       53 |       75.9% |
|    32 |    587.1k |       44 |       77 |       91 |       79.4% |

## Performance Floors

| Metric | Floor | Status |
|--------|-------|--------|
| SHM ping-pong max | >= 1M req/s | PASS |
| UDS ping-pong max | >= 150k req/s | FAIL |
| Local cache lookup | >= 10M lookups/s | PASS |


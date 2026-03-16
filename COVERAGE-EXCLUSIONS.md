# Coverage Exclusions

Lines that cannot reach 100% coverage without special infrastructure
(malloc injection, kernel fault injection, etc.). Each exclusion is
justified. Target: 100% coverage on all testable lines.

## C Library (104 excluded lines out of 1923 total = 94.6% testable ceiling)

### netipc_protocol.c — 0 exclusions (all 29 uncovered lines are testable)

### netipc_uds.c — 25 exclusions

| Lines | Reason |
|-------|--------|
| 73 | `getsockopt` failure on valid socket — unreachable on any supported OS |
| 78 | `SO_SNDBUF <= 0` — kernel always returns positive |
| 113 | `raw_send` short write on local UDS SEQPACKET — race condition |
| 209, 214 | Handshake `raw_send`/`raw_recv` failure — requires mid-handshake connection drop |
| 238 | Hello-ack decode failure — well-formed ACK always decodes |
| 399 | `send_ack` failure during server handshake — mid-handshake drop |
| 431 | `socket()` fails in stale check — OS resource exhaustion |
| 480, 494-496 | `socket()`/`listen()` fails — OS resource exhaustion |
| 520 | `accept()` fails — OS resource exhaustion |
| 556 | `socket()` fails in connect — OS resource exhaustion |
| 634 | `inflight_add` realloc failure — malloc failure |
| 676 | `LIMIT_EXCEEDED` from inflight alloc failure — malloc failure |
| 707 | `chunk_payload_budget == 0` — unreachable (SO_SNDBUF always >> 32) |
| 733-735, 737 | Send rollback on single-packet failure — mid-send local socket failure |
| 766-768, 770 | Send rollback on chunked-send failure — mid-send failure |
| 793 | `realloc` failure in `ensure_recv_buf` — malloc failure |
| 885 | `ensure_recv_buf` fails during chunked receive — malloc failure |
| 908 | `pkt_buf` malloc failure during chunked receive — malloc failure |

### netipc_shm.c — 21 exclusions

| Lines | Reason |
|-------|--------|
| 154 | `stat()` fails on file from directory listing — TOCTOU race |
| 171-172 | `mmap` fails in stale check — memory pressure required |
| 246-248 | `ftruncate` fails after open — filesystem quota / RO FS |
| 254-256 | `mmap` fails in server_create — memory pressure |
| 360-361 | `fstat` fails on valid fd — unreachable |
| 374-375 | `mmap` fails in client_attach — memory pressure |
| 626 | Timeout before `futex_wait` — precise timing race |

### netipc_service.c — 58 exclusions

| Lines | Category | Reason |
|-------|----------|--------|
| 133 | malloc | SHM path transport_send malloc |
| 174, 177, 181 | defensive | SHM receive error/truncation/decode in transport_receive |
| 234, 236, 238 | defensive | Response kind/code/message_id mismatch in do_raw_call |
| 307 | defensive | cgroups_req_encode always succeeds (4-byte buffer) |
| 446, 455 | defensive | increment_encode always succeeds / do_raw_call path |
| 496, 593, 599-600 | malloc | Batch/string_reverse allocation failures |
| 681 | malloc | recv_buf in server session handler |
| 696, 698, 702 | defensive | SHM receive unexpected error/truncation/decode in server |
| 716 | defensive | UDS receive failure in server (covered by disconnect) |
| 814 | malloc | SHM response malloc for large message |
| 824, 829 | defensive | SHM/UDS send failure in server |
| 943 | malloc | sessions array calloc |
| 1020-1023 | SHM | SHM server_create fails after successful handshake |
| 1031-1034 | malloc | sctx calloc |
| 1045-1057 | growth | Session array realloc (initial capacity sufficient) |
| 1068-1081 | OS | pthread_create failure |
| 1258 | malloc | Hash buckets calloc |
| 1297 | malloc | Cache items calloc |
| 1304-1305 | defensive | Item decode fails after successful resp_decode |
| 1315-1316, 1325-1327 | malloc | Item name/path strdup |
| 1398-1399 | malloc | cache_build_items returns NULL |

## Summary

| File | Total Lines | Uncovered | Testable | Exclusions | Testable Ceiling |
|------|------------|-----------|----------|------------|-----------------|
| netipc_protocol.c | 375 | 29 | 29 | 0 | 100.0% |
| netipc_uds.c | 467 | 66 | 41 | 25 | 94.6% |
| netipc_shm.c | 348 | 52 | 31 | 21 | 93.9% |
| netipc_service.c | 733 | 173 | 115 | 58 | 92.1% |
| **Total** | **1923** | **320** | **216** | **104** | **94.6%** |

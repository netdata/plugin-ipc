# TODO: L2 Typed Methods + Spec Compliance Fixes

## TL;DR

Add INCREMENT and STRING_REVERSE as proper L2 typed methods to prove the
architecture pattern: L1 transport is generic, Codec is per-method, L2
composes them. Then fix the verified spec violations.

## Purpose

Establish the repeatable pattern for adding new IPC methods (IP→ASN,
IP→Country, CgroupID→Name, PID→Traffic, etc.) to Netdata's plugin IPC.

---

## Part 1: Add INCREMENT and STRING_REVERSE methods

### Wire layouts

**INCREMENT** (method code 1, request and response identical):
```
| Offset | Size | Type | Field |
|--------|------|------|-------|
| 0      | 8    | u64  | value |
```
Total: 8 bytes. Matches existing benchmark format.

**STRING_REVERSE** (method code 3, request and response identical):
```
| Offset | Size | Type  | Field                         |
|--------|------|-------|-------------------------------|
| 0      | 4    | u32   | str_offset (from payload start) |
| 4      | 4    | u32   | str_length (excluding NUL)    |
| 8      | N+1  | bytes | string data + NUL             |
```
Fixed header: 8 bytes. Variable data follows at str_offset = 8.

### Codec additions (protocol layer, all 3 languages)

Each method gets encode + decode functions:
- `increment_encode(value) → bytes`
- `increment_decode(bytes) → value`
- `string_reverse_encode(str, len) → bytes`
- `string_reverse_decode(bytes) → view (str_ptr, str_len)`

### L2 refactoring

Extract generic call infrastructure from the existing cgroups-specific code:
- `do_raw_call(ctx, method_code, req_payload, resp_buf) → (resp_payload, resp_len)`
- `call_with_retry(ctx, raw_call) → error` (at-least-once retry logic)
- Each typed method just does: encode → call_with_retry → decode

### Ping-pong test pattern

Server handles both methods. Client does:
1. INCREMENT: send 0 → get 1 → send 1 → get 2 → ... → verify N rounds
2. STRING_REVERSE: send "abcdef" → get "fedcba" → send "fedcba" → get "abcdef" → verify

### Files to modify

| Layer | C | Rust | Go |
|-------|---|------|----|
| Codec | netipc_protocol.h, netipc_protocol.c | protocol.rs | frame.go |
| L2 client | netipc_service.h, netipc_service.c | cgroups.rs | client.go, types.go |
| Docs | level1-wire-envelope.md | — | — |

---

## Part 2: Spec compliance fixes (after Part 1)

1. **L1 batch directory validation** on receive
2. **SHM fallback → fail session** (fixes transport desync deadlock)
3. **Stale SHM cleanup** wired into server startup
4. **Dynamic buffer sizing** from negotiated limits

---

## Status

- [x] Analysis complete
- [ ] Part 1: Codec additions
- [ ] Part 1: L2 refactoring
- [ ] Part 1: Ping-pong tests
- [ ] Part 2: Spec compliance fixes

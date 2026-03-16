# TODO: Spec Compliance — Final Pass

## Remaining Work

### 1. L2 spec: update typed call contract to match returned-view API
- level2-typed-api.md describes callback-based delivery
- Code returns decoded views/values directly
- getting-started.md already documents the correct pattern
- Fix: rewrite the typed call section in level2-typed-api.md

### 2. L3 status: add missing fields
- Spec requires: connection state (from L2), last successful refresh timestamp
- Code only has: populated, item_count, systemd, generation, success/failure counts
- Fix: add connection_state and last_refresh_timestamp to C, Rust, Go

### 3. L2 batch client calls
- call_increment_batch, call_string_reverse_batch, call_cgroups_snapshot (already single)
- Encode N items into one L1 batch message, send, receive batch response, decode each
- L1 batch builder + extraction already work — L2 must compose them

### 4. L2 server batch dispatch
- When BATCH flag + item_count > 1: split into items, call handler per item, reassemble
- Currently server always sends item_count=1
- Fix: add batch dispatch in the managed server request loop

### 5. L2/L3 interop tests over SHM
- Current interop fixtures hardcode PROFILE_BASELINE
- Fix: add SHM profile variants to interop tests

### 6. Windows service/cache test coverage
- Rust/Go service tests are unix-only
- Fix: add Windows service test variants

### 7. Coverage tooling
- Spec claims 100% line + branch, tooling enforces 90% line-only
- Fix: update tooling or update the claim

### 8. File size discipline
- cgroups.rs 2689, protocol.rs 2372, posix.rs 2081 — all > 500 lines
- Fix: split into per-concern files where appropriate

## Status
- [ ] 1. L2 typed call spec
- [ ] 2. L3 status fields
- [ ] 3. L2 batch client calls
- [ ] 4. L2 server batch dispatch
- [ ] 5. SHM interop tests
- [ ] 6. Windows test coverage
- [ ] 7. Coverage tooling
- [ ] 8. File sizes

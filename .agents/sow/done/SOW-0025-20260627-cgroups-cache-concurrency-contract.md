# SOW-0025 - Cgroups Cache Concurrency Contract

## Status

Status: completed

`completed` is the successful terminal status. `done` is a directory name, not a status value. Do not use `Status: done` or `Status: complete`.

Sub-state: implementation, validation, documentation, follow-up mapping, and SOW closeout completed for the selected long-term-best L3 snapshot-cache direction.

## Requirements

### Purpose

Protect Netdata integrations from cgroups-cache lifetime and concurrency hazards while keeping the IPC library contract simple, explicit, performant, and consistent across C, Rust, and Go.

The immediate purpose is not to assume a confirmed Netdata production race. It is to decide the correct upstream `plugin-ipc` contract and the matching Netdata consumption pattern before the current latent API hazard becomes a real integration bug.

### User Request

The user requested a SOW in `plugin-ipc` for the verified `netipc` cgroups-cache finding and asked to discuss how Netdata should deal with it.

### Assistant Understanding

Facts:

- The C L3 cgroups cache owns an `items` array and hash buckets in `nipc_cgroups_cache_t`.
- A successful C cache refresh frees the previous `items` array and replaces it.
- C lookup returns a borrowed pointer into the current `items` array.
- C close frees the current `items` array and buckets.
- `docs/netipc-integrator-skill.md` already states that L2 clients and L3 caches are single-owner mutable objects unless the integrator adds external synchronization.
- The same skill already warns not to perform `lookup()` concurrently with `refresh()` on the same L3 cache object unless external synchronization is added.
- Rust's public cache API uses `refresh(&mut self)` and `lookup(&self)`, so safe Rust naturally serializes mutation of the same cache object.
- Go's cache lookup returns a `CacheItem` value copy, but the Go cache object mutates slices and fields during `Refresh()` and has no internal lock; concurrent `Refresh()` and `Lookup()` would still be a Go data race.
- Current Netdata eBPF usage did not show a production caller of `nipc_cgroups_cache_lookup()`.
- Current Netdata eBPF usage refreshes one static cache in the cgroup integration thread, then reads `cache.items` directly and publishes derived state into `ebpf_cgroup_pids` under Netdata's own mutex.
- Current Netdata shutdown path joins the cgroup integration thread before cleanup tears down the netipc cache it reads.

Inferences:

- The original SOW finding should be treated as a real latent API/integration hazard, not as a proven current Netdata eBPF runtime race.
- The C public API is easy to misuse because the returned pointer is borrowed and the struct exposes mutable backing storage.
- The repository already documents the single-owner contract, but the C header and getting-started examples do not make the concurrency and lifetime boundary as hard to miss as the integrator skill does.
- Netdata's direct `cache.items` iteration is currently consistent with its single-threaded refresh/import loop, but it is a fragile coupling to public struct layout if more consumers or threads are added later.

Resolved unknowns:

- The C API moved direct cache storage behind an opaque snapshot pointer inside `nipc_cgroups_cache_t`; public direct item storage access is no longer part of the cache contract.
- Rust and Go were updated to the same guard/get/dup semantics while keeping language-appropriate owned value behavior.
- Netdata migration is not bundled into this repository SOW. It is tracked by a real Netdata follow-up SOW after vendoring.
- This SOW remains independent from SOW-0021 because it addresses L3 cache ownership/lifetime and not Level 2 lookup scale.

### Acceptance Criteria

- The chosen cgroups-cache ownership and concurrency contract is explicitly recorded.
- The C, Rust, and Go cache APIs are reviewed against the chosen contract.
- The C public header, getting-started docs, integrator guidance, and tests are updated or explicitly rejected as unnecessary with evidence.
- Netdata's current eBPF integration path is classified as one of:
  - acceptable under the chosen contract;
  - acceptable after local Netdata synchronization/copying changes;
  - unacceptable until `plugin-ipc` exposes a safer API.
- Any needed Netdata follow-up is tracked in the appropriate Netdata repo SOW or issue before this SOW closes.
- Validation includes same-failure searches for concurrent `refresh()` / `lookup()` / direct `items` usage in C, Rust, Go, tests, docs, and known Netdata integration code.
- If the API changes, tests cover the new safety contract in C, Rust, and Go; if the API does not change, docs/tests prove the single-owner contract is deliberate and visible.

## Analysis

Sources checked:

- `AGENTS.md`
- `.agents/sow/SOW.template.md`
- `.agents/sow/current/SOW-0021-20260613-netipc-at-scale.md`
- `docs/netipc-integrator-skill.md`
- `docs/getting-started.md`
- `src/libnetdata/netipc/include/netipc/netipc_service.h`
- `src/libnetdata/netipc/src/service/netipc_service_cgroups_cache_common.c`
- `src/crates/netipc/src/service/raw/cgroups_cache.rs`
- `src/crates/netipc/src/service/cgroups_snapshot.rs`
- `src/go/pkg/netipc/service/raw/cgroups_cache.go`
- `src/go/pkg/netipc/service/cgroups_snapshot/cache_common.go`
- `netdata/netdata @ 120ca90bf6a9d2c68fa093e2fd95b9456be8aeec`
  - `src/collectors/ebpf.plugin/ebpf_cgroup.c`
  - `src/collectors/ebpf.plugin/ebpf.c`
  - `src/collectors/ebpf.plugin/ebpf_process.h`

Current state:

- `src/libnetdata/netipc/include/netipc/netipc_service.h:550` exposes `nipc_cgroups_cache_t` with mutable `items`, `item_count`, `buckets`, and counters.
- `src/libnetdata/netipc/include/netipc/netipc_service.h:607` documents that `nipc_cgroups_cache_lookup()` returns a pointer valid until the next successful refresh.
- `src/libnetdata/netipc/src/service/netipc_service_cgroups_cache_common.c:149` frees old items during successful refresh.
- `src/libnetdata/netipc/src/service/netipc_service_cgroups_cache_common.c:179` returns `&cache->items[idx]` from lookup.
- `src/libnetdata/netipc/src/service/netipc_service_cgroups_cache_common.c:197` reads status fields without synchronization.
- `src/libnetdata/netipc/src/service/netipc_service_cgroups_cache_common.c:209` frees items during close.
- `docs/netipc-integrator-skill.md:373` says L2 clients and L3 caches should be treated as single-owner mutable objects unless the integrator adds external synchronization.
- `docs/netipc-integrator-skill.md:1019` says not to perform `lookup()` concurrently with `refresh()` on the same L3 cache object unless external synchronization is added.
- `docs/getting-started.md:290` shows C lookup returning a borrowed pointer, but the example does not state the pointer lifetime or single-owner concurrency rule near the code.
- `src/crates/netipc/src/service/raw/cgroups_cache.rs:90` requires `&mut self` for Rust refresh.
- `src/crates/netipc/src/service/raw/cgroups_cache.rs:166` returns `Option<&CgroupsCacheItem>` for Rust lookup.
- `src/go/pkg/netipc/service/raw/cgroups_cache.go:73` mutates Go cache state during refresh.
- `src/go/pkg/netipc/service/raw/cgroups_cache.go:154` returns a Go `CacheItem` value copy from lookup, avoiding borrowed pointer lifetime exposure but not making concurrent access data-race-free.
- `netdata/netdata @ 120ca90bf6a9d2c68fa093e2fd95b9456be8aeec src/collectors/ebpf.plugin/ebpf_cgroup.c:15` has one static eBPF cgroups cache.
- `netdata/netdata @ 120ca90bf6a9d2c68fa093e2fd95b9456be8aeec src/collectors/ebpf.plugin/ebpf_cgroup.c:360` refreshes that cache in the integration thread.
- `netdata/netdata @ 120ca90bf6a9d2c68fa093e2fd95b9456be8aeec src/collectors/ebpf.plugin/ebpf_cgroup.c:428` iterates `ebpf_cgroup_cache.items` directly after refresh.
- `netdata/netdata @ 120ca90bf6a9d2c68fa093e2fd95b9456be8aeec src/collectors/ebpf.plugin/ebpf.c:1130` explicitly joins the cgroup integration thread before cleanup tears down the cache it reads.

Risks:

- If the single-owner contract is kept but not made prominent in the C header and examples, future C integrators can create use-after-free or data-race bugs by sharing one cache across threads.
- If the cache API grows internal locking but still returns borrowed pointers, callers may wrongly believe the returned pointer is safe after a concurrent refresh; that would be false unless the API also changes pointer lifetime semantics.
- If the public C struct remains field-accessible, integrators can bypass any future safety wrapper and directly race on `items`, `item_count`, or `buckets`.
- If Netdata relies on direct cache internals, future IPC changes may create ABI/source coupling or require synchronized vendoring changes.
- If the API is hardened with snapshot/refcount semantics, implementation and test cost increases in C, Rust, and Go.
- If Go remains unlocked, its value-copy lookup avoids dangling references but still permits data races when a shared cache is accessed concurrently.

## Pre-Implementation Gate

Status: ready-for-implementation

Problem / root-cause model:

- The L3 cgroups cache is a mutable client-owned object whose refresh operation swaps owned backing storage.
- The C API exposes both the cache struct fields and borrowed pointers into the mutable backing storage.
- Existing project guidance documents single-owner/external synchronization, but the C header and getting-started example do not make that rule prominent at the point of use.
- Netdata currently avoids the proven dangerous pattern by using one cgroup integration thread and joining it before cleanup, but it also reads public cache internals directly.
- The root issue is therefore contract clarity and API shape, not a confirmed current Netdata eBPF crash.

Evidence reviewed:

- `src/libnetdata/netipc/include/netipc/netipc_service.h:550`
- `src/libnetdata/netipc/include/netipc/netipc_service.h:607`
- `src/libnetdata/netipc/src/service/netipc_service_cgroups_cache_common.c:149`
- `src/libnetdata/netipc/src/service/netipc_service_cgroups_cache_common.c:179`
- `src/libnetdata/netipc/src/service/netipc_service_cgroups_cache_common.c:197`
- `src/libnetdata/netipc/src/service/netipc_service_cgroups_cache_common.c:209`
- `docs/netipc-integrator-skill.md:373`
- `docs/netipc-integrator-skill.md:1019`
- `docs/getting-started.md:290`
- `src/crates/netipc/src/service/raw/cgroups_cache.rs:90`
- `src/crates/netipc/src/service/raw/cgroups_cache.rs:166`
- `src/go/pkg/netipc/service/raw/cgroups_cache.go:73`
- `src/go/pkg/netipc/service/raw/cgroups_cache.go:154`
- `netdata/netdata @ 120ca90bf6a9d2c68fa093e2fd95b9456be8aeec src/collectors/ebpf.plugin/ebpf_cgroup.c:15`
- `netdata/netdata @ 120ca90bf6a9d2c68fa093e2fd95b9456be8aeec src/collectors/ebpf.plugin/ebpf_cgroup.c:360`
- `netdata/netdata @ 120ca90bf6a9d2c68fa093e2fd95b9456be8aeec src/collectors/ebpf.plugin/ebpf_cgroup.c:428`
- `netdata/netdata @ 120ca90bf6a9d2c68fa093e2fd95b9456be8aeec src/collectors/ebpf.plugin/ebpf.c:1130`

Affected contracts and surfaces:

- C public API:
  - `nipc_cgroups_cache_t`
  - `nipc_cgroups_cache_refresh()`
  - `nipc_cgroups_cache_lookup()`
  - `nipc_cgroups_cache_status()`
  - `nipc_cgroups_cache_close()`
- Rust raw and public cgroups snapshot cache facades.
- Go raw and public cgroups snapshot cache facades.
- `docs/getting-started.md`
- `docs/netipc-integrator-skill.md`
- C/Rust/Go tests for cgroups cache lifetime, status, refresh, and lookup behavior.
- Netdata eBPF cgroup integration vendored usage.

Existing patterns to reuse:

- The existing integrator skill already states the single-owner/external synchronization model.
- Rust's `&mut self` refresh pattern is the safest existing language model and should guide any cross-language contract wording.
- Go's lookup value-copy behavior is useful for avoiding borrowed object lifetime exposure.
- Existing cache tests cover refresh success/failure, lookup hit/miss, status, reconnect, and malformed snapshot handling; they can be extended with explicit contract tests if needed.

Risk and blast radius:

- Documentation-only changes have low runtime risk but leave the C and Go APIs easy to misuse.
- Adding a new safe snapshot/copy API has medium implementation and review cost, but reduces future integration risk.
- Changing existing C lookup semantics or hiding public struct fields can be source/ABI-impacting and may require coordinated Netdata vendoring.
- Adding only internal locks without changing borrowed-pointer semantics is a false-safety risk and should not be presented as a complete fix.

Sensitive data handling plan:

- This work should not require secrets, credentials, bearer tokens, SNMP communities, customer data, personal data, private endpoints, or proprietary incident details.
- SOW evidence will use only repository paths, line numbers, and sanitized technical summaries.
- Netdata evidence will be cited as upstream repository plus commit and relative paths, not workstation absolute paths.

Implementation plan:

1. Finalize the C/Rust/Go API names and compatibility path for the selected guarded snapshot-cache contract.
2. Add a read-guard API. Every L3 read accessor must require a caller-held guard so `use/get` and `copy/dup` share one homogeneous access model.
3. Add borrowed `use/get` access that performs lookup and returns a pointer/view valid only until the read guard is released.
4. Add `copy/dup` access that accepts a found borrowed view and returns an owned allocated item, plus an explicit free API that accepts only the owned item type.
5. Implement writer refresh as build-new-snapshot-privately, publish under write synchronization, then retire old snapshot storage only when no guarded readers can use it.
6. Keep per-item copy-on-write/refcounting out of the initial design unless benchmark evidence later proves whole-snapshot guards are insufficient.
7. Audit Netdata's eBPF use against the chosen contract and either document it as acceptable or create the required Netdata follow-up SOW.
8. Run same-failure searches and the relevant C/Rust/Go cache tests.

Validation plan:

- Same-failure search:
  - `rg -n "nipc_cgroups_cache_(lookup|refresh|status|close)|\\.items|CgroupsCache|Cache\\)" src docs tests`
  - Netdata equivalent search for cgroups cache direct field access and lookup usage.
- C validation:
  - focused cache tests for lookup/refresh/status/close behavior and any new safe API.
- Rust validation:
  - cache tests proving the borrow/ownership contract remains correct or matching any new API.
- Go validation:
  - cache tests and, if locking/snapshot changes are selected, race-oriented tests where practical.
- Documentation validation:
  - check that header docs, getting-started examples, and integrator skill all state the same contract.
- SOW validation:
  - `bash .agents/sow/audit.sh`

Artifact impact plan:

- AGENTS.md: no workflow change expected.
- Runtime project skills: `docs/netipc-integrator-skill.md` likely needs update or confirmation because it is the primary integrator-facing runtime guidance.
- Specs: update `.agents/sow/specs/` only if this becomes a durable cross-cutting API contract not already represented in docs.
- End-user/operator docs: `docs/getting-started.md` and possibly API docs need updates if wording or API changes.
- End-user/operator skills: none expected unless exported Netdata integration guidance is updated from this work.
- SOW lifecycle: moved to `.agents/sow/current/` with `Status: in-progress` after the selected guarded snapshot-cache design had concrete C API names, ownership rules, and validation plan.

Open-source reference evidence:

- Netdata integration evidence:
  - `netdata/netdata @ 120ca90bf6a9d2c68fa093e2fd95b9456be8aeec`
  - `src/collectors/ebpf.plugin/ebpf_cgroup.c`
  - `src/collectors/ebpf.plugin/ebpf.c`
  - `src/collectors/ebpf.plugin/ebpf_process.h`
- No unrelated external OSS reference has been checked yet because this is an internal IPC API contract issue.

Open decisions:

- Decision 1: resolved on 2026-06-29. The selected `plugin-ipc` L3 cache contract is the long-term-best guarded snapshot-cache API.
- Decision 2: resolved on 2026-06-29. Netdata should migrate its eBPF cgroup cache import to the guarded API after vendoring this library change; the migration is tracked by a separate Netdata SOW because this repository must complete one SOW at a time.
- Decision 3: resolved on 2026-06-29. This work remains independent from SOW-0021 because it changes the L3 cache ownership/lifetime contract, not the Level 2 lookup scale design.
- Decision 4: resolved on 2026-06-29. C borrowed and owned item access use different public types: borrowed `View` from `get/use`, owned `Item` from `dup/copy`, and `free` accepts only owned `Item`.

## Implications And Decisions

1. `plugin-ipc` L3 cache contract

- Option A - Surgical: keep the existing single-owner/external synchronization contract.
  - Work: strengthen C header docs, getting-started examples, integrator skill wording, and tests to make the borrowed-pointer and no-concurrent-refresh rule explicit.
  - Pros: low API churn, low implementation risk, aligns with existing `docs/netipc-integrator-skill.md`.
  - Cons: C and Go remain easy to misuse if an integrator ignores the contract.
  - Implication: Netdata must ensure every use of one cache instance is serialized or externally synchronized.
  - Risk: future C code may still hold `nipc_cgroups_cache_item_t *` across refresh/close.

- Option B - Long-term-best: add a safe snapshot/copy API and make it the recommended integration surface.
  - Work: introduce explicit stable snapshot or copy/iteration semantics across C, Rust, and Go; update docs and tests; migrate Netdata to the safer surface.
  - Pros: reduces future integration hazards, avoids relying on direct public struct fields, gives Netdata a clearer bulk-read API.
  - Cons: larger API and test surface; may need source/ABI planning if existing public structs remain.
  - Implication: Netdata should not depend on `cache.items` directly once the safer API exists.
  - Risk: a partial C-only fix would create cross-language contract drift.

- Option C - Not recommended: add internal locks only while keeping borrowed pointer lookup as-is.
  - Work: protect refresh/status/lookup/close internals with a mutex.
  - Pros: may reduce some data races for simple status/lookup operations.
  - Cons: does not make returned borrowed pointers safe after another thread refreshes or closes the cache.
  - Implication: users may incorrectly infer thread safety.
  - Risk: false safety; likely worse than explicit single-owner semantics.

Recommendation: Option B for long-term-best if Netdata expects this cache to become a shared or long-lived integration primitive. Option A is acceptable as a surgical fix only if Netdata keeps the current single integration thread pattern and treats direct cache internals as local, reviewed, serialized code.

Selected direction as of 2026-06-29: Option B, with these user-selected constraints:

- The L3 snapshot cache should be thread-safe and high-performance for concurrent use.
- Public reads use a homogeneous pattern: acquire read guard, access data, release read guard.
- `use/get` is the borrowed access API. It performs the lookup and returns a borrowed immutable `View`. The result is valid only while the caller holds the read guard.
- `copy/dup` is not interchangeable with `use/get`. It accepts a borrowed `View` returned by `use/get` and returns an allocated owned `Item`.
- `copy/dup` is therefore conceptually `lock -> use/get -> copy/dup -> unlock -> use owned Item`.
- The allocated copy survives unlock and requires an explicit free API.
- In C, `free` must accept only the owned `Item` type, not the borrowed `View` type. Normal code should fail at compile time if it tries to free a borrowed view.
- The C borrowed view should expose immutable fields such as `const char *name` and `const char *path`; the owned item should own mutable `char *name` and `char *path`.
- Debug/test builds may add ownership checks such as allocation magic/header validation, but the primary safety boundary is distinct public types.
- The initial design should avoid per-item copy-on-write/refcounted survival because it would add allocations and complexity on every snapshot refresh.
- The implementation must preserve C/Rust/Go semantic parity and avoid hidden locking models that make the access rules differ by function.

2. Netdata consumption pattern

- Option A - Surgical: keep current eBPF single-thread refresh/import loop, but document the invariant and avoid adding lookup/shared-cache consumers.
  - Pros: minimal Netdata change; current evidence does not show a live eBPF race.
  - Cons: preserves direct `cache.items` coupling and requires future reviewers to notice the invariant.
  - Implication: Netdata must not call `lookup()` or read `items` from another thread without its own lock.
  - Risk: future changes can break the invariant silently.

- Option B - Long-term-best: expose or use an explicit IPC snapshot/iteration API, then migrate Netdata to import from that API.
  - Pros: removes direct field coupling and gives Netdata a clear safe usage pattern.
  - Cons: requires upstream API work and coordinated Netdata vendor/update work.
  - Implication: Netdata's cgroup integration becomes a consumer of an explicit stable API, not struct internals.
  - Risk: more work now, but less latent production risk.

Recommendation: Option B if this SOW selects the long-term-best IPC API. If the user selects the surgical IPC contract, use Netdata Option A and create a Netdata SOW that records the single-thread invariant and same-failure search.

3. Relationship to SOW-0021

- Option A - Keep independent.
  - Pros: this is about L3 cache ownership/lifetime, while SOW-0021 is about Level 2 lookup scale.
  - Cons: both may touch shared docs and tests, so sequencing must avoid conflicts.
  - Implication: complete or pause one before implementation work overlaps.
  - Risk: duplicated documentation edits if not coordinated.

- Option B - Merge into SOW-0021.
  - Pros: one large API-hardening pass.
  - Cons: SOW-0021 is already broad and paused; adding L3 cache lifetime could make it harder to finish.
  - Implication: the cache fix waits behind scale work.
  - Risk: a small but important hazard remains unresolved longer.

Recommendation: Option A, independent SOW, unless the selected fix requires touching the same C/Rust/Go service internals that SOW-0021 will already restructure.

## Plan

1. Finalize the concrete Rust/Go API names, ownership rules, and source/ABI transition plan for the selected guarded snapshot-cache design.
2. Move this SOW to `current/` and set `Status: in-progress` only after the implementation plan is concrete enough to avoid redesign during coding.
3. Implement the guarded snapshot-cache API, borrowed `use/get` returning immutable views, allocated `copy/dup` returning owned items, explicit free for owned items only, and writer publish/retire mechanics.
4. Verify Netdata usage against the chosen contract and create/update the Netdata-side SOW if needed.
5. Validate with same-failure searches, relevant tests, docs review, concurrency/race validation, and SOW audit.

## Execution Log

### 2026-06-27

- Created pending SOW from the verified finding.
- Recorded that current Netdata eBPF usage does not prove a live race, but the C cache API exposes a latent borrowed-pointer/lifetime hazard.
- Recorded that `plugin-ipc` already documents single-owner cache semantics in `docs/netipc-integrator-skill.md`, but not prominently enough in the C header and getting-started example.

### 2026-06-29

- User selected the long-term-best direction: L3 snapshot cache should support high-performance concurrent use.
- Recorded the homogeneous read model: caller acquires a read guard, uses borrowed access or copies from a borrowed result, then releases the guard.
- Recorded that `copy/dup` accepts an already found borrowed item/view and returns an allocated deep copy that survives unlock and must be freed explicitly.
- Recorded that per-item copy-on-write/refcounting is not the initial design target because it increases allocation and refresh complexity.
- User approved the distinct borrowed `View` and owned `Item` API shape: `get/use` returns a borrowed immutable view; `dup/copy` accepts that view and returns an allocated owned item; `free` accepts only owned items.
- Moved the SOW to `current/` and marked it `in-progress`.
- Concrete C API plan:
  - `nipc_cgroups_cache_read_guard_t`
  - `nipc_cgroups_cache_read_lock()`
  - `nipc_cgroups_cache_read_unlock()`
  - `nipc_cgroups_cache_item_view_t`
  - `nipc_cgroups_cache_get()`
  - `nipc_cgroups_cache_item_dup()`
  - `nipc_cgroups_cache_item_free()`
  - existing scalar status/ready helpers remain thread-safe copy helpers, not borrowed-data access.
- Implemented the guarded cache contract in C:
  - `nipc_cgroups_cache_t` now owns an opaque immutable snapshot pointer, retained response buffer, counters, and synchronization primitives.
  - Refresh builds a new snapshot privately, serializes writers, publishes under a write lock, and frees the retired snapshot only after guarded readers drain.
  - Borrowed access uses `nipc_cgroups_cache_read_lock()`, `nipc_cgroups_cache_get()`, and `nipc_cgroups_cache_read_unlock()`.
  - Owned access uses `nipc_cgroups_cache_item_dup()` from a borrowed view and `nipc_cgroups_cache_item_free()` for owned items only.
- Implemented matching Rust semantics:
  - raw cache uses a writer `Mutex`, snapshot `RwLock`, borrowed `CgroupsCacheItemView<'a>`, and `CgroupsCacheReadGuard<'a>`.
  - public cgroups snapshot facade exposes guard-based access and duplication.
- Implemented matching Go semantics:
  - raw cache uses a writer mutex plus snapshot `RWMutex`.
  - `CacheReadGuard.Get()` returns a borrowed immutable view valid until `Unlock()`.
  - `CacheReadGuard.Dup()` returns an owned `CacheItem` value.
- Updated C, Rust, Go interop fixtures and benchmarks to use the guard/get/dup/free model.
- Added concurrent-reader/refresh coverage for same-cache L3 use in C, Rust, and Go.
- Updated `docs/getting-started.md`, `docs/level3-snapshot-api.md`, and `docs/netipc-integrator-skill.md`.
- Simplified C apps/cgroups lookup zero-item request sizing to assign the fixed header size directly; the previous overflow branch was unreachable and caused avoidable coverage noise.
- Created the Netdata follow-up SOW for eBPF cgroup cache migration:
  - `~/src/netdata-ktsaou.git/.agents/sow/q/pending/SOW-20260629-plugin-ipc-guarded-cgroups-cache-vendor.md`

## Validation

Acceptance criteria evidence:

- Chosen contract recorded:
  - Guarded snapshot-cache contract is recorded in `## Implications And Decisions`.
  - C API uses distinct borrowed `nipc_cgroups_cache_item_view_t` and owned `nipc_cgroups_cache_item_t` types.
  - `nipc_cgroups_cache_item_free()` accepts only owned items, so ordinary C code cannot pass a borrowed view without a type error.
- C, Rust, and Go cache APIs were updated to the same contract:
  - lock/read guard first;
  - `get/use` performs lookup and returns borrowed immutable data valid only under the guard;
  - `dup/copy` accepts the borrowed result and returns owned data;
  - writers publish a new snapshot while readers hold stable old snapshots until guard release.
- C public header, getting-started docs, integrator guidance, tests, interop fixtures, and benchmarks were updated.
- Netdata current eBPF direct-cache-field usage is classified as acceptable only under its existing serialized import loop and not acceptable as the long-term API target. A Netdata follow-up SOW now tracks migration to the guarded API after vendoring.
- Same-failure scans were run after implementation. The plugin-ipc tree has no stale direct lookup/internal-field uses under `src`, `tests`, `bench`, or `docs`; Netdata eBPF direct-field uses remain and are mapped to the Netdata follow-up SOW.

Tests or equivalent validation:

- POSIX build:
  - `cmake --build build`
  - Result: passed after final C cleanup.
- POSIX full CTest:
  - `/usr/bin/ctest --test-dir build --output-on-failure`
  - Result: 48/48 tests passed, total 477.07 seconds.
  - Note: plain `ctest` resolves to a broken local wrapper in this environment, so `/usr/bin/ctest` is the validated command.
- POSIX C coverage:
  - `bash tests/run-coverage-c.sh`
  - Result: passed.
  - Total: 91.9% (`2482/2702`).
  - Key files: `netipc_service_apps_lookup.c` 90.8%, `netipc_service_cgroups_lookup.c` 90.4%.
- POSIX Go coverage:
  - `bash tests/run-coverage-go.sh`
  - Result: passed before the final C-only cleanup.
  - Total: 90.0% (`3643/4050`).
  - Key files: `service/raw/cgroups_cache.go` 93.7%, `protocol/cgroups_snapshot.go` 91.6%.
- POSIX Go full tests:
  - `go test ./...`
  - Result: passed before the final C-only cleanup.
- POSIX Go race check:
  - `go test -race ./pkg/netipc/service/raw -run TestCacheConcurrentReadersRefresh`
  - Result: passed before the final C-only cleanup.
- POSIX Rust coverage:
  - `bash tests/run-coverage-rust.sh`
  - Result: passed before the final C-only cleanup.
  - Rust line coverage: 92.49%.
- POSIX Rust full tests:
  - `cargo test --manifest-path src/crates/netipc/Cargo.toml`
  - Result: passed before the final C-only cleanup.
  - Coverage: 376 library tests plus binary/doc tests.
- POSIX benchmark smoke:
  - `./build/bin/bench_posix_c lookup-bench 1`
  - Result: `lookup,c,c,35585205,0,0,0,99.1,0.0,99.1`
  - `./build/bin/bench_posix_go lookup-bench 1`
  - Result: `lookup,go,go,95029677,0,0,0,99.2,0.0,99.2`
  - `cargo build --manifest-path src/crates/netipc/Cargo.toml --release --bin bench_posix && ./src/crates/netipc/target/release/bench_posix lookup-bench 1`
  - Result: `lookup,rust,rust,98820595,0,0,0,99.0,0.0,99.0`
- Windows build and CTest:
  - Existing `win11` checkout was dirty and on a different commit, so validation used scratch tree `/tmp/plugin-ipc-sow25-test` created from `git archive HEAD` plus this SOW's tracked diff.
  - `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build -j4`
  - Result: passed.
  - `ctest --test-dir build --output-on-failure -j4`
  - Result: 25/25 tests passed, total 313.99 seconds.
- Windows MSYS validation:
  - `bash tests/run-windows-msys-validation.sh /tmp/netipc-sow25-msys-validation 3`
  - Result: passed.
  - Functional slice passed, including `test_cache_win_interop` and `test_cache_win_shm_interop`.
  - Benchmark policy initially retried one `shm-max` row, then final policy passed.
  - Artifacts: `/tmp/netipc-sow25-msys-validation/summary.txt`, `policy.csv`, `joined.csv`, `summary.csv`.
- Windows C coverage:
  - `bash tests/run-coverage-c-windows.sh`
  - Result: passed.
  - Total: 92.7% (`3102/3348`).
  - Key files: `netipc_service_apps_lookup.c` 90.4%, `netipc_service_cgroups_lookup.c` 90.1%.
- Windows Go coverage:
  - `bash tests/run-coverage-go-windows.sh`
  - Result: passed.
  - Total: 90.0% (`3675/4083`).
  - Key file: `service/raw/cgroups_cache.go` 90.2%.
- Windows Rust coverage:
  - `bash tests/run-coverage-rust-windows.sh`
  - Result: passed.
  - Total line coverage: 91.61%.
  - Key files: `service\cgroups_snapshot.rs` 98.06%, `transport\windows.rs` 92.09%, `transport\win_shm.rs` 94.40%.

Real-use evidence:

- POSIX and Windows interop tests covered C, Rust, and Go cache clients against C, Rust, and Go servers.
- Windows validation used a scratch checkout because the user's normal `win11` checkout had unrelated dirty state. No changes were made to that checkout.
- Netdata eBPF integration was inspected as the known downstream consumer. It still directly reads cache internals and is therefore tracked by the Netdata follow-up SOW before this SOW closes.

Reviewer findings:

- No external reviewers were run because the user did not request external reviews for this milestone.
- Self-review findings handled during validation:
  - Windows C coverage initially exposed missing helper-wrapper coverage in the Windows C fixture; fixed by adding `test_lookup_remaining_timeout_helpers()`.
  - Windows C coverage then showed one cgroups lookup file below threshold by one line; fixed by removing unreachable zero-item request-size overflow branches in apps/cgroups lookup clients.
  - The accidental scratch-only copy of `test_win_service_extra.c` into the Windows source directory was removed from `/tmp/plugin-ipc-sow25-test`; it never touched the repository.

Same-failure scan:

- Plugin-ipc stale API scan:
  - Command:
    - `rg -n 'nipc_cgroups_cache_lookup|\.lookup\(|\.Lookup\(|cache\.items|cache\.item_count|cache\.buckets|cache\.bucket_count|cache\.response_buf' src tests bench docs -g '!*.o'`
  - Result: no matches.
- Netdata eBPF downstream scan:
  - Command in `~/src/netdata-ktsaou.git`:
    - `rg -n 'nipc_cgroups_cache_lookup|cgroups_cache.*(items|item_count|generation|systemd_enabled|response_buf)|\.items|\.item_count|\.generation|\.systemd_enabled' src/collectors/ebpf.plugin -g '*.[ch]'`
  - Result: direct-field uses remain in `src/collectors/ebpf.plugin/ebpf_cgroup.c` at lines 368, 369, 372, 376, and 429.
  - Handling: mapped to `~/src/netdata-ktsaou.git/.agents/sow/q/pending/SOW-20260629-plugin-ipc-guarded-cgroups-cache-vendor.md`.

Sensitive data gate:

- No sensitive data was needed or recorded. Evidence uses repository paths, line numbers, and sanitized technical summaries.

Artifact maintenance gate:

- AGENTS.md: no update needed. The repo workflow, SOW rules, and project-wide guardrails did not change.
- Runtime project skills: no `.agents/skills/project-*` runtime skill exists in this repo, so no runtime project skill update was required.
- Specs: `docs/level3-snapshot-api.md` was updated as the authoritative public L3 cache API/spec surface. No `.agents/sow/specs/` update was needed because the durable public contract is now captured in the docs surface.
- End-user/operator docs: `docs/getting-started.md` was updated to show guarded get/dup/free usage.
- End-user/operator skills: `docs/netipc-integrator-skill.md` was updated because downstream assistants/operators consume it as integration guidance.
- SOW lifecycle: this SOW remained the only active SOW during implementation. SOW-0026 stayed pending and untouched. Netdata follow-up work is represented by a real pending Netdata SOW.

Specs update:

- `docs/level3-snapshot-api.md` now documents the guarded snapshot-cache contract, borrowed view lifetime, duplication, free semantics, and concurrency expectations.

Project skills update:

- No runtime project skill update was needed because the project skills index says there are no runtime input project skills yet.

End-user/operator docs update:

- `docs/getting-started.md` now shows lock/get/dup/unlock/free usage for C cache access.

End-user/operator skills update:

- `docs/netipc-integrator-skill.md` now describes the guard/use/dup/free access model and the C/Rust/Go API surfaces.

Lessons:

- Initial lesson: a documented single-owner contract can still be too easy to miss when the C header and example expose borrowed pointers without repeating the concurrency boundary at the point of use.
- Design lesson: a homogeneous guard-based access model is easier to teach than separate hidden-lock and exposed-lock APIs; borrowed data is only valid under the guard, while duplicated data survives and must be explicitly freed.
- Validation lesson: coverage pressure exposed a dead zero-item request-size branch in the C lookup clients. Removing impossible defensive branches can be better than adding synthetic test hooks.

Follow-up mapping:

- Netdata eBPF migration is tracked by:
  - `~/src/netdata-ktsaou.git/.agents/sow/q/pending/SOW-20260629-plugin-ipc-guarded-cgroups-cache-vendor.md`
- SOW-0026 remains a separate pending plugin-ipc SOW and was not executed in this SOW.

## Outcome

Implemented.

The L3 cgroups snapshot cache now has a thread-safe guarded access contract across C, Rust, and Go. Readers hold read guards while using borrowed views. Writers can refresh by building a new snapshot and publishing it without invalidating active guarded readers. Callers that need data beyond the guard can duplicate the borrowed view into an owned item/value and free it through the owned-data API.

## Lessons Extracted

- Distinct borrowed and owned public types make misuse harder than documentation alone, especially in C.
- Guard-based reads keep the API homogeneous: callers always lock before access, whether they intend to borrow or duplicate.
- Whole-snapshot publication under a read/write lock is a simple and performant first step. Per-item copy-on-write/refcounting remains unnecessary without benchmark evidence that the guard model is too coarse.
- Downstream direct-field consumers must be migrated deliberately because making the upstream cache safer does not automatically fix vendored users that bypass the safe API.

## Followup

- Netdata follow-up SOW:
  - `~/src/netdata-ktsaou.git/.agents/sow/q/pending/SOW-20260629-plugin-ipc-guarded-cgroups-cache-vendor.md`
- SOW-0026 remains pending and separate.

## Regression Log

None yet.

Append regression entries here only after this SOW was completed or closed and later testing or use found broken behavior. Use a dated `## Regression - YYYY-MM-DD` heading at the end of the file. Never prepend regression content above the original SOW narrative.

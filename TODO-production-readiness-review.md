## Purpose

Final production-readiness review for `plugin-ipc` before deployment to 1.5M+ Netdata installations daily. The goal is to verify end-to-end correctness, safety, spec compliance, resilience, and test coverage across C, Rust, Go, and all provided tests, and to identify any remaining issues that could block production.

## TL;DR

- Review all files under `docs/` as the specification source.
- Review all C implementation files under `src/libnetdata/netipc/include/netipc/*.h` and `src/libnetdata/netipc/src/**/*.c`.
- Review all Rust implementation files under `src/crates/netipc/src/**/*.rs`.
- Review all Go implementation files under `src/go/pkg/netipc/**/*.go`.
- Review all tests under `tests/**/*`.
- Do not make code changes. Deliver a brutally honest production-readiness review.

## Analysis

- Initial inventory completed for `docs/`, C, Rust, Go, and `tests/`.
- Existing unrelated untracked files are present in the worktree and must not be touched.
- Verified findings:
- Blocking: SHM session isolation is broken across languages. Handshake selects SHM per session, but service layers create at most one SHM region per service while clients still attach to any existing region on disk/object namespace. This can route multiple clients through the same SHM channel and violate negotiated transport selection.
- Blocking: protocol violation handling is incomplete. Service layers skip unexpected non-request messages instead of terminating the session, and client typed-call paths do not validate that the received header is actually the expected response kind/code/message_id before decoding.
- Medium: Rust and Go Level 3 cache lookup is linear scan, not O(1) hash lookup as required by the spec and by the intended hot-path use case.
- Medium: Level 3 helpers hard-code a 64 KiB response buffer instead of deriving it from configured/negotiated response limits. Tests work around this by mutating internal buffers directly.
- Medium: codec decoders accept overlapping `name` / `path` string regions even though the codec spec requires overlap rejection where the schema forbids it.
- Test gaps:
- No test explicitly verifies SHM session isolation for multiple SHM-capable clients using distinct per-client payloads.
- No test explicitly verifies that unexpected message kinds/codes terminate the session.
- No test explicitly verifies rejection of overlapping variable-length fields in cgroups response items.

## Decisions

- No user decisions required yet.

## Plan

1. Read every file in `docs/` to extract protocol, transport, API, safety, and error-handling requirements.
2. Review C headers and source for spec coverage, unsafe memory behavior, input validation, crash paths, and transport-specific edge cases.
3. Review Rust sources for protocol fidelity, unsafe blocks, FFI boundaries, transport safety, and panic/error propagation.
4. Review Go sources for protocol fidelity, bounds checking, concurrency safety, and transport/service correctness.
5. Review all tests to identify coverage gaps relative to the specification and implementation risk surface.
6. Cross-check findings across languages for interoperability, consistency, and hidden divergence from the written spec.
7. Deliver a production recommendation with clear blocking/non-blocking findings and residual risks.

Status: completed. Final recommendation is to reject production deployment until the blocking issues are fixed and the corresponding tests exist.

## Implied Decisions

- Review scope is not limited to the 7 previously fixed issues.
- Production-readiness standard is strict: any plausible crash, corruption, security issue, spec mismatch, or serious test gap must be called out.
- Performance concerns are in scope when they could affect a core Netdata component at scale.

## Testing Requirements

- Verify whether the current tests cover malformed inputs, boundary sizes, concurrency/race scenarios, transport teardown, partial I/O, and cross-language interoperability.
- Identify missing tests that could hide production bugs, even if the current suite passes.
- Note whether sanitizer, valgrind, fuzzing, and race tooling materially cover the highest-risk paths.

## Documentation Updates Required

- Verify whether implementation behavior matches the spec in `docs/`.
- Call out any spec ambiguity, contradiction, or undocumented implementation behavior that could mislead maintainers or integrators.

# SOW-0004 - CGROUPS_LOOKUP and APPS_LOOKUP netipc methods

## Status

Status: completed

Completed on 2026-05-25 after implementation, validation, benchmark
generation, and the corrected post-implementation reviewer gate. A
DeepSeek reviewer run that used the wrong model was discarded and does
not count toward the gate; the final counted DeepSeek pass used
`deepseek/deepseek-v4-pro`. The codec specification documents under
`docs/codec-cgroups-lookup.md` and `docs/codec-apps-lookup.md` are now
the authoritative wire-format sources; the SOW inline format remains
the original design intent.

## Requirements

### Purpose

Add two new netipc methods to `plugin-ipc` that enable a pull-based,
cache-driven enrichment chain for Netdata's plugin ecosystem:

- `CGROUPS_LOOKUP` allows a client to ask for metadata of specific cgroup
  paths it has discovered (rather than receiving a full snapshot).
- `APPS_LOOKUP` allows a client to ask for per-PID metadata, with cgroup
  information already joined server-side.

The pull/lookup pattern matches the receiver's natural working set in each
plugin:

- `network-viewer.plugin` only cares about PIDs that own sockets.
- `apps.plugin` only cares about cgroups that contain live PIDs.

Each receiver caches what it has learned, queries the sender only for
items it does not yet know, and discards entries that drop out of the
working set at the end of each iteration. This avoids the data volume of
full snapshot transfers while preserving the lifecycle correctness of
the snapshot model.

This SOW is the protocol + reference implementation work. The Netdata-side
integration (refactoring cgroup detection into `libnetdata`, wiring the
new methods into `cgroups.plugin`, `apps.plugin`, and
`network-viewer.plugin`) is tracked separately as a Netdata-side SOW
(SOW-B) referenced in this SOW's Followup section.

### User Request and Confirmed Decisions

The user asked for two new netipc methods to enable a pull-based, cached
enrichment chain across `cgroups.plugin`, `apps.plugin`, and
`network-viewer.plugin`. After multiple rounds of design discussion and
multi-agent review of three earlier drafts, the user confirmed the
following decisions. This is the **single authoritative decision list**
for the SOW.

#### Core structural decisions (round 0-1)

1. **Status enums per method**.
   - `CGROUPS_LOOKUP` per-item status: `0` KNOWN, `1` UNKNOWN_RETRY_LATER,
     `2` UNKNOWN_PERMANENT. Defined by enum `nipc_cgroup_lookup_status_t`.
   - `APPS_LOOKUP` per-item PID status: `0` KNOWN, `1` UNKNOWN. Defined by
     enum `nipc_pid_lookup_status_t`.
   - Clients detect PID reuse via `comm` comparison; no
     `STARTTIME_MISMATCH` status on the wire.
2. **Shared orchestrator enum** in `netipc_protocol.h`. Values:
   `0` UNKNOWN, `1` SYSTEMD, `2` DOCKER, `3` K8S, `4` KVM, `5` LXC,
   `6` PODMAN, `7` NSPAWN. Mirrored in Rust and Go.
3. **All labels** from `cg->chart_labels` are emitted. No whitelist.
4. **starttime** returned in `APPS_LOOKUP` responses as a transparency
   field. The request only carries `pid`. The server returns its current
   truth and never compares against client-supplied data.
5. **net_ns_inode** is not on the wire. `network-viewer.plugin` is
   expected to drop its own use of it after the Netdata integration
   completes.
6. **Method codes**: `CGROUPS_LOOKUP = 4`, `APPS_LOOKUP = 5`. Added to
   `src/libnetdata/netipc/include/netipc/netipc_protocol.h` alongside
   the existing `NIPC_METHOD_*` defines.
7. **Single SOW** covers C, Rust, Go, codec docs, tests, and benchmarks.
8. **Test coverage** includes wire-format encode/decode in all languages,
   cross-language fixtures for every producer/consumer pair, validation
   rejection tests, fuzz tests (matching the existing snapshot codec
   requirement), edge round-trip tests, and benchmarks recorded in
   `benchmarks-posix.md` and `benchmarks-windows.md`.
9. **Host-root cgroup handling**: the wire never carries a `HOST_ROOT`
   value in `CGROUPS_LOOKUP`. The Netdata-side `apps.plugin` detects when
   `/proc/<pid>/cgroup` resolves to the root cgroup (v2 line `0::/`; v1
   with all relevant controllers at `/`) and caches the PID locally as a
   host process without ever calling `CGROUPS_LOOKUP`. In the
   `APPS_LOOKUP` response, `cgroup_status = HOST_ROOT` flags this case
   explicitly (see decision 17).
10. **v1 canonical-path precedence**: for `/proc/<pid>/cgroup` parsing on
    cgroup v1 systems, the Netdata-side `apps.plugin` prefers the
    `cpuacct` controller line, falling back to `blkio`, then `memory`.
    This mirrors `cgroups.plugin`'s discovery walk order in
    `discovery_find_all_cgroups_v1` (Netdata
    `src/collectors/cgroups.plugin/cgroup-discovery.c:782`). No functional
    change introduced by `apps.plugin`. The protocol does not depend on
    this precedence; recorded here because it bounds the cross-plugin
    matching guarantee.
11. **Scope split**: this SOW (SOW-A) covers protocol + 3-language
    implementation + docs + tests + benchmarks only. The Netdata-side
    integration is SOW-B (see Followup).
12. **libnetdata module location** (Netdata-side, recorded here for
    SOW-B): `src/libnetdata/cgroups/`. New module. Receives the shared
    cgroup mode/path detection helpers extracted from `cgroups.plugin`.
13. **No functional change** in the libnetdata extraction (SOW-B). The
    extracted code preserves the current discovery behavior bit-for-bit;
    the refactor is purely structural.
14. **Isolated single commit** (Netdata-side constraint, recorded here for
    SOW-B): the extraction of cgroup detection from `cgroups.plugin` into
    `src/libnetdata/cgroups/` must land as a single commit, isolated from
    any other changes. The commit must be reviewable on its own merit. No
    other functional or unrelated changes may be mixed into that commit.
15. **APPS_LOOKUP response variable area uses offset+length for all
    strings** (round 1 / C1: option A). The fixed item header is **60
    bytes** and carries `comm_offset/length`, `cgroup_path_offset/length`,
    `cgroup_name_offset/length`, and `label_count`. The variable area is
    pure NUL-terminated strings + a contiguous label entry table + label
    key/value strings. This matches the `CGROUPS_LOOKUP` pattern exactly.
16. **CGROUPS_LOOKUP request keys are just NUL-terminated paths**
    (round 1 / C6: option B). Each request key in the packed area is
    `path bytes + NUL`. No per-key mini-header.
17. **APPS_LOOKUP response carries an explicit `cgroup_status` field**
    (round 1 / C8: option A). The field is a `u16` with values:
    - `0` KNOWN (cgroup metadata present: path + name + labels +
      orchestrator)
    - `1` UNKNOWN_RETRY_LATER (cgroup path known; cgroups.plugin has
      not resolved metadata yet)
    - `2` UNKNOWN_PERMANENT (cgroup path known; cgroups.plugin returned
      a permanent failure)
    - `3` HOST_ROOT (PID is in the root cgroup; apps.plugin
      short-circuited and never queried cgroups.plugin)

    This lets `network-viewer.plugin` decide cache lifecycle per cgroup
    state: re-query on RETRY_LATER, reuse PERMANENT and HOST_ROOT
    entries without retry during the current provider generation, use
    full data on KNOWN, and apply the generation-reset invalidation in
    decision 22.
18. **Decoder rules for unknown enum values**:
    - **Status fields** (`nipc_cgroup_lookup_status_t`,
      `nipc_pid_lookup_status_t`, `nipc_apps_cgroup_status_t`): decoder
      MUST reject unknown values with `NIPC_ERR_BAD_LAYOUT`. Adding a new
      status requires a layout-version bump.
    - **Orchestrator enum**: decoder MUST accept any `u16` value. Unknown
      values are surfaced to the application layer as a raw `u16`; the
      application layer treats unknown values as `UNKNOWN` for display
      and aggregation. This preserves forward compatibility for new
      orchestrator types.

#### Round-2 design decisions

19. **String encoding model: always emit NUL, length-only "absent"
    signal** (round 2 / CR1: option A). Every string field always has a
    valid `offset` (`>= fixed_item_header_size`) and a NUL terminator.
    `length == 0` means empty string; the NUL is still present at
    `byte[offset]`. "Absent" is expressed by `length == 0`, never by
    `offset == 0`. Each string field, even when `length == 0`, occupies
    its own NUL byte at byte `offset`; multiple zero-length strings
    CANNOT share a NUL byte (the overlap rule rejects shared byte
    ranges). Applies to every string field in every method: `path`,
    `name`, `comm`, `cgroup_path`, `cgroup_name`, label keys, label
    values.
20. **Field semantics derive from `status` and `cgroup_status`**. With
    decision 19, all fields below carry a valid offset and NUL; only
    `length` varies.
    - `APPS_LOOKUP status == UNKNOWN` (PID not tracked): encoder MUST
      set `orchestrator = 0`, `cgroup_status = 0`, `ppid = 0`,
      `uid = NIPC_UID_UNSET`, `starttime = 0`, `comm_length = 0`,
      `cgroup_path_length = 0`, `cgroup_name_length = 0`, `label_count
      = 0`. All offsets MUST still be `>= 60`.
    - `APPS_LOOKUP status == KNOWN`:
      - `comm_length >= 1` (every live process has a comm).
      - `cgroup_status == KNOWN`: `cgroup_path_length >= 1`,
        `cgroup_name_length` may be `0` if no friendly name resolved,
        labels reflect `cg->chart_labels`, `orchestrator` is meaningful.
      - `cgroup_status == UNKNOWN_RETRY_LATER` or `UNKNOWN_PERMANENT`:
        `orchestrator = 0`, `cgroup_path_length >= 1` (raw path
        populated), `cgroup_name_length = 0`, `label_count = 0`.
      - `cgroup_status == HOST_ROOT`: `orchestrator = 0`,
        `cgroup_path_length = 0`, `cgroup_name_length = 0`,
        `label_count = 0`.
    - `CGROUPS_LOOKUP status == KNOWN`: `path_length >= 1` (echoed),
      `name_length` may be `0` if no friendly name, `label_count` may be
      `0` if no labels, `orchestrator` is meaningful.
    - `CGROUPS_LOOKUP status == UNKNOWN_RETRY_LATER` or
      `UNKNOWN_PERMANENT`: `orchestrator = 0`, `path_length >= 1`
      (echo of request), `name_length = 0`, `label_count = 0`.
21. **`NIPC_UID_UNSET` constant** defined as `0xFFFFFFFFu` in
    `netipc_protocol.h`; mirrored as `NIPC_UID_UNSET: u32 = u32::MAX`
    in Rust (`src/crates/netipc/src/protocol/`) and as
    `NipcUIDUnset uint32 = math.MaxUint32` in Go
    (`src/go/pkg/netipc/protocol/`).
22. **`generation` field semantics**: a server-side monotonic counter
    scoped to the service instance. Advisory. Clients SHOULD track the
    last-seen `generation` per service. **Order of operations on each
    response receipt**: (1) check the new `generation`; (2) if decreased
    or reset (server restart detected), evict generation-scoped negative
    entries before processing the response: `UNKNOWN_PERMANENT` for both
    lookup caches, plus `HOST_ROOT` for the `APPS_LOOKUP` PID cache;
    (3) THEN process the response items, caching any new values
    (including new `UNKNOWN_PERMANENT` / `HOST_ROOT` entries from the
    same response). This order ensures fresh negative values in the same
    response as a generation change are preserved. Decoders MUST NOT
    reject any `generation` value, including `0`.
23. **`starttime` units**: Linux `jiffies` since system boot, matching
    the value in `/proc/<pid>/stat` field 22. Conversion to wall-clock
    time is the client's responsibility (typically via
    `sysconf(_SC_CLK_TCK)` and system uptime). Decoders accept any
    `u64` value including `0` and `UINT64_MAX`. On non-Linux platforms,
    `starttime` MUST be set to `0` by the encoder and has no semantic
    meaning; consumers MUST NOT interpret non-Linux `starttime` values.
24. **Empty request (`item_count == 0`)** explicitly allowed. Response
    also has `item_count == 0`. This is the no-op probe use case;
    encoders and decoders MUST handle it without error.
25. **Response order MUST equal request order**: for both methods,
    response item `N` corresponds to request item `N`. The wire decoder
    validates structural integrity only; the typed client SHOULD verify
    that the echoed path / pid at each index matches the request item.
    Mismatched echoes are a server bug; the client MUST flag and either
    discard the affected item or fail the whole response.
26. **Authoritative documentation source**: once written during
    implementation, `docs/codec-cgroups-lookup.md` and
    `docs/codec-apps-lookup.md` become the authoritative wire-format
    sources. The SOW inline format is the design intent that those docs
    must implement; any future drift is resolved in favor of the codec
    docs.
27. **Vendor drift acceptance**: SOW-A does not update the Netdata-side
    vendored copy of `plugin-ipc`. The vendor sync is part of SOW-B.
    `diff-netdata-vendor.sh` has `set -euo pipefail` but its `diff`
    invocations (lines 151-152) use `|| true` to suppress the diff
    exit status; the script prints drift to stdout and exits `0` even
    when drift is present. Drift must therefore be reviewed from the
    printed output, not from the exit code. The SOW-A acceptance
    criterion is: the script runs to completion (exits `0`), and the
    drift printed in its output is limited to the new method files
    (`cgroups_lookup`, `apps_lookup`, new enums, new constants); the
    drift summary is recorded in the SOW Execution Log as expected
    drift. SOW-B re-runs the sync after this SOW lands and is
    responsible for the final no-drift state.
28. **Request header padded to 16 bytes**. The request header for both
    methods is 16 bytes: `u16 layout_version`, `u16 flags`, `u32
    item_count`, `u32 reserved0`, `u32 reserved1`. The padding ensures
    the per-key directory starts at offset 16 and the packed key area
    starts at offset `16 + 8 * item_count`, both of which are 8-byte
    aligned for any `item_count`. The request header uses two `u32
    reserved` fields rather than a single `u64 reserved` for consistency
    with the per-item header convention. **The request and response
    headers are NOT identical**: at offsets 8-15, the request has
    `reserved0 + reserved1` while the response has `generation` (u64).
    The response header for the lookup methods (16 bytes) also differs
    from the existing `CGROUPS_SNAPSHOT` response header (24 bytes,
    with an extra `systemd_enabled` field and `generation` at offset
    16). The lookup methods do not need `systemd_enabled`; the
    orchestrator enum carries the equivalent information.
29. **Label entry table alignment**. The label entry table starts at the
    first 8-byte-aligned offset (relative to the item payload start, byte
    0 of the fixed item header) that is `>=` the end of all preceding
    strings (their NUL bytes inclusive, i.e., `max(string_offset +
    string_length + 1)` over all strings). The builder pads between the
    last string NUL and the label table with **zero bytes** as needed.
    Up to 7 padding bytes may appear. The label table itself is
    `label_count * 16` bytes; label key and value strings are packed
    immediately after the table in the order they appear in the table.
    **Decoders MUST compute the canonical table start** using the same
    formula and reject any payload where the label entries' offsets are
    inconsistent with the canonical position, OR where the padding bytes
    are non-zero. This guarantees byte-identical encoding across
    languages.
30. **Dispatch helpers return `nipc_error_t`**. The C dispatch helpers
    `nipc_dispatch_cgroups_lookup` and `nipc_dispatch_apps_lookup` return
    `nipc_error_t`, matching the existing `nipc_dispatch_cgroups_snapshot`
    signature. Rust and Go use each language's idiomatic error type
    (`Result<_, NipcError>` in Rust, `(Result, error)` in Go). The
    typed handler callback continues to return `bool` (false maps to
    `NIPC_ERR_HANDLER_FAILED`), matching the existing
    `nipc_cgroups_handler_fn`.
31. **Checked arithmetic for bounds checks**. All bounds checks
    involving `offset + length` AND all checks involving
    `count * element_size` (e.g., `item_count * NIPC_LOOKUP_DIR_ENTRY_SIZE`,
    `label_count * NIPC_LOOKUP_LABEL_ENTRY_SIZE`) MUST use checked
    arithmetic to detect overflow. In C: use a 64-bit intermediate
    (`(uint64_t)a + (uint64_t)b`, `(uint64_t)count * (uint64_t)size`).
    In Rust: use `checked_add` / `checked_mul`. In Go: convert to
    `uint64` before arithmetic. A malicious payload with `label_count ==
    0x4000` would compute `0x4000 * 16 == 0x40000` which truncates to
    `0` in `u16` arithmetic without this rule.
32. **C wire-size constants and `_Static_assert`**. Because the natural
    C struct alignment will pad the 60-byte APPS_LOOKUP item header to
    64 bytes (due to the `u64 starttime` field at offset 24), the C
    implementation MUST define explicit wire-size constants and MUST
    NOT use `sizeof(struct)` as the wire length:
    ```c
    #define NIPC_CGROUPS_LOOKUP_REQ_HDR_SIZE       16u
    #define NIPC_CGROUPS_LOOKUP_RESP_HDR_SIZE      16u
    #define NIPC_CGROUPS_LOOKUP_ITEM_HDR_SIZE      28u
    #define NIPC_APPS_LOOKUP_REQ_HDR_SIZE          16u
    #define NIPC_APPS_LOOKUP_RESP_HDR_SIZE         16u
    #define NIPC_APPS_LOOKUP_ITEM_HDR_SIZE         60u
    #define NIPC_LOOKUP_DIR_ENTRY_SIZE              8u
    #define NIPC_LOOKUP_LABEL_ENTRY_SIZE           16u
    #define NIPC_APPS_LOOKUP_KEY_SIZE               8u
    ```
    The C implementation MUST use `_Static_assert` to verify the offset
    of every wire-struct field, mirroring the existing snapshot codec
    pattern at `netipc_protocol.c:91-98`. For the 60-byte APPS_LOOKUP
    item header, the C implementation MUST NOT assert
    `sizeof(struct) == 60` because the natural alignment will produce
    `sizeof == 64`. Field offset assertions are sufficient.
    Encoders and decoders that operate field-by-field via `offsetof`
    are preferred over `memcpy` of a C struct that may include trailing
    padding.

#### Round-3 decisions

33. **Response strings MUST NOT contain interior NUL bytes**. For all
    string fields in responses (`path`, `name`, `comm`, `cgroup_path`,
    `cgroup_name`, label keys, label values), the bytes at `byte[offset]`
    through `byte[offset + length - 1]` MUST NOT contain any `\0` byte.
    The only permitted NUL is the terminator at `byte[offset + length]`.
    This matches the existing request-key rule (request paths reject
    interior NULs) and prevents C consumers using C-string semantics
    from silently truncating strings while Rust/Go length-based views
    see the full bytes. Encoders MUST validate this before emitting;
    decoders MUST reject violations.
34. **Server treats request paths/PIDs as opaque lookup keys**. The
    server MUST NOT resolve, normalize, or open filesystem paths based
    on request content. Path traversal characters (`..`) and absolute
    paths in a request key have no special meaning; they are matched
    byte-for-byte against the server's discovered cgroup set. The
    request `pid` field is treated as a numeric lookup key, not a
    privileged process reference. This bounds the security exposure of
    the lookup methods to: (a) the localhost-only transport and (b)
    the existing auth-token handshake. Documented in the codec specs'
    security considerations section.
35. **Benchmark and acceptance scope is Linux/POSIX for the new lookup
    methods**. The user clarified on 2026-05-25 that this feature's
    Netdata consumers are Linux-only: `cgroups.plugin` and
    `network-viewer.plugin`. Evidence: `netdata/netdata @
    1268945e2ca08a98e7d9eacae632d56d9e3c08d5`,
    `CMakeLists.txt:198`, `CMakeLists.txt:204`, and
    `CMakeLists.txt:2202` show the Linux-only build gates;
    `src/collectors/cgroups.plugin/sys_fs_cgroup.c:126` and
    `src/collectors/cgroups.plugin/sys_fs_cgroup.c:158` show
    `/sys/fs/cgroup` and `/proc/filesystems` use; and
    `src/collectors/network-viewer.plugin/network-viewer.c:482`
    shows `/proc/<pid>/status` use. `apps.plugin` itself is
    multi-platform (`CMakeLists.txt:192`), but the APPS_LOOKUP
    integration path in
    SOW-B depends on Linux cgroup data and is Linux-only. Therefore
    lookup-method benchmark acceptance for this SOW is
    `benchmarks-posix.md`; Windows lookup-method benchmark rows are no
    longer required to close this SOW. Windows compile checks remain
    useful guardrails because `plugin-ipc` keeps Windows transport/API
    code.

### Assistant Understanding

Facts:

- `plugin-ipc` is a cross-language IPC library with C, Rust, and Go
  implementations, governed by `AGENTS.md` and `docs/`.
- Three method codes are defined today (`INCREMENT = 1`,
  `CGROUPS_SNAPSHOT = 2`, `STRING_REVERSE = 3`); see
  `src/libnetdata/netipc/include/netipc/netipc_protocol.h:55`.
- The codec spec format is established in
  `docs/codec-cgroups-snapshot.md`; new methods must ship an equivalent
  codec spec.
- The C, Rust, and Go cross-language interop is a release requirement
  (`AGENTS.md` Goals).
- `CGROUPS_SNAPSHOT` is the only existing typed service; it carries
  `hash`, `options`, `enabled`, prefixed chart-id `name`, and
  `cgroup.procs` `path` per item.
- The current `cgroups-snapshot` consumer is `ebpf.plugin` in the Netdata
  repo. It remains on `CGROUPS_SNAPSHOT` and is not affected by this SOW.
- Benchmarks for existing methods exist in `bench/` and are summarized in
  `benchmarks-posix.md` and `benchmarks-windows.md`.
- Netipc round-trip cost is measured at <1 to ~10 microseconds for SHM
  and ~5 microseconds to ~1 ms for UDS on representative hardware (see
  `benchmarks-posix.md`). Per-item lookups are essentially free at the
  rates this SOW targets.
- `docs/netipc-integrator-skill.md` is the project's runtime integrator
  guidance. It must be updated when public APIs / service workflow
  change.
- `tests/interop_codec.sh` uses one `.bin` file per fixture variant
  (e.g., `cgroups_resp.bin` plus `cgroups_resp_empty.bin`). Each `.bin`
  contains a single serialized payload.

Inferences:

- The new methods follow the same packed/directory/relative-offset
  pattern as `CGROUPS_SNAPSHOT`, proven across all three languages.
- The orchestrator enum is small and stable enough to live as a numeric
  enum in `netipc_protocol.h` rather than as a string field; it is shared
  between the two new methods so any future Netdata enrichment can use
  the same vocabulary.
- A single SOW covering protocol + three languages + tests + benchmarks +
  docs is preferable to splitting because the wire format is the
  authoritative artifact and must remain in lockstep across languages and
  tests.

Unknowns:

- None remain at the protocol-design level. All wire-format details are
  fixed in this SOW after three rounds of multi-agent review.
  Integration-side unknowns (cgroups.plugin refactor, apps.plugin server
  placement, network-viewer client placement) belong to the Netdata-side
  follow-up SOW.

### Acceptance Criteria

- The protocol specification document `docs/codec-cgroups-lookup.md`
  exists, follows the structure of `docs/codec-cgroups-snapshot.md`, and
  is the authoritative wire-format source for `CGROUPS_LOOKUP`.
- The protocol specification document `docs/codec-apps-lookup.md`
  exists, follows the same structure, and is the authoritative wire-
  format source for `APPS_LOOKUP`.
- The shared orchestrator enum, the status enums, the `NIPC_UID_UNSET`
  constant, and the wire-size constants from decision 32 are defined in
  `src/libnetdata/netipc/include/netipc/netipc_protocol.h` and visible
  to C, Rust, and Go consumers; values are identical across all three
  language bindings.
- C `_Static_assert` declarations verify the offset of every field in
  every new wire struct. The C implementation does NOT assert
  `sizeof(struct) == 60` for the APPS_LOOKUP item header (natural
  alignment pads to 64); field-offset assertions are sufficient.
- C, Rust, and Go all implement encoders, decoders, builders, dispatch
  helpers, and typed client/server APIs for both methods, following the
  patterns established for `CGROUPS_SNAPSHOT`. C dispatch helpers
  return `nipc_error_t` (decision 30).
- Cross-language interoperability fixtures exist for both methods and
  pass for every pairwise combination of producer/consumer languages.
  `tests/interop_codec.sh` is extended with the following 15 fixtures
  (one serialized payload per file):
  - `cgroups_lookup_req.bin` (multi-item, 2+ paths)
  - `cgroups_lookup_req_empty.bin` (`item_count == 0`)
  - `cgroups_lookup_resp_known_with_labels.bin`
  - `cgroups_lookup_resp_known_no_labels.bin`
  - `cgroups_lookup_resp_unknown_retry.bin`
  - `cgroups_lookup_resp_unknown_permanent.bin`
  - `cgroups_lookup_resp_empty.bin` (`item_count == 0`)
  - `apps_lookup_req.bin` (multi-item; includes PID 0 and a PID with
    `comm_length == 15` boundary)
  - `apps_lookup_req_empty.bin` (`item_count == 0`)
  - `apps_lookup_resp_known_full.bin` (status=KNOWN,
    cgroup_status=KNOWN, labels present)
  - `apps_lookup_resp_known_retry.bin` (cgroup_status=RETRY_LATER)
  - `apps_lookup_resp_known_permanent.bin` (cgroup_status=PERMANENT)
  - `apps_lookup_resp_known_host_root.bin` (cgroup_status=HOST_ROOT)
  - `apps_lookup_resp_unknown_pid.bin` (status=UNKNOWN; includes
    `NIPC_UID_UNSET` and `starttime` boundary values)
  - `apps_lookup_resp_empty.bin` (`item_count == 0`)
- Unit tests cover happy-path encode/decode, request and response
  validation, truncation, bad alignment, bad layout version, bad item
  count, bad status, bad reserved fields (non-zero), missing NUL,
  interior NUL in response strings (rejected), overlapping strings
  (including label key/value overlap and label entry overlap with
  strings), `offset + length` arithmetic overflow,
  `label_count * NIPC_LOOKUP_LABEL_ENTRY_SIZE` arithmetic overflow,
  string-offset-less-than-header-size rejection,
  directory offset+length exceeds packed-area length rejection, inter-item
  overlap in response packed area, label entry table offset diverging
  from canonical position (rejected), non-zero padding before label
  entry table (rejected), unknown orchestrator (accepted), empty
  request, empty response, empty label list, empty-label-key
  (rejected), max-length comm boundary (`comm_length == 15` accepted;
  `comm_length >= 16` rejected), `comm_length == 0` rejected when
  `status == KNOWN`, sentinel value round-trips (`NIPC_UID_UNSET`,
  `starttime` values `0` and `UINT64_MAX`, `ppid == 0`,
  `cgroup_status == HOST_ROOT` with empty cgroup fields).
- Fuzz tests exist for both decoders (request and response) in every
  language, with the same guarantee as the existing snapshot codec: no
  input may crash or panic.
- Benchmark coverage includes throughput and CPU for each new method, in
  the same shape as the existing `snapshot-baseline`, `snapshot-shm`,
  `lookup`, and ping-pong runs in `benchmarks-posix.md`. The lookup
  benchmark uses 16 request items with three scenarios: 100% KNOWN,
  100% UNKNOWN, 50% mixed. A secondary scenario with 256 items
  validates scaling. Throughput is measured at maximum and at 100k,
  10k, and 1k target req/s. Numbers are recorded in
  `benchmarks-posix.md`. Windows lookup-method benchmark rows are not
  required because the Netdata consumers for this feature are
  Linux-only; Windows compile checks remain validation guardrails for
  generic `plugin-ipc` API coverage.
- `diff-netdata-vendor.sh` is invoked as a smoke check. Drift caused by
  the new method files (`cgroups_lookup`, `apps_lookup`, new enums and
  constants) is expected and recorded in the SOW Execution Log; SOW-B
  re-runs the sync.

## Analysis

Sources checked:

- `AGENTS.md`
- `.agents/sow/SOW.template.md`
- `.agents/sow/done/SOW-0001-20260501-bootstrap-sow-basics.md`
- `.agents/sow/done/SOW-0003-20260524-backport-netdata-go-vendor-cleanup.md`
- `.agents/sow/pending/SOW-0002-20260501-root-todo-classification.md`
- `docs/codec-cgroups-snapshot.md`
- `docs/codec.md`
- `docs/code-organization.md`
- `docs/level2-typed-api.md`
- `docs/level3-snapshot-api.md`
- `docs/netipc-integrator-skill.md`
- `src/libnetdata/netipc/include/netipc/netipc_protocol.h`
- `src/libnetdata/netipc/src/protocol/netipc_protocol.c`
- `src/libnetdata/netipc/src/service/netipc_service.c`
- `src/go/pkg/netipc/protocol/cgroups.go`
- `src/go/pkg/netipc/service/cgroups/{client.go,cache.go,types.go}`
- `src/crates/netipc/src/protocol/cgroups.rs`
- `src/crates/netipc/src/service/cgroups.rs`
- `tests/interop_codec.sh`
- `benchmarks-posix.md`
- `benchmarks-windows.md`
- `diff-netdata-vendor.sh`

Current state:

- `CGROUPS_SNAPSHOT` is the only typed snapshot service in the library
  and has stable codecs in all three languages.
- The Level 1 transport layer (UDS, SHM, named pipe, Windows SHM) is
  unchanged by this SOW; only Level 2 typed APIs and Level 3 service
  kinds are added.
- Method codes 1, 2, 3 are taken; 4 and 5 are free for `CGROUPS_LOOKUP`
  and `APPS_LOOKUP`.
- The packed payload pattern (snapshot header + item directory + packed
  area with 8-byte alignment) is implemented end-to-end and reusable.
- Three earlier SOW drafts were reviewed by six-to-seven independent AI
  agents each (codex, glm, mimo, kimi, minimax, deepseek, qwen). Their
  structural findings (round 1: C1/C6/C8; round 2: CR1/F1-F18; round 3:
  validation-precision and documentation polish) are incorporated into
  this revision.

Risks:

- Wire-format drift across languages: mitigated by 15 cross-language
  fixture files as a hard acceptance gate; every status variant has its
  own file.
- Forward-compatibility: every payload starts with a 16-bit
  `layout_version`; decoders reject unknown versions; new fields require
  a version bump.
- Cross-language enum drift: mitigated by defining values once in
  `netipc_protocol.h` and mirroring values in Rust and Go alongside
  their tests; cross-language fixtures exercise every defined enum
  value plus one unknown orchestrator value.
- Builder bound math: the snapshot builder pattern already handles
  out-of-space (`NIPC_ERR_OVERFLOW`). The new builders must follow the
  same pattern. Label-count and per-item overflow checks are explicit.
  All `offset + length` AND `count * element_size` checks use checked
  arithmetic (decision 31).
- C struct padding: the natural C struct of the 60-byte APPS_LOOKUP
  fields pads to 64 bytes (`u64` alignment); using `sizeof(struct)` as
  wire length would corrupt the protocol. Mitigated by mandated
  wire-size constants and a prohibition on asserting
  `sizeof(struct) == 60` (decision 32).
- Performance regression risk on existing methods: mitigated by running
  the full benchmark suite before and after; numbers recorded in
  `benchmarks-posix.md`.
- Information disclosure: `APPS_LOOKUP` exposes process metadata (`pid`,
  `ppid`, `uid`, `starttime`, `comm`), cgroup identity (`cgroup_path`,
  `cgroup_name`), and labels (which may include k8s namespace, pod
  name, image, etc.). `CGROUPS_LOOKUP` exposes cgroup metadata. An
  authorized local client can probe for existence of arbitrary cgroup
  paths or PIDs. Mitigated by (a) the localhost-only transport
  (Unix-domain socket / named pipe), (b) the existing auth-token
  handshake, and (c) service socket/pipe permissions configured by the
  Netdata-side integration. Documented in the codec specs' security
  considerations section.
- Path traversal: a request key like `/../../../etc/shadow` is valid on
  the wire. Mitigated by decision 34: server MUST treat request paths
  as opaque lookup keys and never resolve them as filesystem paths.
- Server restart and generation-scoped negative cache staleness: a
  server restart resets internal state. Mitigated by decision 22
  (clients track `generation` and evict generation-scoped negative
  entries on decrease/reset before processing the response).

## Pre-Implementation Gate

Status: ready

Problem / root-cause model:

- The current `CGROUPS_SNAPSHOT` method ships the full cgroup table per
  refresh, which is wasteful for consumers that only need a small subset
  (a few PIDs' cgroups, not all cgroups system-wide). The natural
  enrichment chain for Netdata is pyramidal: socket -> pid -> cgroup ->
  metadata. Each layer's working set is smaller than the next. A
  per-item lookup with client-side caching matches this pyramid; a
  snapshot does not.
- `CGROUPS_SNAPSHOT` also has no schema for the friendly-name,
  orchestrator type, or container labels that a UI-facing topology
  producer needs.

Evidence reviewed:

- `docs/codec-cgroups-snapshot.md` (existing snapshot codec).
- `src/libnetdata/netipc/include/netipc/netipc_protocol.h:55` (method
  codes), `:80` (error codes), `:101` (envelope), `:275` (cgroups
  request), `:355` (cgroups builder constants).
- `src/libnetdata/netipc/src/protocol/netipc_protocol.c:91-98`
  (`_Static_assert` pattern for wire-struct verification).
- `src/go/pkg/netipc/protocol/cgroups.go` (Go cgroups codec).
- `src/crates/netipc/src/protocol/cgroups.rs` (Rust cgroups codec).
- `src/go/pkg/netipc/service/cgroups/{client,cache,types}.go` (typed
  client + L3 cache structure used as a template for the new typed
  clients).
- `tests/interop_codec.sh` (cross-language fixture harness; the
  multi-variant pattern using `cgroups_resp.bin` plus
  `cgroups_resp_empty.bin` is precedent for the 15-fixture list).
- `docs/netipc-integrator-skill.md` (project's runtime integrator
  guidance; will need updates for the new method patterns).
- `benchmarks-posix.md` (existing perf floors).
- `diff-netdata-vendor.sh` (sync mechanism between this repo and the
  vendored copy in Netdata; uses `set -euo pipefail`, but the `diff`
  invocations at lines 151-152 use `|| true` to suppress the diff exit
  status; the script prints drift to stdout and exits `0` even when
  drift is present; drift must be reviewed from the printed output, not
  from the script's exit code).
- Multi-agent review of three earlier SOW drafts. Structural findings
  drove decisions 15-17 (round 1) and 19-32 (round 2). Round-3 findings
  drove decisions 33-34 plus tightened validation rules.

Affected contracts and surfaces:

- New methods on the netipc wire: `CGROUPS_LOOKUP` (code 4) and
  `APPS_LOOKUP` (code 5).
- New shared orchestrator enum and status enums in `netipc_protocol.h`,
  mirrored in Rust and Go.
- New constant `NIPC_UID_UNSET` in `netipc_protocol.h`, mirrored.
- New wire-size constants in `netipc_protocol.h` (decision 32).
- New codec spec docs: `docs/codec-cgroups-lookup.md` and
  `docs/codec-apps-lookup.md`.
- New C headers, source files, Rust modules, and Go packages for both
  methods.
- New cross-language fixtures and tests; `tests/interop_codec.sh`
  extended with the 15-fixture list in the acceptance criteria.
- Benchmark harness gains two new scenarios with three workload variants
  each (100% KNOWN, 100% UNKNOWN, 50% mixed), plus a 256-item scaling
  variant.
- `docs/netipc-integrator-skill.md` updated with the lookup method
  patterns and the new wire conventions (decision 19's NUL rule;
  decision 28's 16-byte request header; decision 32's wire-size
  constants; decision 33's interior-NUL rule).
- `vendor-to-netdata.sh` and `diff-netdata-vendor.sh` continue to work
  unchanged. SOW-A does not run them in the update direction; SOW-B is
  responsible for that.

Existing patterns to reuse:

- The full `CGROUPS_SNAPSHOT` family is the template: request struct,
  response builder, response view, dispatch helper, typed Server,
  typed Client, optional L3 cache wrapper.
- Codec spec doc structure from `docs/codec-cgroups-snapshot.md` is the
  template for the two new codec docs.
- The 8-byte alignment between packed items, packed-area with relative
  offsets, NUL-terminated C-strings (with the snapshot codec's "empty
  string still has NUL" pattern), and dual offset+length per string
  field are inherited from the snapshot codec.
- `_Static_assert` for wire-struct verification, per
  `netipc_protocol.c:91-98`.
- Worker-thread server model (`CGROUP_NETIPC_WORKER_COUNT = 2` in
  Netdata's current consumer) is inherited; this SOW does not change
  it.
- Cross-language fixture harness `tests/interop_codec.sh`, with the
  one-file-per-variant convention.

Risk and blast radius:

- Adding new method codes is non-breaking; existing consumers (notably
  Netdata's `ebpf.plugin` using `CGROUPS_SNAPSHOT`) are not affected.
- The two new methods are independent of each other; they can be
  implemented and reviewed in two language-internal passes per language,
  but ship together since the SOW is a single deliverable.
- Cross-language interop is the highest-risk surface and is gated by
  the 15-fixture matrix in the acceptance criteria.
- No transport changes; no envelope changes; no hello/handshake changes.

Sensitive data handling plan:

- The SOW, codec docs, and code do not contain real customer or operator
  data. Wire-format documentation uses synthetic example values such as
  `/system.slice/docker-abc.scope` and `nginx-deployment-abc`.
- Test fixtures use synthetic cgroup paths and PIDs.
- Benchmark output records aggregate throughput/latency and never
  per-item payload content.
- No raw secrets, credentials, bearer tokens, SNMP communities, community
  member names, customer names, personal data, customer-identifying IPs,
  private endpoints, or proprietary incident details are written into
  any durable artifact produced by this SOW.

Implementation plan:

1. Add the shared orchestrator enum, the status enums, the
   `NIPC_UID_UNSET` constant, the wire-size constants (decision 32),
   and the two new method codes (4, 5) to
   `src/libnetdata/netipc/include/netipc/netipc_protocol.h`. Mirror the
   enum values and the UID constant in Rust and Go protocol modules.
2. Write `docs/codec-cgroups-lookup.md`, mirroring the structure of
   `docs/codec-cgroups-snapshot.md`. Lock the wire format inline (see
   "Wire format - CGROUPS_LOOKUP" below). Include a security
   considerations section noting path probing exposure and the opaque
   path-handling rule (decision 34). Include a concrete key-packing
   example.
3. Write `docs/codec-apps-lookup.md`, mirroring the same structure.
   Lock the wire format inline (see "Wire format - APPS_LOOKUP" below).
   Include a security considerations section noting the broader
   exposure (`pid`, `ppid`, `uid`, `starttime`, `comm`, cgroup_path,
   cgroup_name, labels) and the same opaque-key-handling rule.
4. Implement C codecs: request/response structs, builders, decoders,
   dispatch helpers (returning `nipc_error_t`) in
   `src/libnetdata/netipc/{include,src}`, mirroring the shape of the
   cgroups-snapshot files. Use `reserved0`, `reserved1` for duplicate
   reserved fields in the same struct. Add `_Static_assert` for every
   field offset in every wire-struct. Do NOT assert
   `sizeof(struct) == 60` for the APPS_LOOKUP item header. Use the
   wire-size constants from step 1; do NOT use `sizeof(struct)` for
   wire-length math. All `offset + length` AND
   `count * element_size` bounds checks use 64-bit intermediates to
   detect overflow. Add unit tests in `tests/`.
5. Implement Rust codecs in `src/crates/netipc/src/protocol/` and typed
   server/client in `src/crates/netipc/src/service/`, mirroring the
   cgroups module. All bounds checks use `checked_add` / `checked_mul`.
   Add unit tests and proptest fuzz tests.
6. Implement Go codecs in `src/go/pkg/netipc/protocol/` and typed
   server/client + L3 cache in `src/go/pkg/netipc/service/{cgroups_lookup,
   apps_lookup}/`, mirroring the existing cgroups module. All bounds
   checks convert to `uint64` before arithmetic. Add unit tests and
   fuzz tests.
7. Add cross-language fixture tests: extend `tests/interop_codec.sh`
   with the 15-fixture list in the acceptance criteria. Encode in
   language A, decode in language B, for every pairwise combination.
8. Add benchmark scenarios for both methods in `bench/` following the
   `snapshot-baseline`, `snapshot-shm`, `lookup`, and ping-pong shapes.
   Three workload variants per scenario (100% KNOWN, 100% UNKNOWN,
   50% mixed) plus the 256-item scaling variant. Re-run the POSIX
   suite. Update `benchmarks-posix.md`.
9. Update `docs/netipc-integrator-skill.md` with the lookup method
   patterns: the 16-byte request header convention, the always-NUL
   empty-string convention, the no-interior-NUL rule, the wire-size
   constants, the canonical label-table offset, the
   `_Static_assert`-offsets-only-for-padded-headers rule, and the L3
   cache semantics for the three-state cache including the generation-
   based eviction order.
10. Validation pass: confirm spec docs match all three implementations;
    confirm enum and constant values are identical across languages;
    invoke `diff-netdata-vendor.sh` as a smoke check and record the
    expected drift in the Execution Log.
11. Multi-agent review pass on the implemented codecs and the new spec
    docs; iterate until reviewers cannot find anything else.
12. SOW close: move this file to `.agents/sow/done/` and commit it
    together with the implementation in a single commit.

Validation plan:

- Per-language unit tests covering encode/decode happy path and every
  validation rule listed under "Validation rules" below.
- Cross-language fixture round-trip tests for both methods, every
  producer/consumer pair, every status value, every `cgroup_status`
  value, KNOWN with and without labels, and the empty-request /
  empty-response cases.
- Fuzz tests in every language; no input may crash or panic.
- Benchmark runs producing entries in `benchmarks-posix.md` for both
  new methods with three workload variants each plus the 256-item
  scaling variant. Windows lookup-method benchmark rows are not
  required after decision 35.
- Spec docs reviewed against the C, Rust, and Go implementations for
  consistency.
- Manual verification that the orchestrator enum, status enums,
  `NIPC_UID_UNSET`, and the wire-size constants have identical numeric
  values across all three language bindings.
- Multi-agent review pass on the implemented codecs and spec docs
  before the SOW closes; iterate until reviewers cannot find anything
  else.

Artifact impact plan:

- `AGENTS.md`: no change expected; the SOW does not change the project's
  workflow.
- Runtime project skills: none in `.agents/skills/` today; no change
  expected there.
- Specs (under `docs/`): this SOW adds `docs/codec-cgroups-lookup.md`
  and `docs/codec-apps-lookup.md`. The integrator skill
  `docs/netipc-integrator-skill.md` is updated to reflect the new
  patterns.
- End-user/operator docs: none; this is library work consumed by other
  software (Netdata plugins).
- End-user/operator skills: none affected.
- SOW lifecycle: this SOW lives at
  `.agents/sow/current/SOW-0004-20260525-cgroups-apps-lookup-methods.md`
  during implementation and will move to `.agents/sow/done/` when completed, with the
  implementation, docs, tests, benchmarks, and SOW lifecycle change in
  one commit.

Open-source reference evidence:

- No external open-source repositories were checked for this SOW; the
  design reuses patterns already established inside `plugin-ipc`
  (`CGROUPS_SNAPSHOT` codec and typed service shape). Empirical numbers
  in `benchmarks-posix.md` were used to size the feasibility argument
  during the design discussion that produced this SOW.

Open decisions:

- All design decisions are resolved (see decisions 1-34 in the User
  Request section). No user decisions block implementation.

## Wire format - CGROUPS_LOOKUP

### Service identity

- service_namespace: `/run/netdata` (POSIX), derived named pipe
  namespace (Windows).
- service_name: `cgroups-lookup`.
- request kind: the served endpoint serves the `cgroups-lookup` request
  kind only.

### Method code

The outer envelope's `code` field carries method code `4`
(`CGROUPS_LOOKUP`) as defined in
`src/libnetdata/netipc/include/netipc/netipc_protocol.h`.

### Batch semantics

Both request and response are single non-batch Level 1 messages
(`item_count = 1` in the outer envelope, `BATCH` flag NOT set). The
method-internal directory is not a Level 1 batch directory.

### Endianness

All multi-byte integer fields use host byte order (native endianness).
This is localhost-only IPC; both peers share the same architecture.

### Length conventions (footgun warning)

For CGROUPS_LOOKUP request keys: the directory entry `length` field
includes the trailing NUL byte (the length is the full key payload
length, e.g., `length = path_length + 1`). All per-item header string
`length` fields (`path_length`, `name_length`, label key/value
`length`) exclude the trailing NUL. The NUL is always present at
`byte[offset + length]`. This is the same dual convention used by
`CGROUPS_SNAPSHOT`.

### Request payload

The client provides zero or more cgroup paths. The server returns one
response item per request item, in the same order.

Snapshot-level request header (16 bytes):

| Offset | Size | Type | Field | Description |
|--------|------|------|-------|-------------|
| 0 | 2 | u16 | layout_version | Must be `1` |
| 2 | 2 | u16 | flags | Reserved, must be `0` |
| 4 | 4 | u32 | item_count | Number of lookup keys (cgroup paths); `0` is allowed |
| 8 | 4 | u32 | reserved0 | Must be `0` |
| 12 | 4 | u32 | reserved1 | Must be `0` |

Per-key directory (immediately after the 16-byte header), `item_count`
entries (8 bytes each):

| Offset (within entry) | Size | Type | Field |
|-----------------------|------|------|-------|
| 0 | 4 | u32 | offset | Byte offset of key payload from start of packed key area; must be a multiple of 8 |
| 4 | 4 | u32 | length | Byte length of key payload (including the trailing NUL) |

Total directory size: `item_count * 8` bytes. The packed key area starts
at offset `16 + 8 * item_count`, which is 8-byte aligned for any
`item_count`.

Per-key payload (packed key area, aligned to 8 bytes between keys):

A request key is exactly `path bytes + NUL`. There is no per-key header.

- `length` from the directory equals `path_length + 1` (including NUL).
- The path string is at offset `0` within the key.
- The byte at offset `length - 1` MUST be `0`.
- The path MUST NOT contain interior NUL bytes.
- The encoder MUST NOT emit empty paths (`length == 1` with only a NUL);
  if a client wishes to probe with no work, it MUST send
  `item_count == 0` instead.

**Concrete packing example**: a request with paths `/a/b` (5 bytes
incl. NUL) and `/c` (3 bytes incl. NUL) has directory entries
`{offset=0, length=5}` and `{offset=8, length=3}`. The packed key area
contains `/a/b\0XXX/c\0`, where `XXX` is 3 zero padding bytes between
the first key (ending at byte 4) and the second key (starting at
byte 8) to maintain 8-byte alignment.

### Response payload

Snapshot-level response header (16 bytes):

| Offset | Size | Type | Field | Description |
|--------|------|------|-------|-------------|
| 0 | 2 | u16 | layout_version | Must be `1` |
| 2 | 2 | u16 | flags | Reserved, must be `0` |
| 4 | 4 | u32 | item_count | Equals request `item_count` |
| 8 | 8 | u64 | generation | Server-side monotonic counter; advisory (see decision 22) |

**Note**: the response header differs from the request header. At
offsets 8-15, the request carries `reserved0 + reserved1` (two `u32`
zeros); the response carries `generation` (one `u64`). Implementers
MUST NOT reuse the same struct for both.

**Note**: the response header (16 bytes) also differs from the
`CGROUPS_SNAPSHOT` response header (24 bytes), which has a
`systemd_enabled` field and `generation` at offset 16. The lookup
methods do not need `systemd_enabled` (the per-item `orchestrator`
field carries the equivalent information).

Per-item directory (`item_count` entries, 8 bytes each), same shape as
the request directory.

Per-item payload header (28 bytes fixed):

| Offset | Size | Type | Field | Description |
|--------|------|------|-------|-------------|
| 0 | 2 | u16 | layout_version | Must be `1` |
| 2 | 2 | u16 | status | `nipc_cgroup_lookup_status_t`: `0` KNOWN, `1` UNKNOWN_RETRY_LATER, `2` UNKNOWN_PERMANENT (decoder MUST reject unknown values) |
| 4 | 2 | u16 | orchestrator | Shared orchestrator enum; meaningful only when `status == KNOWN`; MUST be `0` when `status != KNOWN` (decoder accepts unknown values per decision 18) |
| 6 | 2 | u16 | reserved0 | Must be `0` |
| 8 | 4 | u32 | path_offset | Echo of request path; offset from item start; MUST be `>= 28` |
| 12 | 4 | u32 | path_length | Echo of request path length, excluding NUL; MUST be `>= 1` for all statuses |
| 16 | 4 | u32 | name_offset | Friendly name offset from item start; MUST be `>= 28` (decision 19) |
| 20 | 4 | u32 | name_length | Friendly name length, excluding NUL; `0` if no friendly name; MUST be `0` when `status != KNOWN` |
| 24 | 2 | u16 | label_count | Number of labels; MUST be `0` when `status != KNOWN` |
| 26 | 2 | u16 | reserved1 | Must be `0` |

Total fixed item header: 28 bytes.

Variable area for one item (after the 28-byte item header):

1. Echoed cgroup path bytes + NUL (`path_length` bytes followed by a NUL
   byte). Always present, since `path_length >= 1`.
2. Friendly name bytes + NUL. Always present (`name_length` may be `0`,
   in which case only the NUL byte is present at `byte[name_offset]`).
3. Label entry table: `label_count` entries, each 16 bytes:
   `u32 key_offset, u32 key_length, u32 value_offset, u32 value_length`.
   The table starts at the **canonical position**: the first
   8-byte-aligned offset (relative to the item payload start) that is
   `>=` the end of all preceding strings (their NUL bytes inclusive,
   i.e., `max(string_offset + string_length + 1)`). Up to 7 padding
   bytes (zero-filled) may precede the table.
4. Per-label key + NUL and value + NUL strings, packed immediately
   after the table in the order they appear in the table. Each is
   NUL-terminated. Empty label values (`value_length == 0`) are allowed
   and the NUL is still present. Empty label keys (`key_length == 0`)
   are NOT allowed (semantically meaningless and a protocol violation).

All variable-area string offsets in the fixed header and in label
entries are relative to the start of the item payload (byte 0 of the
28-byte fixed header). All item payloads in the packed-item area are
aligned to 8-byte boundaries.

### Encoding / decoding

Mirror the existing `CGROUPS_SNAPSHOT` builder/view pattern:

- Builder: caller supplies one item at a time, providing path, status,
  orchestrator, optional name, and zero-or-more label pairs. The
  builder manages offsets, NULs, alignment, padding, and bounds.
  Overflow yields `NIPC_ERR_OVERFLOW` and is sticky for dispatch.
- Decoder: validates the response header, the directory bounds, and
  each item's fixed and variable bounds. Returns an ephemeral view per
  item; string fields are borrowed views into the payload buffer.
- All bounds arithmetic uses checked operations (decision 31).

### Suggested C handler signatures

```c
typedef struct nipc_cgroups_lookup_req_view_t nipc_cgroups_lookup_req_view_t;
typedef struct nipc_cgroups_lookup_builder_t nipc_cgroups_lookup_builder_t;

typedef bool (*nipc_cgroups_lookup_handler_fn)(
    void *user,
    const nipc_cgroups_lookup_req_view_t *request,
    nipc_cgroups_lookup_builder_t *builder);

nipc_error_t nipc_dispatch_cgroups_lookup(
    const uint8_t *req, size_t req_len,
    uint8_t *resp, size_t resp_size, size_t *resp_len,
    nipc_cgroups_lookup_handler_fn handler, void *user);
```

Rust and Go signatures mirror this shape using each language's idioms.
Wire-size constants from decision 32 are used throughout;
`sizeof(struct)` is NOT used for wire-length math.

### Request validation rules

The decoder MUST reject:

- Payload shorter than the 16-byte request header.
- Unknown `layout_version`.
- Non-zero `flags`.
- Non-zero `reserved0` or `reserved1`.
- `item_count` such that
  `16 + item_count * NIPC_LOOKUP_DIR_ENTRY_SIZE` exceeds the payload
  size, OR such that `item_count * NIPC_LOOKUP_DIR_ENTRY_SIZE`
  overflows when computed with checked arithmetic.
- Any directory entry whose `offset + length` exceeds the packed key
  area length (checked arithmetic). Note: directory `offset` is
  RELATIVE to the start of the packed key area (see field description
  above); a relative `offset` of `0` is valid and points to the first
  byte of the packed area.
- Any directory entry whose `offset` is not a multiple of 8.
- Any directory entry with `length < 2` (minimum path is one byte plus
  NUL).
- Any key whose final byte (`byte[length - 1]`) is not `0`.
- Any key containing an interior NUL byte.

### Response validation rules

The decoder MUST reject:

- Payload shorter than the 16-byte response header.
- Unknown `layout_version`.
- Non-zero `flags`.
- `item_count` such that
  `16 + item_count * NIPC_LOOKUP_DIR_ENTRY_SIZE` exceeds the payload
  size, OR such that `item_count * NIPC_LOOKUP_DIR_ENTRY_SIZE`
  overflows when computed with checked arithmetic.
- Any directory entry whose `offset + length` exceeds the packed item
  area length (checked arithmetic). Note: directory `offset` is
  RELATIVE to the start of the packed item area; a relative `offset`
  of `0` is valid and points to the first byte of the packed area.
- Any pair of directory entries whose byte ranges
  `[offset, offset + length)` overlap.
- Any item start that is not 8-byte aligned within the packed item area.
- Any item payload shorter than the 28-byte fixed item header.
- Non-zero `reserved0` or `reserved1` in the item header.
- Unknown `status` value (must be one of `0`, `1`, `2`).
- Any string `offset < 28` (the fixed item header size).
- Any string `offset + length` (or `offset + length + 1` including NUL)
  exceeding the item payload bounds (checked arithmetic).
- Any string field missing the trailing NUL at
  `byte[offset + length]`.
- Any response string containing an interior NUL byte at any position
  `byte[offset]` through `byte[offset + length - 1]` (decision 33).
- `path_length == 0` for any status (echo MUST be present).
- For `status != KNOWN`: `orchestrator != 0`, `name_length != 0`, or
  `label_count != 0`.
- Overlapping byte regions for any pair of strings (echoed path,
  friendly name, label keys, label values), including their trailing
  NULs (each zero-length string still occupies its own NUL byte; two
  zero-length strings cannot share an offset).
- Label entry table that overlaps any string region.
- Label entry table whose start offset is NOT the canonical position
  (decision 29): the first 8-aligned offset `>=
  max(string_offset + string_length + 1)` over all preceding strings.
- Non-zero bytes in the padding region between the last string NUL
  and the label entry table.
- Any label entry with `key_length == 0` (empty label key is a
  protocol violation).
- Any label entry whose
  `key_offset / key_length / value_offset / value_length` references
  bytes outside the item payload (checked arithmetic) or violates the
  no-interior-NUL rule.
- `label_count * NIPC_LOOKUP_LABEL_ENTRY_SIZE` bytes that do not fit
  within the item payload (checked arithmetic; the multiplication
  itself uses 64-bit intermediates per decision 31).

Decoders MUST accept any `orchestrator` value (forward compatibility);
unknown values are surfaced to the application layer as a raw `u16`.

### Typed client responsibilities

- Verify that response `item_count` equals request `item_count`.
- Verify that each response item's echoed `path` bytes equal the
  corresponding request item's path bytes; flag and either drop the
  item or fail the whole response on mismatch.
- Track the last-seen `generation` per service. On `generation`
  decrease or reset, FIRST evict cached `UNKNOWN_PERMANENT` entries,
  THEN process the response items (so any new PERMANENT values in the
  same response are preserved per decision 22).

### Security considerations

- Server MUST treat request paths as opaque lookup keys, not filesystem
  paths. Path traversal characters (`..`) and absolute paths have no
  special meaning (decision 34).
- An authorized local client can probe for existence of arbitrary
  cgroup paths via this method. Mitigation: localhost-only transport,
  the auth-token handshake, and socket/pipe permissions configured by
  the Netdata-side integration.

## Wire format - APPS_LOOKUP

### Service identity

- service_namespace: `/run/netdata` (POSIX), derived named pipe
  namespace (Windows).
- service_name: `apps-lookup`.
- request kind: the served endpoint serves the `apps-lookup` request
  kind only.

### Method code

The outer envelope's `code` field carries method code `5`
(`APPS_LOOKUP`).

### Batch semantics

Both request and response are single non-batch Level 1 messages
(`item_count = 1` in the outer envelope, `BATCH` flag NOT set). The
method-internal directory is not a Level 1 batch directory.

### Endianness

All multi-byte integer fields use host byte order (native endianness).

### Length conventions

- **APPS_LOOKUP request keys are 8-byte binary structs**, NOT strings.
  The directory entry `length` is always `8`; there is no NUL byte in
  the request keys. The "trailing NUL" convention from CGROUPS_LOOKUP
  does NOT apply.
- **APPS_LOOKUP response strings** (`comm`, `cgroup_path`,
  `cgroup_name`, label keys, label values) follow the same convention
  as CGROUPS_LOOKUP response strings: per-item header `length` fields
  exclude the trailing NUL. The NUL is always present at
  `byte[offset + length]`.

### Request payload

The client provides zero or more PIDs. The server returns one response
item per request item, in the same order.

Snapshot-level request header (16 bytes), same shape as
`CGROUPS_LOOKUP`:

| Offset | Size | Type | Field | Description |
|--------|------|------|-------|-------------|
| 0 | 2 | u16 | layout_version | Must be `1` |
| 2 | 2 | u16 | flags | Reserved, must be `0` |
| 4 | 4 | u32 | item_count | Number of PIDs; `0` is allowed |
| 8 | 4 | u32 | reserved0 | Must be `0` |
| 12 | 4 | u32 | reserved1 | Must be `0` |

Per-key directory (`item_count` entries, 8 bytes each), same shape as
`CGROUPS_LOOKUP`.

Per-key payload (8 bytes, fixed):

| Offset | Size | Type | Field | Description |
|--------|------|------|-------|-------------|
| 0 | 4 | u32 | pid | PID being queried; `0` is allowed and yields `status == UNKNOWN` |
| 4 | 4 | u32 | reserved | Must be `0` |

PID `0` is the kernel idle process. The decoder MUST accept it; the
server returns `status == UNKNOWN` for it.

### Response payload

Snapshot-level response header (16 bytes), same shape as
`CGROUPS_LOOKUP` response (the same caveat applies: request and
response headers are NOT identical at offsets 8-15).

Per-item directory (`item_count` entries, 8 bytes each), same shape as
`CGROUPS_LOOKUP`.

Per-item payload header (60 bytes fixed):

| Offset | Size | Type | Field | Description |
|--------|------|------|-------|-------------|
| 0 | 2 | u16 | layout_version | Must be `1` |
| 2 | 2 | u16 | status | `nipc_pid_lookup_status_t`: `0` KNOWN, `1` UNKNOWN (decoder MUST reject unknown) |
| 4 | 2 | u16 | orchestrator | Shared orchestrator enum; meaningful only when `status == KNOWN && cgroup_status == KNOWN`; MUST be `0` otherwise (decoder accepts unknown per decision 18) |
| 6 | 2 | u16 | cgroup_status | `nipc_apps_cgroup_status_t`: `0` KNOWN, `1` UNKNOWN_RETRY_LATER, `2` UNKNOWN_PERMANENT, `3` HOST_ROOT (decoder MUST reject unknown); meaningful only when `status == KNOWN`; MUST be `0` when `status == UNKNOWN` |
| 8 | 4 | u32 | pid | Echo of request PID |
| 12 | 4 | u32 | ppid | Parent PID; `0` if unknown or for PID 1 |
| 16 | 4 | u32 | uid | `NIPC_UID_UNSET = 0xFFFFFFFFu` if unknown; any other `u32` is a valid UID |
| 20 | 4 | u32 | reserved0 | Must be `0` |
| 24 | 8 | u64 | starttime | Linux jiffies since system boot (`/proc/<pid>/stat` field 22); 8-byte aligned; `0` on non-Linux platforms (decision 23) |
| 32 | 4 | u32 | comm_offset | Offset of comm string from item start; MUST be `>= 60` |
| 36 | 4 | u32 | comm_length | Length of comm string, excluding NUL; `<= 15` (TASK_COMM_LEN - 1); MUST be `>= 1` when `status == KNOWN` |
| 40 | 4 | u32 | cgroup_path_offset | Offset of cgroup path string from item start; MUST be `>= 60` |
| 44 | 4 | u32 | cgroup_path_length | Length of cgroup path string, excluding NUL; populated when `status == KNOWN && cgroup_status != HOST_ROOT`; `0` otherwise |
| 48 | 4 | u32 | cgroup_name_offset | Offset of cgroup friendly name from item start; MUST be `>= 60` |
| 52 | 4 | u32 | cgroup_name_length | Length of cgroup friendly name, excluding NUL; populated only when `status == KNOWN && cgroup_status == KNOWN`; may still be `0` if the cgroup has no friendly name |
| 56 | 2 | u16 | label_count | Number of labels; populated only when `status == KNOWN && cgroup_status == KNOWN`; `0` otherwise |
| 58 | 2 | u16 | reserved1 | Must be `0` |

Total fixed item header: 60 bytes.

**C struct padding note**: A natural C struct of these fields will pad
to 64 bytes due to `u64` alignment. The C implementation MUST use the
explicit `NIPC_APPS_LOOKUP_ITEM_HDR_SIZE = 60u` constant from decision
32 for wire-length math; `sizeof(struct)` is NOT a valid substitute.
`_Static_assert` verifies every field offset; do NOT assert
`sizeof(struct) == 60` (it will fail because of trailing padding).

**Consumer warning**: A consumer MUST read `status` first. When
`status == UNKNOWN`, all other fields except `pid` are undefined
(encoder sets defaults per decision 20, but decoders treat them as
meaningless). When `status == KNOWN`, read `cgroup_status` to determine
which cgroup fields are populated.

Variable area for one item (after the 60-byte item header):

1. `comm` bytes + NUL. Always present at `byte[comm_offset]` (the NUL
   alone if `comm_length == 0`, which only occurs when
   `status == UNKNOWN`).
2. `cgroup_path` bytes + NUL. Always present at
   `byte[cgroup_path_offset]` (the NUL alone if
   `cgroup_path_length == 0`, which occurs when `status == UNKNOWN` or
   `cgroup_status == HOST_ROOT`).
3. `cgroup_name` bytes + NUL. Always present at
   `byte[cgroup_name_offset]` (the NUL alone if
   `cgroup_name_length == 0`, which occurs when `status == UNKNOWN`,
   `cgroup_status != KNOWN`, or the cgroup has no friendly name).
4. Label entry table: `label_count` entries, each 16 bytes:
   `u32 key_offset, u32 key_length, u32 value_offset, u32 value_length`.
   The table starts at the **canonical position**: the first
   8-byte-aligned offset (relative to the item payload start) that is
   `>=` the end of all preceding strings (their NUL bytes inclusive).
   Up to 7 padding bytes (zero-filled) may precede the table.
5. Per-label key + NUL and value + NUL strings, packed immediately
   after the table in the order they appear in the table. Empty label
   values (`value_length == 0`) are allowed and the NUL is still
   present. Empty label keys (`key_length == 0`) are NOT allowed.

All variable-area string offsets in the fixed header and in label
entries are relative to the start of the item payload (byte 0 of the
60-byte fixed header). All item payloads in the packed-item area are
aligned to 8-byte boundaries.

Note that each string field, even when its `length == 0`, occupies its
own NUL byte at its `offset`; multiple zero-length strings cannot share
a NUL byte. The overlap rule rejects shared byte ranges.

### Encoding / decoding

Same builder/view shape as `CGROUPS_LOOKUP`. The cgroup fields embedded
in each response item are filled by the Netdata-side `apps.plugin`
from its local `CGROUPS_LOOKUP` cache; from the wire format's
perspective, they are simply additional fields in the response payload.
All bounds arithmetic uses checked operations (decision 31).

### Suggested C handler signatures

```c
typedef struct nipc_apps_lookup_req_view_t nipc_apps_lookup_req_view_t;
typedef struct nipc_apps_lookup_builder_t nipc_apps_lookup_builder_t;

typedef bool (*nipc_apps_lookup_handler_fn)(
    void *user,
    const nipc_apps_lookup_req_view_t *request,
    nipc_apps_lookup_builder_t *builder);

nipc_error_t nipc_dispatch_apps_lookup(
    const uint8_t *req, size_t req_len,
    uint8_t *resp, size_t resp_size, size_t *resp_len,
    nipc_apps_lookup_handler_fn handler, void *user);
```

Rust and Go signatures mirror this shape using each language's idioms.

### Request validation rules

The decoder MUST reject:

- Payload shorter than the 16-byte request header.
- Unknown `layout_version`.
- Non-zero `flags`.
- Non-zero `reserved0` or `reserved1`.
- `item_count` such that
  `16 + item_count * NIPC_LOOKUP_DIR_ENTRY_SIZE +
  item_count * NIPC_APPS_LOOKUP_KEY_SIZE` exceeds the payload size, OR
  such that any of those multiplications overflows when computed with
  checked arithmetic.
- Any directory entry whose `offset + length` exceeds the packed key
  area length (checked arithmetic). Note: directory `offset` is
  RELATIVE to the start of the packed key area; a relative `offset`
  of `0` is valid and points to the first byte of the packed area.
- Any directory entry whose `length != 8` (PID key payloads are fixed
  8 bytes).
- Any directory entry whose `offset` is not a multiple of 8.
- Non-zero `reserved` in any per-key payload.

### Response validation rules

The structural validation rules from CGROUPS_LOOKUP response
validation apply: header bounds, directory bounds (relative `offset`
plus `length` must fit in the packed item area; inter-item overlap),
item header bounds, item alignment, reserved fields, NUL termination,
no interior NUL in strings, string offsets `>= 60`, overlap detection
across all string regions and the label entry table, label entry
bounds, label_count * entry_size overflow, canonical label table
offset, zero-filled padding, no empty label keys, forward-compat for
unknown `orchestrator`.

In addition, APPS_LOOKUP-specific rules:

- Unknown `status` value (must be one of `0`, `1`).
- Unknown `cgroup_status` value (must be one of `0`, `1`, `2`, `3`).
- `comm_length > 15` (TASK_COMM_LEN excludes NUL; maximum is 15).
- For `status == KNOWN`: `comm_length == 0` is a protocol violation.
- For `cgroup_status == KNOWN` (under `status == KNOWN`):
  `cgroup_path_length == 0` is a protocol violation.
- For `cgroup_status == UNKNOWN_RETRY_LATER` or `UNKNOWN_PERMANENT`
  (under `status == KNOWN`): `cgroup_path_length == 0`,
  `orchestrator != 0`, `cgroup_name_length != 0`, or `label_count != 0`
  is a protocol violation (the raw path must be populated; no
  metadata).
- For `cgroup_status == HOST_ROOT` (under `status == KNOWN`):
  `orchestrator != 0`, `cgroup_path_length != 0`,
  `cgroup_name_length != 0`, or `label_count != 0` is a protocol
  violation.
- For `status == UNKNOWN`: any non-default value
  (`orchestrator != 0`, `cgroup_status != 0`, `ppid != 0`,
  `uid != NIPC_UID_UNSET`, `starttime != 0`, `comm_length != 0`,
  `cgroup_path_length != 0`, `cgroup_name_length != 0`,
  `label_count != 0`) is a protocol violation. Offset fields may carry
  any value `>= 60` (they are required to be valid even when the
  corresponding length is 0, since the NUL byte must still be
  reachable).
- All overlap rules apply across the full set of APPS_LOOKUP string
  regions: `comm`, `cgroup_path`, `cgroup_name`, label keys, label
  values (including their trailing NULs).

### Typed client responsibilities

- Verify that response `item_count` equals request `item_count`.
- Verify that each response item's `pid` echo equals the corresponding
  request item's `pid`; flag and either drop the item or fail the whole
  response on mismatch.
- For PID-reuse detection: compare cached `comm` against the live
  socket-side `comm` (or any other available signal). Optionally,
  compare cached `starttime` against the response's `starttime` for
  exact identity verification.
- Track the last-seen `generation` per service. On `generation`
  decrease or reset, FIRST evict cached `UNKNOWN_PERMANENT` and
  `HOST_ROOT` entries, THEN process the response items (decision 22).

### Security considerations

- Server MUST treat the request `pid` as a numeric lookup key, not a
  privileged process reference. The server MUST NOT take any action on
  the indicated process based on receipt of a lookup request (decision
  34).
- An authorized local client can query rich process and container
  metadata via this method: `pid`, `ppid`, `uid`, `starttime`, `comm`,
  cgroup identity (`cgroup_path`, `cgroup_name`), `orchestrator`, and
  all `cg->chart_labels` (which may include k8s namespace, pod name,
  container image, etc.). This is a meaningful information surface.
  Mitigation: localhost-only transport, the auth-token handshake, and
  socket/pipe permissions configured by the Netdata-side integration.

## L3 cache contract

This SOW does not ship generic lookup-specific L3 cache implementations.
It ships the protocol codecs plus typed L2 clients/servers in C, Rust,
and Go. The existing generic L3 cache in this repository remains for
`CGROUPS_SNAPSHOT`; the working-set caches for `CGROUPS_LOOKUP` and
`APPS_LOOKUP` belong to the Netdata-side integration SOW (SOW-B).

The required consumer cache contract for SOW-B is:

- `CgroupsLookupCache`:
  - Key: `string` cgroup path (raw `cg->id`-style path).
  - Value: a structured copy of the per-item response (status,
    orchestrator, friendly name, labels).
  - Lifecycle: cache entries are owned by the caller's iteration. The
    consumer marks entries as "seen" during each iteration; entries
    not seen by end-of-iteration are evicted. There is no time-based
    TTL.
  - Three-state semantics: KNOWN entries are reused; UNKNOWN_RETRY_LATER
    entries are re-queried on the next iteration; UNKNOWN_PERMANENT
    entries are reused without re-query until evicted, OR until a
    `generation` decrease/reset is observed (decision 22), at which
    point all UNKNOWN_PERMANENT entries are evicted **before**
    processing the new response.

- `AppsLookupCache`:
  - Key: `pid` (uint32).
  - Value: a structured copy of the per-item response (status, pid,
    starttime, ppid, uid, comm, cgroup_status, cgroup_path,
    cgroup_name, orchestrator, labels).
  - Lifecycle: same "seen this iteration, else evicted" pattern.
  - PID-reuse detection: caller compares cached `comm` against live
    `comm`. Mismatch invalidates the cache entry and triggers re-query.
  - Three-state semantics for the embedded `cgroup_status` mirror the
    `CgroupsLookupCache` rules: KNOWN reused, UNKNOWN_RETRY_LATER
    re-queried, UNKNOWN_PERMANENT and HOST_ROOT reused without
    re-query during the current provider generation. On generation
    decrease/reset, evict UNKNOWN_PERMANENT and HOST_ROOT entries before
    processing the new response.

The exact public API surface for the SOW-B caches is at the implementing
agent's discretion as long as the lifecycle invariants above are
preserved.

## Execution Log

### 2026-05-25

- Moved SOW from `pending/open` to `current/in-progress` after confirming
  the Pre-Implementation Gate is ready and no user decisions remain open.
- Implemented `CGROUPS_LOOKUP` and `APPS_LOOKUP` codec support in C,
  Rust, and Go:
  - C method codes, enums, constants, wire structs, builders, decoders,
    and dispatch helpers:
    `src/libnetdata/netipc/include/netipc/netipc_protocol.h:59`,
    `src/libnetdata/netipc/src/protocol/netipc_protocol.c:169`,
    `src/libnetdata/netipc/src/protocol/netipc_protocol.c:2172`.
  - Rust method constants and lookup codec module:
    `src/crates/netipc/src/protocol/mod.rs:55`,
    `src/crates/netipc/src/protocol/lookup.rs:277`,
    `src/crates/netipc/src/protocol/lookup.rs:1213`.
  - Go method constants and lookup codec module:
    `src/go/pkg/netipc/protocol/frame.go:51`,
    `src/go/pkg/netipc/protocol/lookup.go:208`,
    `src/go/pkg/netipc/protocol/lookup.go:884`.
- Implemented typed client/server APIs for both methods:
  - C POSIX and Windows service paths:
    `src/libnetdata/netipc/include/netipc/netipc_service.h:214`,
    `src/libnetdata/netipc/src/service/netipc_service.c:723`,
    `src/libnetdata/netipc/src/service/netipc_service_win.c:723`.
  - Rust raw and public service APIs:
    `src/crates/netipc/src/service/raw.rs:312`,
    `src/crates/netipc/src/service/cgroups_lookup.rs:119`,
    `src/crates/netipc/src/service/apps_lookup.rs:154`.
  - Go raw and public service APIs:
    `src/go/pkg/netipc/service/raw/lookup_client.go:20`,
    `src/go/pkg/netipc/service/cgroups_lookup/client.go:54`,
    `src/go/pkg/netipc/service/apps_lookup/client.go:56`.
- Added cross-language codec fixtures for both methods:
  `tests/interop_codec.sh:70`, `tests/interop_codec.sh:74`.
- Added C, Rust, and Go fuzz coverage for both lookup decoders:
  `tests/fuzz_protocol.c`, `src/crates/netipc/src/protocol/mod.rs`,
  and `src/go/pkg/netipc/protocol/fuzz_test.go`.
- Added POSIX lookup-method benchmarks at max, 100k, 10k, and 1k target
  request rates:
  `bench/drivers/c/bench_posix.c`,
  `bench/drivers/rust/src/main.rs`,
  `bench/drivers/go/main.go`,
  `tests/run-posix-bench.sh:601`,
  `tests/generate-benchmarks-posix.sh:238`,
  `benchmarks-posix.md:403`.
- Ran `diff-netdata-vendor.sh`; exit code was `0`. Expected drift was
  limited to the new lookup method changes and generated/public API
  surfaces:
  - C: `netipc_protocol.h`, `netipc_service.h`,
    `netipc_protocol.c`, `netipc_service.c`, `netipc_service_win.c`.
  - Rust: `protocol/lookup.rs`, `service/cgroups_lookup.rs`,
    `service/apps_lookup.rs`, and updated module/raw service files.
  - Go: `protocol/lookup.go`, `protocol/lookup_test.go`,
    `service/cgroups_lookup/`, `service/apps_lookup/`,
    `service/raw/lookup_client.go`, and updated raw/module files.
  SOW-B remains responsible for re-vendoring and reaching no-drift state.
- User clarified that this feature is Linux/POSIX-scoped because
  `cgroups.plugin` and `network-viewer.plugin` are Linux-only. Added
  decision 35 and removed Windows lookup-method benchmark rows from
  close requirements. Kept Windows compile checks as generic
  `plugin-ipc` guardrails.
- Applied the second-pass reviewer arithmetic findings:
  - C lookup response builders now guard the internal builder offset and
    label-table arithmetic with `uint64_t` checked intermediates and
    `align8_u64_over_limit()`:
    `src/libnetdata/netipc/src/protocol/netipc_protocol.c:861`,
    `src/libnetdata/netipc/src/protocol/netipc_protocol.c:1493`,
    `src/libnetdata/netipc/src/protocol/netipc_protocol.c:1548`,
    `src/libnetdata/netipc/src/protocol/netipc_protocol.c:1883`,
    `src/libnetdata/netipc/src/protocol/netipc_protocol.c:1982`.
  - C regression coverage now forces near-`UINT32_MAX` builder offsets
    and expects `NIPC_ERR_OVERFLOW` for both lookup builders:
    `tests/test_protocol.c:2140`.
  - Rust raw typed clients now check lookup request-size arithmetic
    before scratch allocation:
    `src/crates/netipc/src/service/raw.rs:318`,
    `src/crates/netipc/src/service/raw.rs:363`.
  - Go raw typed clients now check lookup request-size arithmetic before
    scratch allocation:
    `src/go/pkg/netipc/service/raw/lookup_client.go:9`,
    `src/go/pkg/netipc/service/raw/lookup_client.go:42`.
- Corrected the reviewer gate state: the DeepSeek run that used the
  wrong model is discarded; a Qwen rerun timed out without a usable
  final report after code changed; neither counts as a clean final pass.
  A full reviewer rerun remains pending on the current tree.
- Applied the next reviewer findings before rerunning the gate:
  - Go builders and request encoders now check every `int`-derived wire
    `u32` field before writing it, and directory offsets use checked
    helper arithmetic:
    `src/go/pkg/netipc/protocol/lookup.go:70`,
    `src/go/pkg/netipc/protocol/lookup.go:92`,
    `src/go/pkg/netipc/protocol/lookup.go:312`,
    `src/go/pkg/netipc/protocol/lookup.go:406`,
    `src/go/pkg/netipc/protocol/lookup.go:696`,
    `src/go/pkg/netipc/protocol/lookup.go:1179`.
  - C request encoding now rejects cgroup request key lengths whose
    `path_len + 1` would overflow the wire `u32` length and records the
    padded APPS_LOOKUP C struct size as a positive static assertion:
    `src/libnetdata/netipc/src/protocol/netipc_protocol.c:231`,
    `src/libnetdata/netipc/src/protocol/netipc_protocol.c:1133`.
  - C lookup negative-path coverage now includes empty
    request/response, malformed request headers/directories, missing
    NUL, interior NUL, malformed response headers/directories, short
    items, string overlap, label padding, empty label key, inter-item
    overlap, bad PID/cgroup statuses, and known PID with empty `comm`:
    `tests/test_protocol.c:2095`,
    `tests/test_protocol.c:2129`,
    `tests/test_protocol.c:2267`.
  - Rust lookup negative-path coverage now mirrors the same important
    validation classes:
    `src/crates/netipc/src/protocol/lookup.rs:1541`,
    `src/crates/netipc/src/protocol/lookup.rs:1567`,
    `src/crates/netipc/src/protocol/lookup.rs:1660`,
    `src/crates/netipc/src/protocol/lookup.rs:1792`.
  - `docs/level2-typed-api.md` now documents the current lookup service
    contracts instead of only listing service names.
  - Rust service lookup dispatch now uses checked `item_count *
    LOOKUP_DIR_ENTRY_SIZE` arithmetic before response buffer sizing:
    `src/crates/netipc/src/service/raw.rs:1513`,
    `src/crates/netipc/src/service/raw.rs:1546`.
  - Rust lookup response builders now return `Result<usize, NipcError>`
    from `finish()` so arithmetic failure cannot be confused with a
    valid empty response, and label access/write loops use explicit
    checked offset arithmetic:
    `src/crates/netipc/src/protocol/lookup.rs:272`,
    `src/crates/netipc/src/protocol/lookup.rs:762`,
    `src/crates/netipc/src/protocol/lookup.rs:795`,
    `src/crates/netipc/src/protocol/lookup.rs:866`,
    `src/crates/netipc/src/protocol/lookup.rs:1275`,
    `src/crates/netipc/src/protocol/lookup.rs:1308`.
  - Go raw lookup dispatch now uses checked response minimum-size
    arithmetic through `lookupMinRequired()`:
    `src/go/pkg/netipc/service/raw/types.go:63`,
    `src/go/pkg/netipc/service/raw/types.go:168`,
    `src/go/pkg/netipc/service/raw/types.go:203`.
  - Go service-level lookup coverage now includes handler failure,
    reconnect/retry, and concurrent client tests:
    `src/go/pkg/netipc/service/raw/lookup_unix_test.go:237`,
    `src/go/pkg/netipc/service/raw/lookup_unix_test.go:275`,
    `src/go/pkg/netipc/service/raw/lookup_unix_test.go:309`.
  - Rust public lookup facades now have unit coverage for config mapping
    and managed-server lifecycle construction:
    `src/crates/netipc/src/service/cgroups_lookup.rs:188`,
    `src/crates/netipc/src/service/cgroups_lookup.rs:210`,
    `src/crates/netipc/src/service/cgroups_lookup.rs:232`,
    `src/crates/netipc/src/service/apps_lookup.rs:182`,
    `src/crates/netipc/src/service/apps_lookup.rs:204`,
    `src/crates/netipc/src/service/apps_lookup.rs:226`.
- Applied pre-rerun reviewer hardening:
  - Go lookup request/response `Item()` accessors now use checked directory
    arithmetic instead of raw `int(count) * dir_entry` offsets:
    `src/go/pkg/netipc/protocol/lookup.go:370`,
    `src/go/pkg/netipc/protocol/lookup.go:461`,
    `src/go/pkg/netipc/protocol/lookup.go:512`,
    `src/go/pkg/netipc/protocol/lookup.go:913`.
  - Go L1 protocol dispatch now pre-checks the minimum response buffer size
    before constructing builders, treats `Finish() == 0` as
    `ErrOverflow`, and has regression coverage for undersized buffers:
    `src/go/pkg/netipc/protocol/lookup.go:1285`,
    `src/go/pkg/netipc/protocol/lookup.go:1314`,
    `src/go/pkg/netipc/protocol/lookup_test.go:209`.
  - Rust lookup response finishing now explicitly checks both copy source
    and copy destination bounds before `copy_within`:
    `src/crates/netipc/src/protocol/lookup.rs:866`.
  - Rust response `Item()` accessors now use checked directory
    arithmetic instead of unchecked offset math:
    `src/crates/netipc/src/protocol/lookup.rs:217`,
    `src/crates/netipc/src/protocol/lookup.rs:581`,
    `src/crates/netipc/src/protocol/lookup.rs:977`.
  - Rust standalone lookup dispatch helpers now reject undersized
    response buffers before constructing builders, with regression
    coverage:
    `src/crates/netipc/src/protocol/lookup.rs:1383`,
    `src/crates/netipc/src/protocol/lookup.rs:1392`,
    `src/crates/netipc/src/protocol/lookup.rs:1415`,
    `src/crates/netipc/src/protocol/lookup.rs:1641`.
  - Go response decoding and `Item()` accessors now use checked wire
    `u32` to `int` conversions plus checked payload slicing, with a
    max-`u32` directory-offset regression test:
    `src/go/pkg/netipc/protocol/lookup.go:84`,
    `src/go/pkg/netipc/protocol/lookup.go:92`,
    `src/go/pkg/netipc/protocol/lookup.go:104`,
    `src/go/pkg/netipc/protocol/lookup_test.go:238`.
  - Go lookup builder label loops no longer shadow the checked
    `entry` variable, and `finishLookupResponse()` now guards the
    `dataOffset < firstItemAbs` underflow case:
    `src/go/pkg/netipc/protocol/lookup.go:866`,
    `src/go/pkg/netipc/protocol/lookup.go:954`,
    `src/go/pkg/netipc/protocol/lookup.go:1388`.
  - Go Windows typed-call retry now has the same overflow retry cap as
    the POSIX client:
    `src/go/pkg/netipc/service/raw/client_windows.go:194`,
    `src/go/pkg/netipc/service/raw/client_windows.go:250`.
  - C lookup dispatch helpers now have happy-path, handler-failure, and
    count-mismatch tests for both methods:
    `tests/test_protocol.c:2041`,
    `tests/test_protocol.c:2079`,
    `tests/test_protocol.c:2699`.
  - README benchmark metadata/headlines and public docs/skill text were
    updated for the lookup methods and generation-scoped cache
    invalidation:
    `README.md:203`,
    `README.md:226`,
    `docs/codec-cgroups-lookup.md:178`,
    `docs/netipc-integrator-skill.md:207`.
- Applied the Kimi blocker fixes from the corrected five-reviewer rerun:
  - README POSIX benchmark headlines now match the checked-in
    `benchmarks-posix.md` values instead of stale earlier-run numbers:
    `README.md:211`.
  - C public lookup item accessors now defensively guard
    `item_count * NIPC_LOOKUP_DIR_ENTRY_SIZE` even when called with a
    manually constructed view:
    `src/libnetdata/netipc/src/protocol/netipc_protocol.c:1224`,
    `src/libnetdata/netipc/src/protocol/netipc_protocol.c:1349`,
    `src/libnetdata/netipc/src/protocol/netipc_protocol.c:1488`,
    `src/libnetdata/netipc/src/protocol/netipc_protocol.c:1883`.
  - Go lookup label validation now uses checked 8-byte alignment, and
    Go raw response-size helper rejects negative header sizes before
    converting to unsigned arithmetic:
    `src/go/pkg/netipc/protocol/lookup.go:241`,
    `src/go/pkg/netipc/service/raw/types.go:64`.
  - Rust lookup label validation and response builder directory/label
    arithmetic now use checked multiplication/addition in the remaining
    reviewer-identified paths:
    `src/crates/netipc/src/protocol/lookup.rs:264`,
    `src/crates/netipc/src/protocol/lookup.rs:872`,
    `src/crates/netipc/src/protocol/lookup.rs:1307`.
  - C lookup dispatch and decoder tests now cover the missing
    APPS_LOOKUP count-mismatch path, request flags, too-short
    CGROUPS_LOOKUP keys, item reserved fields, string OOB/interior-NUL,
    non-KNOWN metadata rejection, non-canonical label offsets,
    oversized label tables, and APPS_LOOKUP status-dependent semantic
    violations:
    `tests/test_protocol.c:2129`,
    `tests/test_protocol.c:2286`,
    `tests/test_protocol.c:2326`,
    `tests/test_protocol.c:2469`,
    `tests/test_protocol.c:2578`,
    `tests/test_protocol.c:2680`,
    `tests/test_protocol.c:2712`.
  - POSIX benchmark generation now gates all eight lookup-method
    max-throughput scenarios, and the regenerated report shows PASS for
    each new floor:
    `tests/generate-benchmarks-posix.sh:390`,
    `benchmarks-posix.md:513`.
  - Codec docs and the integrator skill now include the lookup length
    footguns, `generation == 0` acceptance, and current typed facade
    list:
    `docs/codec-cgroups-lookup.md:63`,
    `docs/codec-apps-lookup.md:77`,
    `docs/netipc-integrator-skill.md:77`.

## Validation

Acceptance criteria evidence:

- Codec specs exist and are linked from the docs index:
  `docs/codec-cgroups-lookup.md`, `docs/codec-apps-lookup.md`,
  `README.md`, `docs/README.md`, and `docs/codec.md`.
- Shared constants/enums are implemented in all three languages:
  C `src/libnetdata/netipc/include/netipc/netipc_protocol.h:59`,
  Rust `src/crates/netipc/src/protocol/mod.rs:55`,
  Go `src/go/pkg/netipc/protocol/frame.go:51`.
- C field-offset verification is implemented in
  `src/libnetdata/netipc/src/protocol/netipc_protocol.c:169`; the
  APPS_LOOKUP item uses field-offset asserts rather than a full
  `sizeof == 60` assert, matching the SOW decision.
- Typed clients validate response item count and echoed request identity:
  C `src/libnetdata/netipc/src/service/netipc_service.c:732`,
  Rust `src/crates/netipc/src/service/raw.rs:332`,
  Go `src/go/pkg/netipc/service/raw/lookup_client.go:37`.
- Cross-language fixtures are present for the required request,
  response, empty, status, label, host-root, and unknown-PID cases in
  `tests/interop_codec.sh:70` and `tests/interop_codec.sh:74`.
- POSIX benchmark coverage exists for all required workload shapes and
  target rates in `benchmarks-posix.md:403`.
- Windows lookup service code compiles for Go and Rust targets. Windows
  lookup-method benchmark rows are no longer a close blocker after
  decision 35.

Tests or equivalent validation:

- `cmake --build build -j2`: passed.
- `cmake --build build -j2 && build/bin/test_protocol`: passed;
  `test_protocol` reported 388 passed, 0 failed after the Kimi blocker
  fixes expanded lookup negative-path and dispatch-helper coverage.
- `/usr/bin/ctest --test-dir build --output-on-failure`: passed,
  46/46 tests, total test time 443.44s after the Kimi blocker fixes.
  The shell's first `ctest` command is a broken local wrapper
  (`ModuleNotFoundError: cmake`), so validation used `/usr/bin/ctest`.
- `cd src/go && go test ./pkg/netipc/protocol ./pkg/netipc/service/raw ./pkg/netipc/service/cgroups_lookup ./pkg/netipc/service/apps_lookup`:
  passed after the latest dispatch-buffer, checked-wire-offset, and
  checked-label-alignment fixes; the raw service package took about
  77.569s.
- `cargo test --manifest-path src/crates/netipc/Cargo.toml --lib`:
  passed after the latest arithmetic, dispatch-buffer, and
  service-facade test fixes, 329/329 tests, about 30.25s.
- `bash tests/interop_codec.sh`: passed; C, Rust, and Go decoders
  accepted each other's lookup fixtures, and all lookup fixture files
  were byte-identical across C/Rust/Go. The latest decode summaries
  were Rust 89 passed / 0 failed, Go 90 passed / 0 failed, and C 101
  passed / 0 failed.
- `bash tests/generate-benchmarks-posix.sh benchmarks-posix.csv benchmarks-posix.md`:
  passed after the reviewer fixes; expected 297 data rows plus header.
  The lookup-method benchmark evidence includes 96 rows in
  `benchmarks-posix.csv`: 8 scenarios x 3 language drivers x 4 target
  rates. The generated Performance Floors section now includes PASS
  gates for all eight lookup-method max-throughput scenarios.
- `bash tests/generate-benchmarks-windows.sh benchmarks-windows.csv benchmarks-windows.md`:
  passed for the existing Windows matrix; it does not yet include lookup
  method rows.
- `GOOS=windows GOARCH=amd64 go test -c` compile checks passed for
  `./pkg/netipc/protocol`, `./pkg/netipc/service/raw`,
  `./pkg/netipc/service/cgroups_lookup`, and
  `./pkg/netipc/service/apps_lookup`.
- `cargo check --target x86_64-pc-windows-gnu --manifest-path src/crates/netipc/Cargo.toml`:
  passed.
- `bash .agents/sow/audit.sh`: passed.
- `git diff --check`: passed.
- Generated Windows Go test binaries from `go test -c` were removed after
  validation; `find src/go -maxdepth 1 -type f \( -name '*.test' -o
  -name '*.exe' \)` returned no files.

Real-use evidence:

- Real-use integration lands in SOW-B on the Netdata side. This SOW's
  executable evidence is the cross-language fixture suite, typed service
  tests, fuzz tests, and POSIX benchmark rows listed above.

Reviewer findings:

- The user requested five external reviewer runs for the complete
  implementation, SOW, specs, tests, docs, benchmarks, and unwanted side
  effects. First-pass reviewer findings were triaged against the current
  tree before any code changes.
- Confirmed and fixed: C `APPS_LOOKUP` item decode copied
  `sizeof(wire)` for a naturally padded struct even though the wire
  header is exactly 60 bytes. Fixed by copying
  `NIPC_APPS_LOOKUP_ITEM_HDR_SIZE` at
  `src/libnetdata/netipc/src/protocol/netipc_protocol.c:1637`.
- Confirmed and fixed: Go lookup label layout accumulated item sizes
  with unchecked `int` arithmetic. Fixed by adding checked add/multiply/
  align helpers and using them in request encoders and lookup builders:
  `src/go/pkg/netipc/protocol/lookup.go:78`,
  `src/go/pkg/netipc/protocol/lookup.go:560`,
  `src/go/pkg/netipc/protocol/lookup.go:650`. Added
  `TestLookupLabelLayoutOverflow` at
  `src/go/pkg/netipc/protocol/lookup_test.go:209`.
- Accepted hardening: Rust builder and request/response item slicing had
  additions that were safe after earlier validation but did not visibly
  follow the checked-arithmetic rule. Added `checked_subslice` and
  replaced the reviewed unchecked builder additions:
  `src/crates/netipc/src/protocol/lookup.rs:202`,
  `src/crates/netipc/src/protocol/lookup.rs:680`,
  `src/crates/netipc/src/protocol/lookup.rs:1139`.
- Rejected as false positives after direct evidence checks:
  - C apps request NULL-PID handling is present at
    `src/libnetdata/netipc/src/protocol/netipc_protocol.c:1226`.
  - Main string overlap rejection is present in C, Rust, and Go at
    `src/libnetdata/netipc/src/protocol/netipc_protocol.c:1366`,
    `src/libnetdata/netipc/src/protocol/netipc_protocol.c:1704`,
    `src/crates/netipc/src/protocol/lookup.rs:554`,
    `src/crates/netipc/src/protocol/lookup.rs:935`,
    `src/go/pkg/netipc/protocol/lookup.go:430`, and
    `src/go/pkg/netipc/protocol/lookup.go:712`.
  - Lookup fuzz coverage exists in C, Rust, and Go:
    `tests/fuzz_protocol.c:146`,
    `src/crates/netipc/src/protocol/mod.rs:1804`,
    `src/go/pkg/netipc/protocol/fuzz_test.go:216`.
  - Interop fixture files are generated at runtime and byte-compared by
    `tests/interop_codec.sh:77` through `tests/interop_codec.sh:127`;
    there is no committed static `.bin` fixture directory for lookup.
- Confirmed and fixed from the second-pass reviewer run: C lookup
  response builders still had unchecked internal `size_t`/offset
  arithmetic in builder initialization and item assembly. Fixed with
  checked `uint64_t` intermediates and overflow tests at
  `src/libnetdata/netipc/src/protocol/netipc_protocol.c:861`,
  `src/libnetdata/netipc/src/protocol/netipc_protocol.c:1548`,
  `src/libnetdata/netipc/src/protocol/netipc_protocol.c:1982`, and
  `tests/test_protocol.c:2140`.
- Accepted hardening from the second-pass reviewer run: Rust and Go raw
  typed clients now check lookup request-size arithmetic before scratch
  allocation at `src/crates/netipc/src/service/raw.rs:318`,
  `src/crates/netipc/src/service/raw.rs:363`, and
  `src/go/pkg/netipc/service/raw/lookup_client.go:9`.
- Confirmed and fixed from the next reviewer run: Go builders wrote some
  `int`-derived offsets and lengths to wire `u32` fields without an
  explicit `MaxUint32` guard. Added `checkedU32Int`,
  `lookupDirOffset`, and checked label-entry writes at
  `src/go/pkg/netipc/protocol/lookup.go:70`,
  `src/go/pkg/netipc/protocol/lookup.go:92`,
  `src/go/pkg/netipc/protocol/lookup.go:696`, and
  `src/go/pkg/netipc/protocol/lookup.go:1179`.
- Confirmed and fixed: C and Rust lookup unit tests did not explicitly
  cover enough of the SOW's negative-path validation list. Added
  deterministic C and Rust negative tests for malformed request and
  response layouts at `tests/test_protocol.c:2095`,
  `tests/test_protocol.c:2129`, `tests/test_protocol.c:2267`,
  `src/crates/netipc/src/protocol/lookup.rs:1541`,
  `src/crates/netipc/src/protocol/lookup.rs:1567`,
  `src/crates/netipc/src/protocol/lookup.rs:1660`, and
  `src/crates/netipc/src/protocol/lookup.rs:1792`.
- Confirmed and fixed from an in-progress GLM review that is not counted
  as a final clean pass because the tree changed while it was running:
  C `CGROUPS_LOOKUP` request encoding used `size_t key_len =
  path_len + 1`, which can wrap on 32-bit. Fixed with a checked
  `uint64_t` intermediate at
  `src/libnetdata/netipc/src/protocol/netipc_protocol.c:1133`.
- Accepted defensive cleanup: added a positive `_Static_assert` for the
  naturally padded 64-byte C APPS_LOOKUP item struct at
  `src/libnetdata/netipc/src/protocol/netipc_protocol.c:231`, and made
  the Go label write loops use checked index and `next` arithmetic.
- Confirmed and fixed from the final pre-close reviewer run: Rust service
  dispatch used checked add around an unchecked `item_count * dir_entry`
  multiply, Rust lookup builder `finish()` returned ambiguous `0` on
  internal arithmetic failure, Go raw dispatch used unchecked `int`
  response minimum-size arithmetic on 32-bit, Go lookup service tests
  lacked retry/concurrency/handler-failure coverage, and Rust public
  lookup facades lacked unit tests. The fixes are recorded in the
  Execution Log above.
- Confirmed and fixed from the pre-rerun reviewer findings: Go lookup
  `Item()` accessors had checked decode invariants but still used
  unchecked offset arithmetic internally, Rust response finishing relied
  on `copy_within` destination panics instead of explicit overflow
  errors, and Go L1 dispatch could return `(0, nil)` on a too-small
  response buffer after `Finish()` failed. The fixes and regression test
  are recorded in the Execution Log above.
- Confirmed and fixed from the last reviewer issue set before the final
  rerun: Rust standalone dispatch helpers could panic on undersized
  response buffers, Rust response `Item()` accessors still had unchecked
  directory arithmetic, Go response decoding still had unchecked wire
  `u32` to `int` conversions on 32-bit systems, Go label write loops
  shadowed checked variables, Go Windows retry handling lacked the POSIX
  overflow retry cap, C dispatch helpers lacked direct tests, and
  README/docs/skill text had stale benchmark and cache lifecycle wording.
  All were fixed and validation reran successfully.
- Confirmed and fixed from the Kimi blocking review in the corrected
  reviewer rerun: README benchmark headlines were materially stale, C
  public lookup item accessors missed defensive multiplication guards,
  Go label validation used unchecked `Align8`, Rust label validation had
  one remaining unchecked `i * LOOKUP_LABEL_ENTRY_SIZE`, APPS_LOOKUP C
  dispatch lacked count-mismatch coverage, and deterministic C decoder
  tests did not yet cover several SOW-mandated rejection classes. All
  were fixed and validation reran successfully. Additional non-blocking
  reviewer notes were also addressed where cheap and useful: lookup
  benchmark floors were added, codec docs got explicit length footguns
  and `generation == 0` acceptance, and stale typed-facade wording was
  removed from the integrator skill.
- Final corrected post-fix reviewer rerun completed with
  `llm-netdata-cloud/glm-5.1`, `llm-netdata-cloud/kimi-k2.6`,
  `llm-netdata-cloud/minimax-m2.7-coder`,
  `llm-netdata-cloud/qwen3.6-plus`, and
  `deepseek/deepseek-v4-pro`. All counted reviewers reported READY or
  READY WITH NON-BLOCKING NOTES, with no blocking correctness,
  security, interoperability, benchmark, or documentation findings.
  The earlier wrong-model DeepSeek run is explicitly discarded.
- Non-blocking reviewer notes were triaged:
  - The stale README `ctest` count was corrected from 37/37 to 46/46.
  - Rust apps-lookup negative test breadth, Rust builder panic style,
    Rust helper `unwrap()` style, Go loop checked-arithmetic style, and
    C builder sticky-error entry checks were accepted as hardening
    opportunities, not blockers. The current tree already has direct
    deterministic C tests, Rust property tests, Go fuzz/codec tests,
    cross-language fixtures, service tests, and benchmark floors that
    validate the shipped contracts.
  - APPS_LOOKUP `HOST_ROOT` zero-length string offsets were confirmed
    valid because the protocol requires empty string lengths and offsets
    at or after the fixed header, not a unique canonical offset value.
  - Go coverage-script expansion for typed lookup wrapper packages was
    not added in this SOW because those packages are thin facades over
    the raw lookup service tests already run in validation; adding a
    coverage-counting change without new typed-wrapper tests would
    change a historical headline rather than validate a new protocol
    contract.

Same-failure scan:

- Checked for repeated lookup-method benchmark, fixture, arithmetic,
  service-test, negative-test, and API omissions with `rg` over `bench/`,
  `tests/`, `src/crates/netipc/`, `src/go/pkg/netipc/`, and
  `src/libnetdata/netipc/`.
- `git diff --check` passed, covering whitespace/conflict-marker issues.

Sensitive data gate:

- Sensitive scan over the SOW, codec docs, integrator skill,
  benchmarks, and fixture sources found no real customer data, secrets,
  credentials, private endpoints, or personal names in committed
  artifacts.
- The POSIX and Windows benchmark generators were changed to write a
  sanitized machine label instead of the local hostname.
- The close-out sensitive-data scan found pre-existing Windows helper
  script paths with a local username under `/c/Users/`. These paths were
  replaced with `$HOME`/`CARGO_HOME`-based paths before commit.

Artifact maintenance gate:

- AGENTS.md: no change needed; workflow and SOW rules are unchanged.
- Runtime project skills: no runtime `project-*` skills exist.
- Specs: updated via `docs/codec-cgroups-lookup.md`,
  `docs/codec-apps-lookup.md`, `docs/codec.md`, `docs/README.md`, and
  `README.md`.
- End-user/operator docs: `docs/level2-typed-api.md` updated because
  public typed APIs changed.
- End-user/operator skills: `docs/netipc-integrator-skill.md` updated
  because integration guidance changed.
- SOW lifecycle: completed after the final corrected post-fix reviewer
  rerun was triaged, with SOW file moved to `.agents/sow/done/`.

Specs update:

- Completed for the new protocol behavior:
  `docs/codec-cgroups-lookup.md` and `docs/codec-apps-lookup.md`.

Project skills update:

- No update needed; this repo has no runtime project skills today.

End-user/operator docs update:

- `docs/level2-typed-api.md` updated for the new typed service APIs.

End-user/operator skills update:

- `docs/netipc-integrator-skill.md` updated for the new lookup methods.

Lessons:

- The benchmark generators must not publish raw local hostnames because
  hostnames can contain personal data.
- The typed clients must validate echoed request identity, not just
  structural decode success; the protocol docs now make this explicit.
- C protocol code must avoid broad search/replace edits around repeated
  `memcpy(&wire, ...)` patterns; APPS_LOOKUP's 60-byte wire header is
  intentionally smaller than the naturally aligned C struct.
- The post-implementation reviewer gate should include service-level
  integration coverage, not only codec and cross-language fixture
  coverage, because lookup correctness also depends on retry and
  concurrency behavior in the raw service layer.

Follow-up mapping:

- SOW-B (Netdata repo) remains the tracked follow-up for the Netdata
  integration and vendored-copy sync. This SOW does not create a
  plugin-ipc follow-up file for Netdata-side work because it belongs to
  the Netdata repository.

## Outcome

Completed. The repository now has additive `CGROUPS_LOOKUP` and
`APPS_LOOKUP` netipc methods across C, Rust, and Go, with codec specs,
typed service APIs, cross-language fixtures, tests, fuzz coverage,
benchmark generation, and POSIX benchmark evidence. Windows lookup
benchmark rows remain out of scope by recorded decision, but Go and Rust
lookup service code compile for Windows targets.

The Netdata-side consumer integration and vendored-copy sync remain
tracked by SOW-B in the Netdata repository.

## Lessons Extracted

- Reviewer gates must pin exact model names when a user specifies them;
  wrong-model results are not reusable evidence.
- Lookup-method correctness spans codec layout, typed service dispatch,
  client identity checks, and benchmark coverage. Reviewing only the
  codec files misses real failure modes.
- APPS_LOOKUP's 60-byte wire item header is intentionally smaller than
  the naturally aligned C struct; future C changes must keep using the
  explicit wire-size constant for serialization.
- Benchmark headline documentation must be regenerated or manually
  checked after benchmark artifacts change.

## Followup

- **SOW-B (Netdata repo)**: integration work that consumes the two new
  netipc methods. Scope:
  - Refactor cgroup mode / canonical path detection from
    `src/collectors/cgroups.plugin/` into `src/libnetdata/cgroups/`.
    This refactor MUST land as a **single isolated commit**,
    reviewable on its own, with **no functional change** to current
    cgroup detection behavior (decisions 13 and 14).
  - Add the `CGROUPS_LOOKUP` server to `cgroups.plugin`, using the
    new libnetdata module for cgroup detection and the new netipc
    method. Includes the reactive-discovery queue (per-request
    trigger that adds unknown cgroups to a serialized discovery queue
    inside `cgroups.plugin`), with throttling to prevent
    thundering-herd on cold start. Maps `cgroups.plugin`'s discovery
    results onto the three response status values (KNOWN /
    UNKNOWN_RETRY_LATER / UNKNOWN_PERMANENT).
  - Add `/proc/<pid>/cgroup` reading to `apps.plugin` (using the new
    libnetdata helpers), one-time per PID per lifetime. Apply the v1
    controller precedence cpuacct -> blkio -> memory (decision 10).
    Short-circuit root cgroup paths (`/`) at this stage and tag the
    PID locally as `HOST_ROOT` (decision 9, used by APPS_LOOKUP
    responses).
  - Add the `CGROUPS_LOOKUP` client + three-state cache
    (KNOWN / UNKNOWN_RETRY / UNKNOWN_PERMANENT) inside `apps.plugin`.
    End-of-iteration cleanup removes entries not seen this cycle.
    Track `generation` per service and evict UNKNOWN_PERMANENT on
    decrease/reset before processing the response.
  - Add the `APPS_LOOKUP` server to `apps.plugin`, returning
    fully-joined PID + cgroup data with `cgroup_status` populated from
    the CGROUPS_LOOKUP cache and from the host-root short-circuit.
  - Add the `APPS_LOOKUP` client + per-PID cache inside
    `network-viewer.plugin`, with comm-based PID-reuse detection,
    generation-based UNKNOWN_PERMANENT eviction (evict-before-process),
    and end-of-iteration cleanup of unreferenced entries.
  - Wire the new container/service aggregation modes into the topology
    Function (`containers:by_name`), with the fallback chain
    container_name -> service_name -> process_name so every actor is
    always mapped. Drop network-viewer's own use of `net_ns_inode` for
    container classification (decision 5).
  - Re-vendor the updated `plugin-ipc` library into Netdata; confirm
    `diff-netdata-vendor.sh` reports no drift after vendor sync.

## Regression Log

None yet.

Append regression entries here only after this SOW was completed or
closed and later testing or use found broken behavior. Use a dated
`## Regression - YYYY-MM-DD` heading at the end of the file. Never
prepend regression content above the original SOW narrative.

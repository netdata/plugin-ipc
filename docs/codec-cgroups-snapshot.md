# Codec: cgroups Snapshot Message Type

## Purpose

This document defines the wire-level payload contract for the cgroups
snapshot message type. This is the first concrete method family in the
plugin-ipc library, designed to replace the existing ad-hoc
cgroups.plugin to ebpf.plugin shared-memory communication in Netdata.

This contract must be implemented identically in C, Rust, and Go.
Any implementation that produces or consumes bytes differently from
this specification is wrong.

## Service identity

- service_namespace: `/run/netdata` (POSIX), derived named pipe namespace
  (Windows)
- service_name: `cgroups-snapshot`

Discovery and ownership model:

- clients resolve `cgroups-snapshot` by service identity only
- clients do not know which plugin/process provides it
- the endpoint serves the `cgroups-snapshot` request kind only
- the provider may start late, restart, or disappear; clients are expected
  to reconnect through higher layers

## Method code

The outer envelope's `code` field carries method code `2`
(`CGROUPS_SNAPSHOT`) as defined in the wire envelope spec.

## Design constraints

This contract is designed to be usable later with the real cgroups and
ebpf plugins exactly as-is. It preserves the semantic fields actually
used by the real consumer, while excluding transport/internal fields
(such as body_length) that do not belong in a public service contract.

The identity model matches current Netdata behavior: items are identified
by `hash + name`, where hash is `simple_hash(name)` as computed by the
producer.

## Request payload

The snapshot refresh request is minimal. The first implementation always
returns the full snapshot (no generation-aware or differential refresh).

### Wire layout

| Offset | Size | Type | Field | Description |
|--------|------|------|-------|-------------|
| 0 | 2 | u16 | layout_version | Payload schema version |
| 2 | 2 | u16 | flags | Reserved, must be 0 |

Total fixed size: 4 bytes.

No variable-length fields in the request payload.

### Encoding

The encoder takes no meaningful input beyond the layout version and
produces a 4-byte payload.

### Decoding

The decoder validates:

- Payload length >= 4 bytes
- layout_version is recognized

Returns a request view (trivial for this method type, but the decode
path must still exist for consistency and forward compatibility).

## Response payload

The response carries the full snapshot: snapshot-level metadata followed
by per-item data.

### Snapshot-level fields

| Offset | Size | Type | Field | Description |
|--------|------|------|-------|-------------|
| 0 | 2 | u16 | layout_version | Payload schema version, must be `1` |
| 2 | 2 | u16 | flags | Reserved, must be `0` |
| 4 | 4 | u32 | item_count | Number of snapshot items |
| 8 | 4 | u32 | systemd_enabled | Whether systemd cgroup integration is active |
| 12 | 4 | u32 | reserved | Reserved, must be `0` |
| 16 | 8 | u64 | generation | Snapshot generation / version number |

Total fixed snapshot header: 24 bytes.

The `item_count` field is the authoritative source for the number of
snapshot items in this response. It is internal to the snapshot payload
and unrelated to the Level 1 outer envelope's `item_count` (which is
always 1 for this service kind).

### Per-item directory

Immediately after the snapshot header, there is an item directory with
item_count entries:

| Offset (relative to directory start) | Size | Type | Field |
|--------------------------------------|------|------|-------|
| 0 | 4 | u32 | offset | Byte offset of item payload from start of packed item area |
| 4 | 4 | u32 | length | Byte length of item payload |

Each entry is 8 bytes. Total directory size: item_count * 8 bytes.

A cgroups snapshot response is always a single Level 1 message
(item_count = 1 in the outer envelope, BATCH flag NOT set). The entire
snapshot — header, item directory, and packed items — is contained in
one self-contained response payload. The item directory here is
method-internal, not a Level 1 batch directory.

### Per-item payload

Each item payload is self-contained:

| Offset | Size | Type | Field | Description |
|--------|------|------|-------|-------------|
| 0 | 2 | u16 | layout_version | Must be `1` |
| 2 | 2 | u16 | flags | Reserved, must be `0` |
| 4 | 4 | u32 | hash | simple_hash(name), identity field |
| 8 | 4 | u32 | options | Cgroup option flags |
| 12 | 4 | u32 | enabled | Whether this cgroup is enabled |
| 16 | 4 | u32 | name_offset | Offset of name string (from item start) |
| 20 | 4 | u32 | name_length | Length of name string (excluding NUL) |
| 24 | 4 | u32 | path_offset | Offset of path string (from item start) |
| 28 | 4 | u32 | path_length | Length of path string (excluding NUL) |

Total fixed item header: 32 bytes.

Following the fixed header is the packed variable data area for this
item, containing:

- name bytes followed by NUL (`\0`)
- path bytes followed by NUL (`\0`)

String offsets are relative to the start of the item payload (byte 0
of the item). The first valid string offset is 32 (immediately after
the fixed 32-byte item header).

### Alignment

All item payloads must be aligned to 8-byte boundaries within the
packed item area. The encoder must insert padding between items as
needed.

## Encoding (response)

The response encoder (used by the server/producer) operates through a
builder:

1. Initialize or reset the builder with snapshot-level fields
   (systemd_enabled, generation)
2. For each cgroup item: add the item through the builder, providing
   hash, options, enabled, name, and path
3. The builder internally manages:
   - Item directory construction (offset + length per item)
   - Variable data packing with trailing NUL
   - Alignment and padding
4. Finalize the builder to produce the complete response payload bytes

The builder exposes semantic field names only. Handlers never compute
offsets, manage padding, or insert NUL terminators.

## Decoding (response)

The response decoder (used by the client/consumer) takes the response
payload byte range and produces an ephemeral snapshot view:

1. Validate snapshot header (layout_version, payload size)
2. Validate the item directory (item_count entries, all offsets/lengths
   within bounds)
3. Provide access to snapshot-level fields (item_count,
   systemd_enabled, generation)
4. Provide per-item access by index, returning an ephemeral item view
   with:
   - hash (u32)
   - options (u32)
   - enabled (u8/bool)
   - name_view (pointer/slice into payload bytes, NUL-terminated)
   - path_view (pointer/slice into payload bytes, NUL-terminated)

All views are ephemeral. They borrow the underlying payload bytes and
are valid only within the current callback or library call.

### Validation rules

The decoder must reject:

- Payload shorter than the fixed snapshot header (24 bytes)
- Unknown layout_version
- item_count inconsistent with remaining payload size
- Any item directory entry whose offset + length exceeds the packed
  item area
- Any item payload shorter than the fixed item header (32 bytes)
- Any string field whose offset + length exceeds the item's variable
  data area
- Any string field missing the trailing NUL at the expected position
- Any item whose name and path byte regions (including trailing NUL)
  overlap — that is, where `name_offset < path_offset + path_length + 1`
  AND `path_offset < name_offset + name_length + 1`

## Batch semantics

Batching does not apply to the cgroups snapshot method. Both request
and response are always single non-batch Level 1 messages (item_count
= 1, BATCH flag NOT set). A refresh is always one request producing
one self-contained snapshot response.

## Lookup identity

Items are identified by `hash + name`. This matches the current Netdata
consumer behavior where the ebpf plugin keys on
`hash == ptr->hash && strcmp(name, ptr->name) == 0`.

Consumers that build local caches from the snapshot should use this
composite key for lookup.

## Testing requirements

- **Round-trip tests**: encode a snapshot, decode it, verify all fields
  match — in all languages.
- **Cross-language interop tests**: encode in language A, decode in
  language B — for all language pairs (C, Rust, Go).
- **Boundary tests**:
  - Empty snapshot (item_count = 0)
  - Single-item snapshot
  - Maximum-item snapshot (at the negotiated response payload size limit)
  - Items with empty strings (name_length = 0, path_length = 0, but
    NUL terminator still present)
  - Items with maximum-length strings (at the negotiated payload limit)
  - Items with long names and long paths simultaneously
- **Validation rejection tests**: every rejection rule listed above,
  including truncated payloads, out-of-bounds offsets, missing NUL.
- **Fuzz tests**: feed arbitrary bytes to the decode function. No input
  may crash or panic. Invalid inputs must produce clean errors.
- **Deterministic corpus**: a production-shaped fake corpus of varied
  items (systemd services, containers, user slices, machine slices)
  must be used across all test suites to ensure realistic coverage.

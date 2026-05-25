# Codec: apps Lookup Message Type

## Purpose

`APPS_LOOKUP` lets a client ask for metadata for specific PIDs. The
server returns process fields plus cgroup metadata that has already been
joined server-side.

This contract must be implemented identically in C, Rust, and Go. Any
implementation that produces or consumes bytes differently from this
specification is wrong.

## Service Identity

- service_namespace: `/run/netdata` on POSIX, derived named pipe namespace on Windows
- service_name: `apps-lookup`
- method code: `5` (`NIPC_METHOD_APPS_LOOKUP`)
- outer envelope: one non-batch request and one non-batch response

The method-internal item directory is not a Level 1 batch directory.

## Shared Constants

- `NIPC_APPS_LOOKUP_REQ_HDR_SIZE`: 16
- `NIPC_APPS_LOOKUP_RESP_HDR_SIZE`: 16
- `NIPC_APPS_LOOKUP_ITEM_HDR_SIZE`: 60
- `NIPC_LOOKUP_DIR_ENTRY_SIZE`: 8
- `NIPC_LOOKUP_LABEL_ENTRY_SIZE`: 16
- `NIPC_APPS_LOOKUP_KEY_SIZE`: 8
- `NIPC_UID_UNSET`: `0xFFFFFFFF`

The C implementation must not use `sizeof(struct)` as the APPS item
wire length. Natural C alignment pads the 60-byte header to 64 bytes.
Use explicit wire-size constants and field-offset assertions.

## Enums

PID status:

| Value | Name |
|------:|------|
| 0 | `KNOWN` |
| 1 | `UNKNOWN` |

Cgroup status:

| Value | Name |
|------:|------|
| 0 | `KNOWN` |
| 1 | `UNKNOWN_RETRY_LATER` |
| 2 | `UNKNOWN_PERMANENT` |
| 3 | `HOST_ROOT` |

Decoders must reject unknown status and cgroup-status values.

Shared orchestrator values:

| Value | Name |
|------:|------|
| 0 | `UNKNOWN` |
| 1 | `SYSTEMD` |
| 2 | `DOCKER` |
| 3 | `K8S` |
| 4 | `KVM` |
| 5 | `LXC` |
| 6 | `PODMAN` |
| 7 | `NSPAWN` |

Decoders must accept unknown orchestrator values and expose the raw `u16`
to the caller.

## Request Payload

The request contains zero or more PID keys. `item_count == 0` is valid
and means a no-op probe.

Footgun: APPS_LOOKUP request keys are fixed 8-byte binary structs, not
strings. Request key directory `length` is always `8`; there is no
trailing NUL. Response string length fields exclude the trailing NUL,
but the NUL byte must still be present at `offset + length`.

Fixed request header, 16 bytes:

| Offset | Size | Type | Field | Rule |
|-------:|-----:|------|-------|------|
| 0 | 2 | u16 | layout_version | must be `1` |
| 2 | 2 | u16 | flags | must be `0` |
| 4 | 4 | u32 | item_count | number of PID keys |
| 8 | 4 | u32 | reserved0 | must be `0` |
| 12 | 4 | u32 | reserved1 | must be `0` |

The per-key directory starts at byte 16. Each entry is 8 bytes:

| Offset | Size | Type | Field |
|-------:|-----:|------|-------|
| 0 | 4 | u32 | offset from packed key area start |
| 4 | 4 | u32 | length |

Each key payload is exactly 8 bytes:

| Offset | Size | Type | Field | Rule |
|-------:|-----:|------|-------|------|
| 0 | 4 | u32 | pid | numeric lookup key |
| 4 | 4 | u32 | reserved | must be `0` |

Request validation:

- `layout_version == 1`
- `flags == 0`
- `reserved0 == 0` and `reserved1 == 0`
- directory multiplication and `offset + length` use checked arithmetic
- every key offset is 8-byte aligned
- every directory length is exactly 8
- each key reserved field is `0`

## Response Payload

Fixed response header, 16 bytes:

| Offset | Size | Type | Field | Rule |
|-------:|-----:|------|-------|------|
| 0 | 2 | u16 | layout_version | must be `1` |
| 2 | 2 | u16 | flags | must be `0` |
| 4 | 4 | u32 | item_count | equals request item_count |
| 8 | 8 | u64 | generation | advisory service generation |

The item directory follows the header and uses the same 8-byte entry
shape as the request.
Decoders must accept every `generation` value, including `0`.

Per-item fixed header, 60 bytes:

| Offset | Size | Type | Field | Rule |
|-------:|-----:|------|-------|------|
| 0 | 2 | u16 | layout_version | must be `1` |
| 2 | 2 | u16 | status | PID status enum |
| 4 | 2 | u16 | orchestrator | raw shared enum value |
| 6 | 2 | u16 | cgroup_status | cgroup-status enum |
| 8 | 4 | u32 | pid | echoed request PID |
| 12 | 4 | u32 | ppid | parent PID, or `0` |
| 16 | 4 | u32 | uid | process UID, or `NIPC_UID_UNSET` |
| 20 | 4 | u32 | reserved0 | must be `0` |
| 24 | 8 | u64 | starttime | Linux jiffies since boot |
| 32 | 4 | u32 | comm_offset | `>= 60` |
| 36 | 4 | u32 | comm_length | max 15 |
| 40 | 4 | u32 | cgroup_path_offset | `>= 60` |
| 44 | 4 | u32 | cgroup_path_length | status-dependent |
| 48 | 4 | u32 | cgroup_name_offset | `>= 60` |
| 52 | 4 | u32 | cgroup_name_length | status-dependent |
| 56 | 2 | u16 | label_count | status-dependent |
| 58 | 2 | u16 | reserved1 | must be `0` |

Every string has an offset and a trailing NUL, including empty strings.
An empty string has `length == 0` and still occupies its own NUL byte.
Two zero-length strings cannot share the same NUL byte.

## Field Semantics

For `status == UNKNOWN`:

- `orchestrator == 0`
- `cgroup_status == 0`
- `ppid == 0`
- `uid == NIPC_UID_UNSET`
- `starttime == 0`
- `comm_length == 0`
- `cgroup_path_length == 0`
- `cgroup_name_length == 0`
- `label_count == 0`

For `status == KNOWN`:

- `comm_length` is 1 to 15 bytes
- `starttime` is Linux `/proc/<pid>/stat` field 22 in jiffies since boot
- non-Linux encoders set `starttime == 0`

For `cgroup_status == KNOWN`:

- `cgroup_path_length >= 1`
- `cgroup_name_length` may be `0`
- labels may be present
- `orchestrator` is application meaningful

For `cgroup_status == UNKNOWN_RETRY_LATER` or `UNKNOWN_PERMANENT`:

- `cgroup_path_length >= 1`
- `orchestrator == 0`
- `cgroup_name_length == 0`
- `label_count == 0`

For `cgroup_status == HOST_ROOT`:

- `orchestrator == 0`
- `cgroup_path_length == 0`
- `cgroup_name_length == 0`
- `label_count == 0`

## Labels

Label entries begin at the canonical table offset: the first 8-byte
aligned offset greater than or equal to the end of the preceding `comm`,
`cgroup_path`, and `cgroup_name` strings, including their NUL
terminators.

Each label entry is 16 bytes:

| Offset | Size | Type | Field |
|-------:|-----:|------|-------|
| 0 | 4 | u32 | key_offset |
| 4 | 4 | u32 | key_length |
| 8 | 4 | u32 | value_offset |
| 12 | 4 | u32 | value_length |

Padding before the label table must be zero. Label key and value strings
are packed immediately after the table in table order. Label keys must
not be empty. Empty label values are allowed.

## Decoder Validation

The decoder must reject:

- truncated headers or directories
- unknown layout versions
- non-zero flags or reserved fields
- directory count multiplication overflow
- directory `offset + length` overflow or out-of-bounds ranges
- overlapping item ranges
- unaligned item offsets
- item payloads shorter than 60 bytes
- unknown PID status values
- unknown cgroup-status values
- `comm_length > 15`
- status-dependent field violations listed above
- string offsets before byte 60
- string `offset + length + 1` overflow or out-of-bounds ranges
- missing trailing NUL
- interior NUL bytes inside any response string
- overlapping string byte ranges, including trailing NUL bytes
- non-canonical label table offset
- non-zero padding before the label table
- empty label keys
- label table byte-count overflow
- any label string out of bounds or missing its trailing NUL

## Typed Client Responsibilities

The wire decoder validates structure. The typed client must also verify:

- response `item_count` equals request `item_count`
- response item `N` echoes request PID `N`
- cache users track the response `generation`; on generation decrease or
  reset, evict cached `UNKNOWN_PERMANENT` and `HOST_ROOT` entries before
  processing the new response

An echoed-PID mismatch is a server bug and the typed client must fail the
response.

## Security Considerations

Request PIDs are numeric lookup keys. A server must not treat a request
PID as permission to perform privileged process operations.

An authorized local client can probe for process metadata and cgroup
metadata: `pid`, `ppid`, `uid`, `starttime`, `comm`, cgroup path, cgroup
name, and labels. This exposure is bounded by the localhost transport,
socket or pipe permissions, and the existing auth-token handshake.

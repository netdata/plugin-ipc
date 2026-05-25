# Codec: cgroups Lookup Message Type

## Purpose

`CGROUPS_LOOKUP` lets a client ask for metadata for specific cgroup
paths it has already discovered. It is a pull-oriented alternative to a
full cgroups snapshot when the consumer's working set is small.

This contract must be implemented identically in C, Rust, and Go. Any
implementation that produces or consumes bytes differently from this
specification is wrong.

## Service Identity

- service_namespace: `/run/netdata` on POSIX, derived named pipe namespace on Windows
- service_name: `cgroups-lookup`
- method code: `4` (`NIPC_METHOD_CGROUPS_LOOKUP`)
- outer envelope: one non-batch request and one non-batch response

The method-internal item directory is not a Level 1 batch directory.

## Shared Constants

- `NIPC_CGROUPS_LOOKUP_REQ_HDR_SIZE`: 16
- `NIPC_CGROUPS_LOOKUP_RESP_HDR_SIZE`: 16
- `NIPC_CGROUPS_LOOKUP_ITEM_HDR_SIZE`: 28
- `NIPC_LOOKUP_DIR_ENTRY_SIZE`: 8
- `NIPC_LOOKUP_LABEL_ENTRY_SIZE`: 16

## Enums

Per-item status:

| Value | Name |
|------:|------|
| 0 | `KNOWN` |
| 1 | `UNKNOWN_RETRY_LATER` |
| 2 | `UNKNOWN_PERMANENT` |

Decoders must reject unknown status values.

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

The request contains zero or more cgroup path keys. `item_count == 0` is
valid and means a no-op probe.

Length convention: request directory `length` includes the trailing NUL
byte (`path_length + 1`). Response string length fields exclude the
trailing NUL, but the NUL byte must still be present at
`offset + length`.

Fixed request header, 16 bytes:

| Offset | Size | Type | Field | Rule |
|-------:|-----:|------|-------|------|
| 0 | 2 | u16 | layout_version | must be `1` |
| 2 | 2 | u16 | flags | must be `0` |
| 4 | 4 | u32 | item_count | number of path keys |
| 8 | 4 | u32 | reserved0 | must be `0` |
| 12 | 4 | u32 | reserved1 | must be `0` |

The per-key directory starts at byte 16. Each entry is 8 bytes:

| Offset | Size | Type | Field |
|-------:|-----:|------|-------|
| 0 | 4 | u32 | offset from packed key area start |
| 4 | 4 | u32 | length including trailing NUL |

The packed key area starts at `16 + item_count * 8`. Each key is exactly
`path bytes + NUL`. Keys are aligned to 8-byte boundaries relative to
the packed key area.

Request key validation:

- `layout_version == 1`
- `flags == 0`
- `reserved0 == 0` and `reserved1 == 0`
- directory multiplication and `offset + length` use checked arithmetic
- every key offset is 8-byte aligned
- every key length is at least 2 bytes
- final key byte is NUL
- path bytes before the trailing NUL contain no interior NUL

## Response Payload

Fixed response header, 16 bytes:

| Offset | Size | Type | Field | Rule |
|-------:|-----:|------|-------|------|
| 0 | 2 | u16 | layout_version | must be `1` |
| 2 | 2 | u16 | flags | must be `0` |
| 4 | 4 | u32 | item_count | equals request item_count |
| 8 | 8 | u64 | generation | advisory service generation |

The response header is not the request header: bytes 8-15 carry a `u64`
generation, not two reserved `u32` fields.
Decoders must accept every `generation` value, including `0`.

The item directory follows the header and uses the same 8-byte entry
shape as the request. Directory offsets are relative to the packed item
area, not to the beginning of the whole payload.

Per-item fixed header, 28 bytes:

| Offset | Size | Type | Field | Rule |
|-------:|-----:|------|-------|------|
| 0 | 2 | u16 | layout_version | must be `1` |
| 2 | 2 | u16 | status | status enum |
| 4 | 2 | u16 | orchestrator | raw shared enum value |
| 6 | 2 | u16 | reserved0 | must be `0` |
| 8 | 4 | u32 | path_offset | `>= 28` |
| 12 | 4 | u32 | path_length | `>= 1` |
| 16 | 4 | u32 | name_offset | `>= 28` |
| 20 | 4 | u32 | name_length | may be `0` |
| 24 | 2 | u16 | label_count | number of labels |
| 26 | 2 | u16 | reserved1 | must be `0` |

Every string has an offset and a trailing NUL, including empty strings.
An empty string has `length == 0` and still occupies its own NUL byte.
Two zero-length strings cannot share the same NUL byte.

For `status == KNOWN`:

- `path_length >= 1`
- `name_length` may be `0`
- `label_count` may be `0`
- `orchestrator` is application meaningful

For `status != KNOWN`:

- `path_length >= 1`
- `orchestrator == 0`
- `name_length == 0`
- `label_count == 0`

## Labels

Label entries begin at the canonical table offset: the first 8-byte
aligned offset greater than or equal to the end of the preceding path
and name strings, including their NUL terminators.

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
- item payloads shorter than 28 bytes
- unknown status values
- status-dependent field violations: empty `path`, non-zero
  `orchestrator`, non-empty `name`, or non-zero `label_count` on
  non-`KNOWN` items
- string offsets before byte 28
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
- response item `N` echoes request path `N`
- cache users track the response `generation`; on generation decrease or
  reset, evict cached `UNKNOWN_PERMANENT` entries before processing the
  new response

An echoed-path mismatch is a server bug and the typed client must fail
the response.

## Security Considerations

Request paths are opaque lookup keys. A server must not resolve,
normalize, open, or traverse filesystem paths from request content.
Characters such as `..` have no protocol meaning.

An authorized local client can probe whether the provider knows a cgroup
path. This exposure is bounded by the localhost transport, socket or
pipe permissions, and the existing auth-token handshake.

# Level 1: Wire Envelope Contract

## Purpose

This document defines the common message framing shared by all transports.
Every message on the wire — whether carried over UDS, Named Pipe, or SHM —
uses these exact byte layouts. All implementations (C, Rust, Go) must
produce and consume identical bytes for the same logical message.

All multi-byte fields are little-endian.

## Outer message header (32 bytes)

Every message begins with this fixed header.

| Offset | Size | Type | Field | Description |
|--------|------|------|-------|-------------|
| 0 | 4 | u32 | magic | Must be `0x4e495043` ("NIPC") |
| 4 | 2 | u16 | version | Must be `1` |
| 6 | 2 | u16 | header_len | Must be `32` |
| 8 | 2 | u16 | kind | Message kind |
| 10 | 2 | u16 | flags | Bit flags |
| 12 | 2 | u16 | code | Method id or control opcode |
| 14 | 2 | u16 | transport_status | Envelope-level status |
| 16 | 4 | u32 | payload_len | Bytes after the header |
| 20 | 4 | u32 | item_count | 1 for single, N for batch |
| 24 | 8 | u64 | message_id | Request/response correlation |

Total: 32 bytes. Enforced by compile-time assertions.

### Message kind values

| Value | Name | Meaning |
|-------|------|---------|
| 1 | REQUEST | Client-to-server request |
| 2 | RESPONSE | Server-to-client response |
| 3 | CONTROL | Protocol control message (handshake) |

### Flag values

| Bit | Name | Meaning |
|-----|------|---------|
| 0 | BATCH | `0x0001` — payload contains batch item directory + packed items |

### Transport status values

These are envelope-level / protocol-level only. They never represent
business or method outcomes.

| Value | Name | Meaning |
|-------|------|---------|
| 0 | OK | Message processed successfully at envelope level |
| 1 | BAD_ENVELOPE | Malformed header or payload structure |
| 2 | AUTH_FAILED | Authentication token rejected |
| 3 | INCOMPATIBLE | Protocol version or limit negotiation failed |
| 4 | UNSUPPORTED | Requested profile or method not supported |
| 5 | LIMIT_EXCEEDED | Payload or batch count exceeds negotiated limit |
| 6 | INTERNAL_ERROR | Unrecoverable internal error |

### Control opcodes

Used in the `code` field when `kind = CONTROL`:

| Value | Name | Meaning |
|-------|------|---------|
| 1 | HELLO | Client initiates handshake |
| 2 | HELLO_ACK | Server responds to handshake |

### Method codes

Used in the `code` field when `kind = REQUEST` or `kind = RESPONSE`:

| Value | Name | Meaning |
|-------|------|---------|
| 1 | INCREMENT | Test/benchmark method |
| 2 | CGROUPS_SNAPSHOT | Cgroups snapshot refresh |

New methods are assigned sequential codes. The method code space is
shared across all services — each method has a globally unique code.

## Batch item directory

When `flags & BATCH` is set and `item_count > 1`, the payload begins
with an item directory followed by packed item payloads.

Batches are homogeneous: the outer header's `code` field identifies the
method type for all items in the batch. Mixed-method batches are not
representable in the wire format.

### Item directory entry (8 bytes)

| Offset | Size | Type | Field | Description |
|--------|------|------|-------|-------------|
| 0 | 4 | u32 | offset | Byte offset from start of packed item area |
| 4 | 4 | u32 | length | Byte length of item payload |

### Item alignment

Items are aligned to 8-byte boundaries within the packed item area.
Encoders must insert padding between items as needed. Item offsets
must be multiples of 8.

### Batch payload layout

```
[item_ref[0] ... item_ref[item_count-1]]   (item_count * 8 bytes)
[padding to 8-byte boundary if needed]
[item 0 payload]                            (aligned to 8 bytes)
[padding]
[item 1 payload]                            (aligned to 8 bytes)
[padding]
...
[item N-1 payload]                          (aligned to 8 bytes)
```

### Single message layout

When `item_count = 1` and `BATCH` flag is not set, there is no item
directory. The payload follows the header directly as one self-contained
method/control payload.

## Chunk continuation header (32 bytes)

When a message exceeds the negotiated packet size, it is split into
chunks. The first chunk carries the original outer message header.
Continuation chunks (chunk_index > 0) carry this header instead:

| Offset | Size | Type | Field | Description |
|--------|------|------|-------|-------------|
| 0 | 4 | u32 | magic | Must be `0x4e43484b` ("NCHK") |
| 4 | 2 | u16 | version | Must be `1` |
| 6 | 2 | u16 | flags | Reserved, must be `0` |
| 8 | 8 | u64 | message_id | Must match the original message |
| 16 | 4 | u32 | total_message_len | Total original message size (header + payload) |
| 20 | 4 | u32 | chunk_index | 0-based chunk sequence number |
| 24 | 4 | u32 | chunk_count | Total number of chunks |
| 28 | 4 | u32 | chunk_payload_len | This chunk's payload size in bytes |

Total: 32 bytes.

### Chunking rules

- The first chunk contains the original 32-byte outer header plus as
  many payload bytes as fit in one packet.
- Continuation chunks contain this 32-byte chunk header plus payload
  bytes.
- Chunk payload budget per packet = negotiated_packet_size - 32 bytes.
- `chunk_count` must be > 0.
- `chunk_index` must be < `chunk_count`.
- `chunk_payload_len` must be > 0.
- `total_message_len` must be > 0.
- The receiver validates `magic`, `message_id`, `chunk_index`, and
  `chunk_count` on every continuation chunk. Any mismatch is a
  protocol violation.

## Handshake payloads

### Hello payload (44 bytes)

Sent by the client as a CONTROL message with `code = HELLO`.

| Offset | Size | Type | Field | Description |
|--------|------|------|-------|-------------|
| 0 | 2 | u16 | layout_version | Must be `1` |
| 2 | 2 | u16 | flags | Reserved, must be `0` |
| 4 | 4 | u32 | supported_profiles | Bitmask of client's supported profiles |
| 8 | 4 | u32 | preferred_profiles | Bitmask of client's preferred profiles |
| 12 | 4 | u32 | max_request_payload_bytes | Client's request payload ceiling |
| 16 | 4 | u32 | max_request_batch_items | Client's request batch item ceiling |
| 20 | 4 | u32 | max_response_payload_bytes | Client's response payload ceiling |
| 24 | 4 | u32 | max_response_batch_items | Client's response batch item ceiling |
| 28 | 4 | - | padding | Reserved, must be `0` |
| 32 | 8 | u64 | auth_token | Caller-supplied authentication token |
| 40 | 4 | u32 | packet_size | Client's transport packet size |

Total: 44 bytes.

### Hello-ack payload (36 bytes)

Sent by the server as a CONTROL message with `code = HELLO_ACK`.

| Offset | Size | Type | Field | Description |
|--------|------|------|-------|-------------|
| 0 | 2 | u16 | layout_version | Must be `1` |
| 2 | 2 | u16 | flags | Reserved, must be `0` |
| 4 | 4 | u32 | server_supported_profiles | Server's supported profiles |
| 8 | 4 | u32 | intersection_profiles | AND of client and server support |
| 12 | 4 | u32 | selected_profile | Profile chosen for this session |
| 16 | 4 | u32 | agreed_max_request_payload_bytes | Negotiated request payload limit |
| 20 | 4 | u32 | agreed_max_request_batch_items | Negotiated request batch item limit |
| 24 | 4 | u32 | agreed_max_response_payload_bytes | Negotiated response payload limit |
| 28 | 4 | u32 | agreed_max_response_batch_items | Negotiated response batch item limit |
| 32 | 4 | u32 | agreed_packet_size | Negotiated packet size = min(client, server) |

Total: 36 bytes.

### Profile bitmask values

Profile bits are transport-specific. POSIX and Windows use the same
bit positions where possible:

| Bit | Value | POSIX name | Windows name |
|-----|-------|-----------|-------------|
| 0 | `0x01` | UDS_SEQPACKET | NAMED_PIPE |
| 1 | `0x02` | SHM_HYBRID | SHM_HYBRID |
| 2 | `0x04` | SHM_FUTEX | SHM_BUSYWAIT |
| 3 | `0x08` | — | SHM_WAITADDR |

Bit 0 is always the baseline profile for the platform.

### Handshake negotiation rules

1. Client sends HELLO with its supported/preferred profiles, directional
   limits, auth token, and packet size.
2. Server computes `intersection = client_supported & server_supported`.
3. If intersection is empty, server sends HELLO_ACK with
   `transport_status = UNSUPPORTED` and closes.
4. If auth token does not match, server sends HELLO_ACK with
   `transport_status = AUTH_FAILED` and closes.
5. Server selects a profile deterministically:
   a. Compute `preferred_intersection = intersection &
      client_preferred & server_preferred`.
   b. If `preferred_intersection` is non-zero, select the highest set
      bit in it.
   c. Otherwise select the highest set bit in `intersection`.
   d. Higher bits represent faster/more capable profiles (SHM > baseline).
      Bit 0 is always the baseline fallback.
6. Server negotiates directional limits by taking the minimum of client
   and server values for each direction.
7. Server computes `agreed_packet_size = min(client_packet_size,
   server_packet_size)`.
8. Server sends HELLO_ACK with `transport_status = OK` and all
   negotiated values.
9. Both sides use the negotiated values for the remainder of the session.

### Default constants

| Constant | Value | Description |
|----------|-------|-------------|
| MAX_PAYLOAD_DEFAULT | 1024 | Default single-payload ceiling in bytes |

## Validation rules

Receivers must validate on every received message:

- `magic` must be `0x4e495043` (or `0x4e43484b` for chunk continuation)
- `version` must be `1`
- `header_len` must be `32`
- `kind` must be 1, 2, or 3
- `payload_len` must not exceed the negotiated directional limit
- `item_count` must not exceed the negotiated directional batch item limit
- For batch messages: the item directory must fit within `payload_len`,
  and all item offsets + lengths must fall within the packed item area
- For chunk continuations: `message_id`, `chunk_index`, and `chunk_count`
  must be consistent with the in-progress chunked receive

Any validation failure is a protocol violation and results in session
termination.

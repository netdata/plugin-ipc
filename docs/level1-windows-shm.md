# Level 1: Windows SHM Transport Contract

## Purpose

This document defines the shared memory region layout, synchronization
protocol, and lifecycle rules for the Windows SHM transport. All
implementations (C native, C MSYS, Rust native, Go native) must use
this exact layout and synchronization protocol to interoperate on the
same shared region.

## Kernel object names

The shared memory region and synchronization objects are identified by
kernel object names in the `Local\` namespace:

```
Local\netipc-{hash:016llx}-{service}-p{profile}-mapping
Local\netipc-{hash:016llx}-{service}-p{profile}-req_event
Local\netipc-{hash:016llx}-{service}-p{profile}-resp_event
```

Where:

- `hash` = FNV-1a 64-bit hash of `run_dir + "\n" + service_name +
  "\n" + auth_token`
- `service` = `service_name` (must contain only alphanumeric, dash,
  underscore, and dot; reject otherwise)
- `profile` = selected profile number

The auth token is included in the hash so that only peers sharing the
same token derive the same object names. This provides a basic
authorization barrier for SHM access.

### FNV-1a 64-bit hash

Same algorithm as the Named Pipe transport:

- Offset basis: `0xcbf29ce484222325`
- Prime: `0x00000100000001B3`
- Input is the concatenation: `run_dir + "\n" + service_name + "\n" +
  auth_token_as_decimal_string`

## Profile bits

| Bit | Value | Name | Synchronization |
|-----|-------|------|-----------------|
| 1 | `0x02` | SHM_HYBRID | Spin + kernel events |
| 2 | `0x04` | SHM_BUSYWAIT | Pure busy-spin, no kernel events |
| 3 | `0x08` | SHM_WAITADDR | Reserved (WaitOnAddress, not currently used cross-process) |

SHM_HYBRID is the general-purpose profile. SHM_BUSYWAIT is for
single-client low-latency scenarios where CPU usage is acceptable.

## Region layout

The shared memory region is created via `CreateFileMappingW` and
mapped via `MapViewOfFile`. The region has three sections:

```
[Header: 128 bytes, offset 0]
[Request area: request_capacity bytes, 64-byte aligned]
[Response area: response_capacity bytes, 64-byte aligned]
```

All data offsets are aligned to 64-byte cache-line boundaries.

## Region header (128 bytes)

| Offset | Size | Type | Field | Volatile | Description |
|--------|------|------|-------|----------|-------------|
| 0 | 4 | u32 | magic | no | Must be `0x4e535748` ("NSWH") |
| 4 | 4 | u32 | version | no | Must be `3` |
| 8 | 4 | u32 | header_len | no | Must be `128` |
| 12 | 4 | u32 | profile | no | Selected profile (2, 4, or 8) |
| 16 | 4 | u32 | request_offset | no | Byte offset to request area |
| 20 | 4 | u32 | request_capacity | no | Request area size in bytes |
| 24 | 4 | u32 | response_offset | no | Byte offset to response area |
| 28 | 4 | u32 | response_capacity | no | Response area size in bytes |
| 32 | 4 | u32 | spin_tries | no | Spin iterations before kernel wait |
| 36 | 4 | LONG | req_len | yes | Current request message length |
| 40 | 4 | LONG | resp_len | yes | Current response message length |
| 44 | 4 | LONG | req_client_closed | yes | Client-side close flag |
| 48 | 4 | LONG | req_server_waiting | yes | Server waiting for request flag |
| 52 | 4 | LONG | resp_server_closed | yes | Server-side close flag |
| 56 | 4 | LONG | resp_client_waiting | yes | Client waiting for response flag |
| 60 | 4 | - | padding | no | Reserved |
| 64 | 8 | LONG64 | req_seq | yes | Request sequence number |
| 72 | 8 | LONG64 | resp_seq | yes | Response sequence number |
| 80 | 48 | - | reserved | no | Reserved for future use |

Total: 128 bytes. Enforced by compile-time assertions including exact
offset checks for `spin_tries` (32), `req_len` (36), `resp_len` (40),
`req_client_closed` (44), `req_server_waiting` (48),
`resp_server_closed` (52), `resp_client_waiting` (56), `req_seq` (64),
and `resp_seq` (72).

### Constants

| Name | Value |
|------|-------|
| REGION_MAGIC | `0x4e535748` |
| REGION_VERSION | `3` |
| CACHELINE_SIZE | `64` bytes |
| SHM_HYBRID_DEFAULT_SPIN_TRIES | `1024` |
| BUSYWAIT_DEADLINE_POLL_MASK | `1023` |

The Windows default spin count (1024) is higher than POSIX (128)
because Windows kernel synchronization primitives have higher overhead,
and SHM performance on Windows requires more spinning to avoid falling
into kernel wait paths.

## Publication protocol

### SHM_HYBRID (spin + kernel events)

Uses two named kernel events (`req_event`, `resp_event`) created via
`CreateEventW`, plus spin loops on sequence numbers.

#### Client sends a request

1. Write the complete message into the request area.
2. Store the message length in `req_len` (interlocked exchange).
3. Increment `req_seq` (interlocked increment) to publish.
4. If `req_server_waiting` is set, signal `req_event` via `SetEvent`.

#### Server receives a request

1. Spin up to `spin_tries` iterations checking `req_seq` (interlocked
   compare).
2. If not advanced: set `req_server_waiting = 1`, check `req_seq`
   once more (avoid race), then `WaitForSingleObject(req_event,
   timeout)`.
3. Clear `req_server_waiting` after waking.
4. Read `req_len` and the message bytes from the request area.

#### Server sends a response

1. Write the complete response into the response area.
2. Store the message length in `resp_len` (interlocked exchange).
3. Increment `resp_seq` (interlocked increment) to publish.
4. If `resp_client_waiting` is set, signal `resp_event` via `SetEvent`.

#### Client receives a response

1. Spin up to `spin_tries` iterations checking `resp_seq`.
2. If not advanced: set `resp_client_waiting = 1`, check `resp_seq`
   once more, then `WaitForSingleObject(resp_event, timeout)`.
3. Clear `resp_client_waiting` after waking.
4. Read `resp_len` and the message bytes from the response area.

### SHM_BUSYWAIT (pure spin, no kernel events)

No kernel event objects are created. Both sides spin continuously on
sequence numbers.

- The publication protocol is the same as SHM_HYBRID except:
  - No `SetEvent` calls.
  - No `WaitForSingleObject` calls.
  - The waiter spins indefinitely (with deadline polling every
    `BUSYWAIT_DEADLINE_POLL_MASK + 1` iterations to check for timeout
    expiry).

This profile burns full CPU on both client and server but achieves the
lowest possible latency.

## Close protocol

### Client closes

1. Set `req_client_closed = 1` (interlocked exchange).
2. If SHM_HYBRID: signal `req_event` to wake a waiting server.
3. Unmap the region and close handles.

### Server closes

1. Set `resp_server_closed = 1` (interlocked exchange).
2. If SHM_HYBRID: signal `resp_event` to wake a waiting client.
3. Unmap the region, close the file mapping handle.
4. Close event handles if SHM_HYBRID.

### Detecting peer close

- The server checks `req_client_closed` after waking from a wait or
  spin timeout. If set, the server treats the session as closed.
- The client checks `resp_server_closed` similarly.
- On peer close detection, the detecting side must report the session
  as disconnected (graceful close, equivalent to `io.EOF`).

## Memory ordering

- Sequence number and length stores use interlocked (atomic) operations
  with release semantics.
- Sequence number and length loads use interlocked (atomic) operations
  with acquire semantics.
- The sequence increment acts as the publication fence, same as POSIX.
- The `volatile` qualifier on flag fields ensures compiler ordering but
  is not sufficient for cross-processor ordering — interlocked
  operations provide that.

## Single in-flight constraint

Same as POSIX SHM: one in-flight message per direction. The client must
wait for the response before sending the next request. Batch-level
pipelining (multiple items in one message) is the mechanism for higher
throughput.

## Region lifecycle

### Server creates the region

1. Create the file mapping via `CreateFileMappingW` using the derived
   mapping name.
2. Map the view via `MapViewOfFile`.
3. Write the header: magic, version, header_len, profile, offsets,
   capacities, spin_tries. Initialize all volatile fields to zero.
4. If SHM_HYBRID: create `req_event` and `resp_event` as auto-reset
   kernel events (`CreateEventW` with `bManualReset = FALSE`).
   Auto-reset is required: `SetEvent` wakes exactly one waiter and
   resets automatically, which matches the one-writer/one-reader-per-
   direction model.
5. The region is now ready for clients.

### Client attaches to the region

1. Open the file mapping via `OpenFileMappingW` using the derived
   mapping name.
2. Map the view via `MapViewOfFile`.
3. Validate the header: magic, version, header_len, profile.
4. Read offsets, capacities, and spin_tries from the header.
5. If SHM_HYBRID: open `req_event` and `resp_event` kernel events.
6. The client is now ready to publish requests and consume responses.

### Server destroys the region

1. Unmap the view.
2. Close the file mapping handle.
3. Close event handles if SHM_HYBRID.
4. Kernel object cleanup is automatic when all handles are closed.

### Client detaches

1. Unmap the view.
2. Close the file mapping handle.
3. Close event handles if SHM_HYBRID.

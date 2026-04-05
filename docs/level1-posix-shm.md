# Level 1: POSIX SHM Transport Contract

## Purpose

This document defines the shared memory region layout, synchronization
protocol, and lifecycle rules for the POSIX SHM transport on Linux.
All implementations (C, Rust, Go) must use this exact layout and
synchronization protocol to interoperate on the same shared region.

This transport is Linux-only. FreeBSD and macOS fall back to UDS
baseline.

## Region file path

```
{run_dir}/{service_name}-{session_id:016x}.ipcshm
```

Where `session_id` is the server-assigned session identifier from the
hello-ack payload, formatted as a zero-padded 16-character lowercase
hex string.

Each session gets its own SHM region. Multiple concurrent clients each
have independent regions with independent request/response areas,
sequence numbers, and futex signal words.

Created by the server via `open` + `ftruncate` on a filesystem path.
The client opens the same path after the handshake negotiates an SHM
profile, using the `session_id` received in the hello-ack.

## Profile bits

| Bit | Value | Name | Synchronization |
|-----|-------|------|-----------------|
| 1 | `0x02` | SHM_HYBRID | Spin + futex |
| 2 | `0x04` | SHM_FUTEX | Futex only (reserved) |

SHM_HYBRID is the current active profile. SHM_FUTEX is reserved for
potential future use.

## Region layout

The shared memory region is a contiguous mapped area with three sections:

```
[Header: 64 bytes, offset 0]
[Request area: request_capacity bytes, 64-byte aligned]
[Response area: response_capacity bytes, 64-byte aligned]
```

All offsets within the region are 64-byte aligned
(`NETIPC_SHM_REGION_ALIGNMENT = 64`).

## Region header (64 bytes)

| Offset | Size | Type | Field | Atomic | Description |
|--------|------|------|-------|--------|-------------|
| 0 | 4 | u32 | magic | no | Must be `0x4e53484d` ("NSHM") |
| 4 | 2 | u16 | version | no | Must be `3` |
| 6 | 2 | u16 | header_len | no | Must be `64` |
| 8 | 4 | i32 | owner_pid | no | PID of server process |
| 12 | 4 | u32 | owner_generation | no | Generation counter for PID reuse detection |
| 16 | 4 | u32 | request_offset | no | Byte offset from region start to request area |
| 20 | 4 | u32 | request_capacity | no | Size of request area in bytes |
| 24 | 4 | u32 | response_offset | no | Byte offset from region start to response area |
| 28 | 4 | u32 | response_capacity | no | Size of response area in bytes |
| 32 | 8 | u64 | req_seq | yes | Request sequence number |
| 40 | 8 | u64 | resp_seq | yes | Response sequence number |
| 48 | 4 | u32 | req_len | yes | Current request message length |
| 52 | 4 | u32 | resp_len | yes | Current response message length |
| 56 | 4 | u32 | req_signal | yes | Request futex word |
| 60 | 4 | u32 | resp_signal | yes | Response futex word |

Total: 64 bytes. Enforced by compile-time assertion.

### Constants

| Name | Value |
|------|-------|
| REGION_MAGIC | `0x4e53484d` |
| REGION_VERSION | `3` |
| REGION_ALIGNMENT | `64` bytes |
| DEFAULT_SPIN_TRIES | `128` |

## Capacity derivation

Request and response area capacities are derived from the negotiated
directional limits:

- `request_capacity` = maximum request message size (header + max
  request payload including batch overhead)
- `response_capacity` = maximum response message size (header + max
  response payload including batch overhead)

Both are rounded up to the region alignment boundary.

These capacities are fixed for the lifetime of the current SHM session.
Level 1 does not resize a mapped region in place. If higher layers later
reconnect with larger learned limits, the new session gets a new
`session_id`, a new SHM file, and capacities derived from that new
handshake.

## Publication protocol

SHM uses a publish/consume model with one in-flight message per
direction. Each direction has its own sequence number, length, and
signal word.

### Client sends a request

1. Write the complete message (outer header + payload) into the request
   area starting at `request_offset`.
2. Store the message length in `req_len` (atomic release).
3. Increment `req_seq` (atomic release) to publish the request.
4. Wake the server by writing to `req_signal` and calling
   `futex(FUTEX_WAKE)` on it.

### Server receives a request

1. Spin up to `DEFAULT_SPIN_TRIES` iterations checking if `req_seq`
   has advanced (atomic acquire).
2. If the sequence has not advanced after spinning, block on
   `futex(FUTEX_WAIT)` on `req_signal` with a timeout.
3. Once `req_seq` has advanced, read `req_len` (atomic acquire).
4. If `req_len` is 0, report a protocol error — `send` rejects
   zero-length messages, so this indicates SHM corruption.
5. Validate `req_len` against `request_capacity`. If `req_len` exceeds
   the capacity, discard the message and report an error. This prevents
   out-of-bounds reads from a malicious or buggy peer.
6. Read the message bytes from the request area.

### Server sends a response

1. Write the complete response message into the response area starting
   at `response_offset`.
2. Store the message length in `resp_len` (atomic release).
3. Increment `resp_seq` (atomic release) to publish the response.
4. Wake the client by writing to `resp_signal` and calling
   `futex(FUTEX_WAKE)` on it.

### Client receives a response

1. Spin up to `DEFAULT_SPIN_TRIES` iterations checking if `resp_seq`
   has advanced (atomic acquire).
2. If the sequence has not advanced after spinning, block on
   `futex(FUTEX_WAIT)` on `resp_signal` with a timeout.
3. Once `resp_seq` has advanced, read `resp_len` (atomic acquire).
4. If `resp_len` is 0, report a protocol error (SHM corruption).
5. Validate `resp_len` against `response_capacity`. If `resp_len`
   exceeds the capacity, discard the message and report an error.
5. Read the message bytes from the response area.

## Memory ordering

- All sequence number and length stores use **release** ordering.
- All sequence number and length loads use **acquire** ordering.
- The sequence number increment acts as the publication fence: all
  payload bytes must be visible before the sequence advances.
- The reader must observe the sequence advance before reading payload
  bytes.

## Spin and wait

The hybrid synchronization model spins first, then falls back to
kernel-assisted blocking:

1. **Spin phase**: check the sequence number in a tight loop for up to
   `spin_tries` iterations. Each iteration should include a CPU pause
   hint (`PAUSE` on x86, equivalent on other architectures) to avoid
   starving the peer.
2. **Wait phase**: if spinning did not observe a sequence advance,
   block on `futex(FUTEX_WAIT, &signal_word, expected_value, timeout)`.
3. The publisher always calls `futex(FUTEX_WAKE)` after advancing the
   sequence, regardless of whether the consumer is spinning or waiting.

The spin count is a performance tuning parameter. The default of 128
balances throughput against CPU usage on production VMs. Higher values
increase maximum throughput but also increase CPU consumption at low
request rates.

## Single in-flight constraint

The current SHM layout supports exactly one in-flight message per
direction. The client must wait for the response before sending the
next request. Pipelining on SHM is achieved at the batch level: pack
multiple items into one batch message, send it as one publication,
receive one batch response.

This is an implementation constraint of the current layout, not a
protocol prohibition.

## Region lifecycle

### Server creates a per-session region

The server creates one SHM region per accepted session, after the
handshake negotiates an SHM profile. The region path is derived from
the `session_id` assigned during the handshake.

1. Derive the region path: `{run_dir}/{service_name}-{session_id:016x}.ipcshm`.
2. Create the file via `open(O_RDWR | O_CREAT | O_EXCL, 0600)`.
   `O_EXCL` ensures no collision with an existing region.
3. `ftruncate` to the required size (header + request area + response
   area).
4. `mmap` the region with `MAP_SHARED`.
5. Write the header: magic, version, header_len, owner_pid,
   owner_generation, offsets, capacities. Initialize all atomic fields
   to zero.
6. The region is now ready for the client.

The server must track all active per-session SHM regions so they can
be cleaned up on session close and server shutdown.

If a later reconnect negotiates larger capacities, the server creates a
new SHM file for the new session instead of resizing the old file in
place.

### Client attaches to the region

1. Derive the region path using the `session_id` from the hello-ack.
2. Open the `.ipcshm` file.
3. Validate the file size (must be >= header_len).
4. `mmap` the region.
5. Validate the header: magic, version, header_len.
6. Read offsets and capacities from the header.
7. The client is now ready to publish requests and consume responses.

If the file does not exist or is undersized (server has not yet
finished creating it), the client treats this as a retryable
protocol-not-ready condition.

### Server destroys a per-session region

When a session closes (graceful or broken):

1. `munmap` the region.
2. `unlink` the per-session `.ipcshm` file.

### Client detaches from the region

1. `munmap` the region.
2. Close the file descriptor.

### Stale region detection

Both `server_create` (per-session) and `cleanup_stale` (startup scan)
use the same stale detection logic:

1. Open the file and mmap the header. If `open()` fails with a
   permission error (EACCES, EPERM), the file is left in place — it
   may belong to another user or process.
2. Validate magic. If invalid or file is undersized: stale — unlink.
3. Check `owner_pid` and `owner_generation`:
   - If `owner_pid` is alive AND `owner_generation` is non-zero:
     the region is live — leave it.
   - Otherwise (PID dead, or generation is zero indicating an
     uninitialized/legacy region): stale — unlink.

The `owner_generation` check catches PID reuse: a new process that
reuses an old PID will not have the same generation value. A zero
generation indicates an uninitialized or legacy region that should
be reclaimed regardless of PID liveness.

### Stale region cleanup on server startup

When a server starts, it scans for stale per-session SHM files left
behind by a previous server instance that crashed or was killed:

1. Scan `{run_dir}` for files matching `{service_name}-*.ipcshm`.
2. For each file: apply the stale detection logic above.
3. Stale files are unlinked. Live files are left in place.

This cleanup runs once at server startup, before the listener begins
accepting connections. It is the safety net for crashes and hard
reboots.

### Stale recovery on per-session create

When `server_create` is called for a new session, it applies the same
stale detection logic to the target path before attempting `O_EXCL`
create. If a stale file exists, it is unlinked first. If a live file
exists, the create fails with address-in-use.

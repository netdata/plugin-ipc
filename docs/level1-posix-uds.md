# Level 1: POSIX UDS SEQPACKET Transport Contract

## Purpose

This document defines the interoperability contract for the POSIX
UDS SEQPACKET baseline transport. All implementations (C, Rust, Go)
must follow this contract to interoperate on the same socket.

## Transport type

`AF_UNIX` / `SOCK_SEQPACKET` — message-oriented, reliable, ordered,
connection-based Unix domain sockets with preserved message boundaries.

## Socket path derivation

```
{run_dir}/{service_name}.sock
```

Example: `/run/netdata/cgroups-snapshot.sock`

- `run_dir` is caller-supplied (typically `/run/netdata` on Linux)
- `service_name` is the service identity string
- The path must not exceed the platform's `sun_path` limit

## Profile bit

UDS SEQPACKET is profile bit 0 (`0x01`), named `UDS_SEQPACKET`.
This is the baseline profile — always supported, always available as
fallback.

## Connection lifecycle

1. **Server**: creates and binds the socket, listens for connections.
   If a stale socket file exists from a dead process, the server
   unlinks it and recreates. If the socket is actively held by a live
   process, the server fails with address-in-use.
   If two servers start concurrently for the same service name, one may
   lose the probe/bind race and fail with address-in-use. The contract is
   one owner per service endpoint.

2. **Client**: connects to the socket path. The OS delivers a connected
   SEQPACKET file descriptor.

3. **Handshake**: immediately after connection, the client sends a
   HELLO control message and the server responds with HELLO_ACK, as
   defined in the wire envelope spec. The handshake determines the
   session profile, directional limits, packet size, and `session_id`.

   The negotiated capacities are fixed for that session. If higher
   layers later reconnect with larger learned capacities, the next
   session gets a new `session_id` and therefore a new per-session SHM
   file.

4. **Data plane**: if the negotiated profile is UDS_SEQPACKET (bit 0),
   all subsequent messages travel over the same SEQPACKET connection.
   If a higher profile is negotiated (e.g., SHM), the data plane
   still uses the same UDS connection for control/lifecycle coordination,
   and successful HELLO_ACK guarantees the SHM resources for that session
   are already prepared on the server side. If the client later cannot
   attach SHM locally, it must close that session and reconnect without SHM.
   There is no same-session fallback.

## Packet size

SEQPACKET has a kernel-imposed maximum message size. The transport
determines its packet size from the socket's `SO_SNDBUF` option.

The effective packet size for the connection is negotiated during
handshake:

```
agreed_packet_size = min(client_packet_size, server_packet_size)
```

Both sides use `agreed_packet_size` for chunking decisions.

## Chunking

If a complete message (32-byte header + payload) exceeds
`agreed_packet_size`, the sender chunks it:

- First packet: the original 32-byte outer header + as many payload
  bytes as fit (up to `agreed_packet_size - 32` payload bytes).
- Continuation packets: 32-byte chunk continuation header + payload
  bytes (up to `agreed_packet_size - 32` payload bytes each).
- Each packet is sent as one SEQPACKET message (preserved boundary).
- The receiver reads one SEQPACKET message per chunk.

If the complete message fits in one packet, no chunking occurs and no
chunk header is used.

See the wire envelope spec for chunk header layout and validation rules.

## Receive timeout and abort

The baseline receive primitive remains a blocking Level 1 operation for
callers that want raw transport semantics. Level 1 also provides a
timeout-aware receive form used by Level 2 typed calls.

The timeout-aware receive form must:

- wait for each required SEQPACKET chunk with a deadline derived from the
  caller-provided timeout
- apply the same deadline to the first packet and all continuation
  packets that make up one logical message
- poll an explicit abort file descriptor at the same time as the socket
  file descriptor
- return a distinct timeout error when the deadline expires
- return a distinct aborted error when the abort descriptor is signaled

Level 2 is responsible for deciding whether a timeout or abort should
break the session. Level 1 only reports the condition precisely.

## SHM file path derivation

When the handshake negotiates a SHM profile, the server creates a
per-session shared memory region file:

```
{run_dir}/{service_name}-{session_id:016x}.ipcshm
```

Where `session_id` is the server-assigned session identifier from the
hello-ack, formatted as a zero-padded 16-character lowercase hex string.
Each session gets its own SHM region. See the POSIX SHM transport
contract for full region layout and lifecycle details.

The UDS connection remains open for the session lifetime (it carried
the handshake and is used for SHM lifecycle coordination). For SHM
profiles, successful HELLO_ACK means the shared memory region has already
been created for that session. If the client still cannot attach it, the
client must close that session and reconnect baseline-only. If a later reconnect learns
larger capacities, it establishes a new session with a new `session_id`
and therefore a different SHM file path.

## Stale endpoint recovery

### Socket file

- Server checks if the socket path exists before bind.
- If it exists: attempt to connect to it. If connection succeeds, a
  live server owns it — fail with address-in-use. If connection fails
  with ENOENT, nothing is there. Any other connect failure
  (ECONNREFUSED, EACCES, a foreign file squatting on the socket path,
  etc.) means the endpoint is not a live server — it is silently
  unlinked and recreated.
- The only exception is fd exhaustion (EMFILE/ENFILE) while creating
  the probe socket: liveness cannot be evaluated, so the endpoint is
  kept and the server fails with address-in-use rather than risk
  deleting a live socket.

### SHM file

- Server checks the `.ipcshm` file for ownership metadata (PID and
  generation) stored in the SHM region header.
- If the owner PID is alive and the generation matches, the region is
  active — fail with address-in-use.
- If the owner PID is dead or the region is invalid/undersized/unreadable,
  the region is stale or junk — silently unlink and recreate. Only fd
  exhaustion (EMFILE/ENFILE) keeps the file in place, because liveness
  could not be evaluated.
- Clients that open an undersized/unpopulated SHM file (server has
  created it but not yet initialized the header) treat it as a
  retryable protocol-not-ready condition.

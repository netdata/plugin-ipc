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

2. **Client**: connects to the socket path. The OS delivers a connected
   SEQPACKET file descriptor.

3. **Handshake**: immediately after connection, the client sends a
   HELLO control message and the server responds with HELLO_ACK, as
   defined in the wire envelope spec. The handshake determines the
   session profile, directional limits, packet size, and `session_id`.

   Directional limits are not symmetric:
   - request limits are sender-driven
   - response limits are server-driven

   The negotiated capacities are fixed for that session. If higher
   layers later reconnect with larger learned capacities, the next
   session gets a new `session_id` and therefore a new per-session SHM
   file.

4. **Data plane**: if the negotiated profile is UDS_SEQPACKET (bit 0),
   all subsequent messages travel over the same SEQPACKET connection.
   If a higher profile is negotiated (e.g., SHM), the data plane
   switches after the handshake — see the SHM transport contracts.

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
the handshake and is used for SHM lifecycle coordination). The data
plane switches to the shared memory region. If a later reconnect learns
larger capacities, it establishes a new session with a new `session_id`
and therefore a different SHM file path.

## Stale endpoint recovery

### Socket file

- Server checks if the socket path exists before bind.
- If it exists: attempt to connect to it. If connection succeeds, a
  live server owns it — fail with address-in-use. If connection fails
  (refused/no listener), the socket is stale — unlink and recreate.

### SHM file

- Server checks the `.ipcshm` file for ownership metadata (PID and
  generation) stored in the SHM region header.
- If the owner PID is alive and the generation matches, the region is
  active — fail with address-in-use.
- If the owner PID is dead or the region is invalid/undersized, the
  region is stale — unlink and recreate.
- Clients that open an undersized/unpopulated SHM file (server has
  created it but not yet initialized the header) treat it as a
  retryable protocol-not-ready condition.

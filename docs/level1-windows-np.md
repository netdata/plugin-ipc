# Level 1: Windows Named Pipe Transport Contract

## Purpose

This document defines the interoperability contract for the Windows
Named Pipe baseline transport. All implementations (C native, C MSYS,
Rust native, Go native) must follow this contract to interoperate on
the same pipe.

## Transport type

Win32 Named Pipe in message mode (`PIPE_TYPE_MESSAGE` /
`PIPE_READMODE_MESSAGE`).

## Pipe name derivation

```
\\.\pipe\netipc-{hash:016llx}-{service}
```

Where:

- `hash` = FNV-1a 64-bit hash of `run_dir` (the namespace path)
- `service` = `service_name`, which must contain only alphanumeric
  characters, dash, underscore, and dot. Names containing any other
  character must be rejected at initialization (not silently sanitized).
- The hash is formatted as a zero-padded 16-character lowercase hex string

Example: `\\.\pipe\netipc-a1b2c3d4e5f67890-cgroups-snapshot`

### FNV-1a 64-bit hash

- Offset basis: `0xcbf29ce484222325`
- Prime: `0x00000100000001B3`
- For each byte in the input: `hash = (hash ^ byte) * prime`

All implementations must use the same FNV-1a algorithm to derive
identical pipe names from the same inputs.

## Profile bit

Named Pipe is profile bit 0 (`0x01`), named `NAMED_PIPE`.
This is the Windows baseline profile.

## Connection lifecycle

1. **Server**: creates a Named Pipe instance with `CreateNamedPipeW`.
   The pipe is created in message mode with overlapped I/O capability.
   The server calls `ConnectNamedPipe` to wait for a client connection.

2. **Client**: connects to the pipe with `CreateFileW`. The connection
   is established when the server's `ConnectNamedPipe` completes.

3. **Handshake**: immediately after connection, the client sends a
   HELLO control message and the server responds with HELLO_ACK, as
   defined in the wire envelope spec. Handshake messages travel over
   the Named Pipe.

   The handshake determines the session profile, directional limits,
   packet size, and `session_id`.

   The negotiated capacities are fixed for that session. If higher
   layers later reconnect with larger learned capacities, the next
   session gets a new `session_id` and therefore a new per-session SHM
   object set.

4. **Data plane**: if the negotiated profile is NAMED_PIPE (bit 0),
   all subsequent messages travel over the same pipe. If a higher
   profile is negotiated (e.g., SHM_HYBRID), the Named Pipe remains open
   for control/lifecycle coordination, and successful HELLO_ACK guarantees
   the SHM resources for that session are already prepared on the server
   side. If the client later cannot attach Win SHM locally, it must close
   that session and reconnect without SHM. There is no same-session fallback.

## Packet size

Named Pipes in message mode preserve message boundaries. The logical
packet size is negotiated during handshake as with POSIX UDS.

The pipe's buffer sizes are set at creation time to accommodate the
maximum negotiated message size.

## Chunking

Chunking rules are identical to the POSIX UDS transport. If a complete
message exceeds `agreed_packet_size`, it is split into chunks. Each
chunk is sent as a separate pipe write. The receiver reads chunks
sequentially.

See the wire envelope spec for chunk header layout and validation.

## SHM mapping name derivation

When the handshake negotiates a SHM profile, the shared memory region
and synchronization objects use kernel object names:

```
Local\netipc-{hash:016llx}-{service}-p{profile}-s{session_id:016llx}-mapping
Local\netipc-{hash:016llx}-{service}-p{profile}-s{session_id:016llx}-req_event
Local\netipc-{hash:016llx}-{service}-p{profile}-s{session_id:016llx}-resp_event
```

Where:

- `hash` = FNV-1a 64-bit hash of `run_dir + "\n" + service_name +
  "\n" + auth_token` (the auth token is included in the hash to
  prevent unauthorized access to the shared region)
- `service` = `service_name` (same character rules as the pipe name:
  alphanumeric, dash, underscore, dot only; reject otherwise)
- `profile` = the selected profile number (1, 2, 4, or 8)
- `session_id` = the server-assigned session identifier from HELLO_ACK

The Named Pipe connection remains open for the session lifetime. The
data plane for SHM profiles uses the shared memory region that was
already created before successful HELLO_ACK. If the client still cannot
attach it, the client must close that session and reconnect baseline-only.
If a later reconnect
learns larger capacities, it establishes a new session with a new
`session_id` and therefore a different SHM mapping / event name set.

## Disconnect detection

The following Win32 errors indicate peer disconnect and must be treated
as graceful connection close (not protocol violations):

- `ERROR_BROKEN_PIPE`
- `ERROR_NO_DATA`
- `ERROR_PIPE_NOT_CONNECTED`

## Multi-client support

Named Pipe servers support multiple concurrent clients by creating
additional pipe instances. Each `ConnectNamedPipe` completion yields
an independent session. The server must create a new pipe instance
before or after accepting each client to remain available for the
next connection.

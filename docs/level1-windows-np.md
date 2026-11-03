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

## Access policy

NetIPC services are local, and server and clients run as the same account
(Netdata runs its agent and plugins as LocalSystem).

- **Pipe DACL**: every pipe instance is created with an explicit, protected
  security descriptor, `D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;<server-SID>)`,
  where `<server-SID>` is the user SID of the server process token. Only
  SYSTEM, Administrators and the server's own account can open the pipe;
  Everyone and Anonymous get no access, not even read-only. A client running
  as another non-admin account fails to open the pipe with
  `ERROR_ACCESS_DENIED`, which the client reports as a connect failure.
- **Remote clients**: pipes are created with `PIPE_REJECT_REMOTE_CLIENTS`,
  so opens through SMB (`\\host\pipe\...`, including `\\127.0.0.1`) are
  refused.
- **Client impersonation level**: clients open the pipe with
  `SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION`. A server can identify
  the client's account but never act as it.
- **Server identity**: whoever creates a pipe name first owns it. Right after
  opening the pipe, before HELLO, the client reads the pipe owner
  (`GetSecurityInfo`, `OWNER_SECURITY_INFORMATION`) and continues only when
  it is the client's own account, SYSTEM (`S-1-5-18`) or Administrators
  (`S-1-5-32-544`). A non-admin account cannot assign any of those as owner,
  so a pipe it squats is refused and receives neither HELLO nor the auth
  token. The refusal is a distinct L1 error (C
  `NIPC_NP_ERR_UNTRUSTED_SERVER`, Rust `NpError::UntrustedServer`, Go
  `ErrUntrustedServer`); Level 2 clients report it as `AUTH_FAILED`.

An elevated administrator creates objects owned by Administrators; a
non-elevated user or a service account creates objects owned by its own SID.
Both pass the client check when the client runs as the same account.

## Connection lifecycle

1. **Server**: creates a Named Pipe instance with `CreateNamedPipeW`.
   The pipe is created in message mode with overlapped I/O capability.
   The server calls `ConnectNamedPipe` to wait for a client connection.

2. **Client**: connects to the pipe with `CreateFileW`. The connection
   is established when the server's `ConnectNamedPipe` completes.

   Between accepting one client and creating the next instance a server
   has no free instance, and `CreateFileW` fails with `ERROR_PIPE_BUSY`.
   The client then waits with `WaitNamedPipeW`, for at most 1000 ms
   (C `NIPC_NP_CONNECT_BUSY_WAIT_MS`, Rust `CONNECT_BUSY_WAIT_MS`, Go
   `ConnectBusyWaitMs`), and tries `CreateFileW` once more. Any other
   failure, or a second failure, fails the connect.

   The bounded connect (C `nipc_np_connect_timeout()`, Rust
   `NpSession::connect_with_timeout()`, Go `ConnectTimeout()`) takes a
   timeout (0 = none) and an abort signal (C an event handle, Rust an
   `AtomicBool`, Go a channel). The busy wait then also ends at that
   deadline, in `WaitNamedPipeW` slices of at most 100 ms so an abort ends
   it promptly, and the wait for HELLO_ACK ends at the same deadline. Expiry
   returns a timeout error, an abort an aborted error. `connect()` is the
   bounded connect without a deadline or abort. A pipe that disconnected
   while the client waits for HELLO_ACK is still read, because a server
   that rejects the client writes its reply and then closes.

   On the server, accept is split: `nipc_np_accept_pending()` (C),
   `NpListener::accept_pending()` (Rust) and `Listener.AcceptPending()`
   (Go) return the connected pipe instance without reading;
   `nipc_np_accept_handshake()`, `PendingPipe::handshake()` and
   `PendingConn.Handshake()` then wait for HELLO with a timeout (0 = none)
   and, in Rust and Go, an abort signal. Expiry returns a timeout error,
   an abort an aborted error, in all three languages. A failed handshake
   closes the instance without `DisconnectNamedPipe`, which would discard a rejection
   reply the client has not read yet. `nipc_np_accept_handshake_prepared()`,
   `PendingPipe::handshake_prepared()` and `PendingConn.HandshakePrepared()`
   also take a prepare callback with the selected profile and the final
   payload limits, called after the HELLO checks and before HELLO_ACK; on
   false the server drops that profile and selects again, rejecting with
   UNSUPPORTED when none is left. Level 2 servers create the session's SHM
   objects for the selected profile only in this callback.
   `nipc_np_accept()` / `accept()` / `Accept()` do both
   steps with the 5000 ms server handshake deadline. A server wakes a
   blocked `ConnectNamedPipe` by connecting to its own pipe name and
   closing at once (Rust
   `NpListenerWaker`, Go `Listener.Wake()`); it never closes a handle the
   accepting thread may still use.

   Closing a session only closes its handle, in both roles. The peer still
   reads every message written before the close, then sees
   `ERROR_BROKEN_PIPE`. Sessions do not call `FlushFileBuffers` or
   `DisconnectNamedPipe` on close: a flush blocks while the peer is not
   reading, and on a synchronous pipe handle neither `CancelSynchronousIo`
   nor `DisconnectNamedPipe` from another thread releases it.

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

## Pipelining

Pipes are synchronous (no `FILE_FLAG_OVERLAPPED`) in all three languages,
with each direction's buffer set to the larger of the packet size and
64 KiB. A send is a blocking `WriteFile` per packet that cannot stop to
read, so the POSIX "Pipelined sends" progress (`level1-posix-uds.md`) is
not implemented here yet.

Until it is, a pipelining caller must keep the request and response bytes
of its unread messages within the pipe buffer: read responses before a
send that would exceed it. Otherwise, once the responses it is not reading
fill the pipe, the server blocks sending, stops reading requests, and both
sides block. A caller with one request in flight (every typed call) is not
affected.

## Receive timeout and abort

The baseline receive primitive remains a blocking Level 1 operation for
callers that want raw transport semantics. Level 1 also provides a
timeout-aware receive form used by Level 2 typed calls.

The timeout-aware receive form must:

- wait for each required pipe message with a deadline derived from the
  caller-provided timeout
- apply the same deadline to the first packet and all continuation
  packets that make up one logical message
- observe the caller's abort signal while waiting for pipe readability,
  either through an abort event or by waiting in slices of at most 100 ms
  and checking an abort flag between slices; an abort is observed within
  100 ms
- treat the end of an abort-check slice as "keep waiting", never as the
  deadline
- return a distinct timeout error when the deadline expires
- return a distinct aborted error when the abort is observed

Level 2 is responsible for deciding whether a timeout or abort should
break the session. Level 1 reports timeout and abort distinctly; peer
disconnect is reported when the pipe read path observes the corresponding
Win32 pipe error.

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

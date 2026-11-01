# Level 1: POSIX UDS SEQPACKET Transport Contract

## Purpose

This document defines the interoperability contract for the POSIX
UDS SEQPACKET baseline transport. All implementations (C, Rust, Go)
must follow this contract to interoperate on the same socket.

## Transport type

`AF_UNIX` / `SOCK_SEQPACKET` — message-oriented, reliable, ordered,
connection-based Unix domain sockets with preserved message boundaries.

macOS has no `AF_UNIX` `SOCK_SEQPACKET`. There the baseline runs over
`SOCK_STREAM` with the same bytes on the wire; see "macOS stream baseline".

Every socket and pipe the transport creates (listener, accepted and
connected sessions, the client abort pipe, SHM region descriptors) is
close-on-exec (`SOCK_CLOEXEC`, `accept4(..., SOCK_CLOEXEC)`, `O_CLOEXEC`,
`F_DUPFD_CLOEXEC`; platforms without them set `FD_CLOEXEC` right after
creation). A session descriptor inherited by a child that a consumer forks
and execs would keep a dead peer's socket open and hide the disconnect.

## Socket path derivation

```
{run_dir}/{service_name}.sock
```

Example: `/run/netdata/cgroups-snapshot.sock`

- `run_dir` is caller-supplied (typically `/run/netdata` on Linux)
- `service_name` is the service identity string
- The path must not exceed the platform's `sun_path` limit

## Socket permissions

The listener socket file is owner-only: mode `0600`, narrowed further by the
process umask. C, Rust and Go set it the same way: `fchmod(fd, 0600)` on the
unbound socket, then `bind()`. Linux and FreeBSD create the socket file with
the unbound socket's mode, so the file never exists with a wider mode, no
path-based `chmod` runs, and the process-global umask is never changed (other
threads of the embedding process may be creating files at the same time).

Consequences:

- clients must run as the server's user (or as a user the kernel lets
  bypass file permissions, such as root);
- `run_dir` must not be writable by untrusted users: whoever can write it
  can remove or replace the endpoint;
- a platform whose kernel rejects `fchmod` on an unbound socket fails
  `listen` with a socket error instead of creating a wider socket.

macOS rejects `fchmod` on an unbound socket and creates the socket file
with mode `0777 & ~umask`, so it binds in a private directory instead:

1. Under the endpoint lock, a leftover `{run_dir}/{service_name}.d` from a
   crashed server is removed: a directory holding only `s` is emptied and
   removed, any other non-directory at that name is unlinked. A directory
   with other content is never deleted; `listen` fails with a socket error.
2. `mkdir {service_name}.d` (mode 0700, set again with `chmod` so a umask
   without owner bits cannot make it unusable), `bind()` to
   `{service_name}.d/s`, then `chmod` the socket file to its created mode
   `& 0600`, which keeps the umask narrowing.
3. `rename()` it to `{service_name}.sock` and remove the directory.

No other user can reach the socket while its mode is wider, and the
process-global umask is never changed. The temporary path must also fit
`sun_path`.

## Profile bit

UDS SEQPACKET is profile bit 0 (`0x01`), named `UDS_SEQPACKET`.
This is the baseline profile — always supported, always available as
fallback.

## Connection lifecycle

1. **Server**: takes the endpoint lock (see "Stale endpoint recovery"),
   then creates and binds the socket and listens for connections.
   If a stale socket file exists from a dead process, the server
   unlinks it and recreates. If the socket is actively held by a live
   process, the server fails with address-in-use.
   If two servers start concurrently for the same service name, exactly
   one gets the lock; the other fails with address-in-use. The contract
   is one owner per service endpoint.

2. **Client**: connects to the socket path. The OS delivers a connected
   SEQPACKET file descriptor.

   The bounded connect (C `nipc_uds_connect_timeout()`, Rust
   `UdsSession::connect_with_timeout()`, Go `ConnectTimeout()`) takes a
   timeout (0 = none) and an abort signal (C a descriptor, Rust an
   `AtomicBool`, Go a channel). A blocking connect waits while the
   listener's backlog is full; on Linux the bounded connect waits in
   `SO_SNDTIMEO` slices of at most 100 ms, so an abort ends it promptly,
   and resets the send timeout once connected. Other systems refuse a full
   backlog at once. The wait for HELLO_ACK ends at the same deadline.
   Expiry returns a timeout error, an abort an aborted error. `connect()`
   is the bounded connect without a deadline or abort.

   On the server, accept is split: `nipc_uds_accept_pending()` (C),
   `UdsListener::accept_pending()` (Rust) and `Listener.AcceptPending()`
   (Go) take the connection without reading; `nipc_uds_accept_handshake()`,
   `PendingConnection::handshake()` and `PendingConn.Handshake()` then wait
   for HELLO with a timeout (0 = none) and, in Rust and Go, an abort signal.
   A failed handshake closes the connection. `nipc_uds_accept_handshake_prepared()`,
   `PendingConnection::handshake_prepared()` and `PendingConn.HandshakePrepared()`
   also take a prepare callback. The server calls it with the selected profile
   and the final request and response payload limits after the HELLO checks
   and before HELLO_ACK; on false it drops that profile and selects again from
   the rest of the intersection, rejecting with UNSUPPORTED when none is left.
   Level 2 servers create the session's SHM region in this callback.
   `nipc_uds_accept()` /
   `accept()` / `Accept()` do both steps with the 5000 ms server handshake
   deadline.

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

SEQPACKET has a kernel-imposed maximum message size. On Linux a datagram
larger than the sender's `SO_SNDBUF` value (as reported by
`getsockopt`) minus 32 bytes of kernel accounting overhead fails with
`EMSGSIZE`, and very large buffers also fail with `ENOBUFS` near 1 MiB. On
FreeBSD the largest datagram equals `SO_SNDBUF` exactly (no overhead; the
default is 8192 bytes, `net.local.seqpacket.maxseqpacket`).

Each side derives the packet size it proposes as:

```
limit    = min(SO_SNDBUF - overhead, 262144)   (65536 if SO_SNDBUF cannot be read)
proposal = configured packet size, if non-zero and below limit; else limit
```

`overhead` is 0 on FreeBSD and 32 elsewhere.

A configured packet size above the limit is clamped silently; it is a
preference, not a requirement.

The effective packet size for the connection is negotiated during
handshake:

```
agreed_packet_size = min(client_packet_size, server_packet_size)
```

The client must reject a HELLO_ACK whose `agreed_packet_size` exceeds its
own proposal (or is not greater than the 32-byte header) as a protocol
error: a larger size could exceed what the client's kernel accepts.

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
- observe the caller's abort signal while waiting, either by polling an
  abort file descriptor at the same time as the socket file descriptor or
  by waiting in slices of at most 100 ms and checking an abort flag
  between slices; an abort is observed within 100 ms
- treat the end of an abort-check slice as "keep waiting", never as the
  deadline
- return a distinct timeout error when the deadline expires
- return a distinct aborted error when the abort is observed

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

## macOS stream baseline

macOS has no `AF_UNIX` `SOCK_SEQPACKET`, so its baseline uses `SOCK_STREAM`.
The handshake, profile bit and packets are unchanged; only how a receiver
finds packet boundaries differs. C, Rust and Go implement it the same way.

- **Packets.** A packet is the 32-byte header plus the bytes it declares,
  so no framing bytes are added. After an outer header (`NIPC`) the body is
  `min(payload_len, packet_size - 32)` bytes, after a continuation header
  (`NCHK`) it is `chunk_payload_len` bytes. A handshake packet is read
  with a packet size of 0, which caps the body at the receive buffer.
  Bytes that do not fit the buffer are not read, as a SEQPACKET receive
  into a short buffer drops them; the length checks then fail.
- **First header first.** A session's first packet header is checked
  before its body is read. A header that is not an outer header, or that
  declares more payload or items than the negotiated limits, is passed to
  the framing alone, so `LIMIT_EXCEEDED` or a protocol error is reported
  without waiting for bytes the peer may never send.
- **End of stream.** EOF before a packet's first byte is a disconnect. EOF
  inside a packet is a protocol error, inside a continuation a chunk error.
  Bytes left after a message's last packet are read as the next packet.
- **Packet size.** A stream has no datagram limit: the proposal is the
  configured packet size, or 262144 when it is 0 or larger. `SO_SNDBUF` is
  not consulted. Messages larger than the agreed size are chunked as on
  SEQPACKET.
- **Writes.** A packet may take several `sendmsg()` calls; the sender
  resumes after a partial write. Header and body go out without copying.
  `SO_NOSIGPIPE` is set on every session socket, so a write to a closed
  peer fails with `EPIPE` instead of raising `SIGPIPE`, also in Go, whose
  darwin `syscall` package has no `MSG_NOSIGNAL`.
- **Partial receive.** A receive timeout or abort before the first byte of
  a message leaves the session usable. Any receive failure after it
  (timeout, abort, malformed or over-limit packet) leaves unread bytes of
  the message queued, so the receiving side is shut down
  (`shutdown(SHUT_RD)`) and every later receive fails. Sends stay valid, so
  a server can still answer a rejected request with `LIMIT_EXCEEDED`.
- **Socket buffers.** macOS's default `AF_UNIX` stream buffers are 8 KiB
  per direction, far below Linux's. A pipelining caller that sends many
  requests before reading responses blocks sooner: once the responses it is
  not reading fill its receive buffer, the server stops reading requests
  too. Read responses while sending, or keep the bytes in flight small.
- **Connect.** A full listen backlog refuses a connect at once
  (`ECONNREFUSED`), as on FreeBSD; Linux waits for room.
- **Not available:** SHM profiles. macOS servers and clients offer the
  baseline only.

## Interrupted system calls

`send()`, `sendmsg()` and `recv()` calls that fail with `EINTR` are
retried inside the transport. A SEQPACKET call moves a whole message or
nothing, so a retry can neither duplicate nor split a message. On the
macOS stream an interrupted call moved no bytes, and partial transfers are
resumed, so the same holds there. Signals
delivered to the process therefore never surface as send/receive
failures.

## Stale endpoint recovery

### Endpoint lock

- Before it touches the socket path, the server takes an exclusive,
  non-blocking `flock` on `{run_dir}/{service_name}.lock`. The file is
  created owner-only (0600) with `O_CLOEXEC | O_NOFOLLOW`.
- If another listener holds the lock, the server fails with
  address-in-use. This closes the window between one server's `bind()`
  and `listen()`, where a connect probe would see a refused socket and
  unlink a live endpoint.
- The listener holds the lock for its whole life and releases it after
  it unlinks the socket at close. The kernel releases it when the
  process dies. The lock file itself is never deleted: deleting it would
  let a server that already opened the old file and a server that
  creates a new one both hold "the" lock.
- All three implementations use the same lock file, so C, Rust and Go
  servers exclude each other.

### Socket file

- Server checks if the socket path exists before bind. The connect
  probe below stays as a guard against servers of older versions,
  which take no lock.
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

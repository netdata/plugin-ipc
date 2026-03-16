# Level 2: Typed Convenience API Specification

## Purpose

Level 2 is a pure convenience orchestration layer. It provides zero unique
functionality. Everything Level 2 does, a caller can do manually with
Level 1 primitives and Codec functions.

Level 2 exists so that the common patterns — blocking request/response,
client lifecycle management, managed multi-client servers — do not have
to be reimplemented by every integration.

## Scope

Level 2 owns:

- Blocking typed request/response call wrappers
- Client context lifecycle (initialize, refresh, ready, status, close)
- Connection state machine with automatic reconnect policy
- Managed server mode (acceptor, per-session request/response loops,
  concurrent session limit)
- Per-method typed dispatch helpers in the Codec layer

Level 2 does NOT own:

- Transport, framing, sequencing, pipelining, chunking (Level 1)
- Batch framing and item directory management (Level 1)
- Payload encoding/decoding (Codec)
- Response builder mechanics (Codec)
- Snapshot refresh, caching, or lookup strategy (Level 3)

## Dependency

Level 2 depends on:

- **Level 1**: for all transport operations (connect, listen, accept, send,
  receive, close, wait objects, batch assembly/extraction)
- **Codec**: for all payload encoding/decoding (encode request, decode
  response view, response builders)

Level 2 never touches wire bytes directly. It never manages transport
state directly. It calls Level 1 and Codec functions exclusively.

## Principles

### 1. Zero unique functionality

Every operation Level 2 performs is a composition of Level 1 and Codec
calls. There is no behavior in Level 2 that cannot be replicated by a
caller using Level 1 + Codec directly.

If a feature cannot be expressed as a composition of Level 1 + Codec, it
does not belong in Level 2 — it belongs in Level 1 or Codec.

### 2. Callers do not see transports

Level 2 clients and servers have no visibility into transport details.
They do not know whether the underlying connection is UDS, Named Pipe,
or SHM. They do not know whether their request was chunked. They do not
manage connections, sessions, or handshakes.

The entire transport layer is invisible. Level 2 callers interact only
with typed request/response structures and service identities.

### 3. Callbacks are strongly typed and single-item

Server callbacks receive one decoded typed request view and one response
builder. They produce one typed response. Period.

The typed handler does not know and does not care about transport
details, wire format, or session state. It receives decoded data and
returns a result. The Codec dispatch helper handles encoding and
decoding.

### 4. At-least-once call semantics

Level 2 client calls are intentionally at-least-once, not exactly-once.

If a call fails and the session was previously READY, Level 2 must
disconnect, reconnect (including a full handshake), and resend the
request once. This means the server may receive the same request twice.
Duplicate requests are acceptable by contract.

If the session was NOT previously READY, the call fails immediately
without attempting reconnection.

If the retry also fails, Level 2 reports failure to the caller.

### 5. No hidden background threads (client)

Level 2 clients do not spawn background threads for connection management.
Connection state transitions (connect, reconnect, disconnect) happen
inside explicit caller-driven operations: `refresh()` and typed call
methods.

The caller owns the timing of connection work by calling `refresh()`
from its own loop at whatever cadence it chooses.

## Client context

Level 2 provides one persistent client context per service. For example,
a plugin that needs IP-to-ASN enrichment creates one `ctx_ip_to_asn`
context at startup and uses it for the lifetime of the process.

### Lifecycle

- **initialize(service_namespace, service_name, config)**: creates the
  context. Does NOT connect. Does NOT require the server to be running.
  The config includes: auth token, supported/preferred profiles, directional
  limits. Returns the context object.

- **refresh(ctx)**: the caller calls this periodically from its own loop.
  This is where connection attempts and reconnections happen. Returns
  whether the state changed, so the caller can react if needed.
  No hidden threads. No automatic timers.

- **ready(ctx)**: returns a boolean. This is a cheap cached predicate —
  no syscalls, no I/O. Suitable for hot-path checks. Returns true only
  if the context is in the READY state.

- **close(ctx)**: tears down the context, closes the underlying Level 1
  session if connected, releases all resources.

### State model

The client context tracks its connection state with the following states:

- **DISCONNECTED**: no connection. `refresh()` will attempt to connect.
- **CONNECTING**: connection attempt in progress.
- **READY**: connected, handshake completed, calls can proceed.
- **NOT_FOUND**: the service endpoint does not exist.
- **AUTH_FAILED**: handshake auth verification failed.
- **INCOMPATIBLE**: handshake profile or limit negotiation failed.
- **BROKEN**: the connection was previously READY but has broken.

`ready(ctx)` returns true only for READY.

`status(ctx)` returns a detailed snapshot including the current state,
reconnect counts, and operational counters. This is for diagnostics
and logging, not for hot-path decisions.

### Typed single-item calls

Level 2 exposes per-method-type blocking call functions. Each call:

1. Encodes the typed request using the Codec
2. Sends it via Level 1 as a single-item message
3. Receives the response via Level 1
4. Checks outer `transport_status` — if not OK, reports failure
   without attempting to decode
5. Decodes the response payload using the Codec
6. Returns the decoded result directly to the caller

For simple types (INCREMENT), the call returns a scalar value (or via
out-parameter in C). For complex types (CGROUPS_SNAPSHOT), the call
returns an ephemeral view that borrows the response buffer and is valid
until the next call.

There are no callbacks on the client side. Every typed call is a
synchronous function that returns the decoded result:

- **C**: `nipc_client_call_increment(&client, value, &result)` returns an
  error code, result via out-parameter.
  `nipc_client_call_cgroups_snapshot(&client, req_buf, resp_buf, resp_size, &view)`
  returns an error code, view via out-parameter.
- **Rust**: `client.call_increment(value)` returns `Result<u64>`.
  `client.call_snapshot(&mut resp_buf)` returns `Result<SnapshotView>`.
- **Go**: `client.CallIncrement(value)` returns `(uint64, error)`.
  `client.CallSnapshot(respBuf)` returns `(*SnapshotView, error)`.

If the client is not READY, the call fails immediately without I/O.

### Typed batch calls

Level 2 also provides per-method-type batch call functions. Each batch
call:

1. Encodes each typed request item using the Codec
2. Assembles them into one Level 1 batch message using the batch builder
3. Sends the batch via Level 1 (one message, one message_id)
4. Receives the batch response via Level 1
5. Checks outer `transport_status` — if not OK, reports failure for
   the entire batch without attempting to decode
6. Extracts each response item using Level 1 batch extraction
7. Decodes each response item using the Codec
8. Returns decoded results to the caller

Items are correlated by position: response item 0 corresponds to
request item 0. The batch travels as one logical message — no
pipelining overhead, one round-trip for N items.

## Managed server

Level 2 provides a managed server mode for callers who want the library
to handle connection acceptance and per-session request/response loops.

### Configuration

The caller provides at initialization:

- Service endpoint identity (namespace + name)
- Auth token for handshake verification
- Supported/preferred profiles and directional limits
- Maximum concurrent sessions (worker count limit)
- A handler callback that dispatches by method code to typed helpers

The service set is fixed after startup. Adding or removing services
requires process restart.

### Operation

The managed server internally:

1. Creates a Level 1 listener for the service endpoint
2. Runs an acceptor loop that accepts incoming Level 1 sessions
3. Spawns a thread (C, Rust) or goroutine (Go) per accepted session,
   up to the configured maximum concurrent sessions
4. Each session thread reads one Level 1 message at a time, calls the
   handler callback with the raw request payload, and sends the handler's
   response back via Level 1
5. The handler dispatches by `method_code` and delegates to per-method
   typed dispatch helpers (see Handler contract below)
6. Per-session isolation: each session has its own recv buffer and
   response buffer, no cross-session coordination

### Handler contract

The managed server uses a raw-byte handler at the transport level:

```
handler(method_code, request_payload_bytes, response_buffer) → success/failure
```

Each method type provides a **typed dispatch helper** in the Codec layer
that wraps decode → typed handler → encode. The raw handler dispatches
by `method_code` and delegates to the appropriate typed helper:

```
handler(method_code, req, resp) {
    switch(method_code) {
        INCREMENT:      dispatch_increment(req, resp, on_increment)
        STRING_REVERSE: dispatch_string_reverse(req, resp, on_reverse)
        CGROUPS:        dispatch_cgroups(req, resp, on_cgroups)
    }
}
```

The typed business-logic handler:

- Receives decoded typed data (not raw bytes)
- For simple types (INCREMENT): receives and returns scalar values
- For complex types (CGROUPS_SNAPSHOT): receives a decoded request
  and fills a response builder
- Returns success or failure
- Never sees transport details, wire format, or raw offsets
- Never does encode/decode — the dispatch helper handles it

Handler failure semantics:

- If the handler returns success, the dispatch helper's encoded output
  becomes the response payload and the outer envelope carries
  `transport_status = OK`.
- If the handler returns failure, the library sends a response with
  `transport_status = INTERNAL_ERROR` and an empty payload
  (`payload_len = 0`, `item_count = 1`). Clients receiving
  INTERNAL_ERROR must not attempt to decode the payload.
- Business-level result codes (e.g., "item not found") are not handler
  failures — they are expressed as fields inside the response payload
  via the builder. The handler returns success in that case.

### Batch splitting (planned)

When a batch request arrives (BATCH flag set, item_count > 1), the
managed server:

1. Extracts each item payload using Level 1 batch extraction
2. Calls the handler once per item, collecting each response
3. Assembles individual responses into one Level 1 batch response
   using the batch builder, preserving request order
4. Sends the batch response as one logical message

Items are correlated by position: response item 0 corresponds to
request item 0. If the handler fails on any item, the entire batch
gets `transport_status = INTERNAL_ERROR` with empty payload.

### Shutdown

The caller signals shutdown explicitly. The managed server stops accepting
new connections and cleans up resources. The exact drain/abort mechanics
are implementation details.

## Testing requirements

Level 2 must have:

- **High test coverage** (90%+ enforced): every client state transition, every call path,
  every managed server dispatch path, in all languages and on all
  supported platforms.
- **Client lifecycle tests**: initialize without server running, connect
  on refresh, ready/not-ready transitions, reconnect after failure,
  state reporting accuracy.
- **Retry tests**: call succeeds normally, call fails and retries
  successfully, call fails and retry also fails, call fails when not
  previously READY (no retry attempted).
- **Batch dispatch tests**: single-item message dispatch, batch message
  dispatch with 1 worker, batch message dispatch with multiple workers,
  response order preservation, mixed single and batch messages.
- **Multi-client tests**: multiple concurrent clients to one managed
  server, independent session failure, correct response routing.
- **Convenience path tests**: call when ready, call when not ready
  (returns no-response), call after disconnect.
- **Integration tests**: Level 2 client calling Level 2 managed server,
  across all language pairs (C, Rust, Go), for every method type.

No exceptions. Level 2 is the integration surface that most Netdata
plugins will use. It must be proven correct under all operational
conditions.

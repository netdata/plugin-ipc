# Level 2: Typed Convenience API Specification

## Purpose

Level 2 is a pure convenience orchestration layer. It provides zero unique
functionality. Everything Level 2 does, a caller can do manually with
Level 1 primitives and Codec functions.

Level 2 exists so that the common patterns — blocking request/response,
client lifecycle management, managed multi-client servers — do not have
to be reimplemented by every integration.

## Service Model

Level 2 is built around service kinds, not plugin identity.

- one Level 2 client context targets one service kind
- one managed server endpoint exports one service kind
- one service endpoint serves one request kind only
- clients connect to a service name and do not care which plugin serves it

Examples of service kinds:

- `cgroups-snapshot`
- `ip-to-asn`
- `pid-traffic`

The provider of a service is an operational detail. Clients only know the
service identity and the typed payload contract for that service kind.

## Scope

Level 2 owns:

- Blocking typed request/response call wrappers
- Client context lifecycle (initialize, refresh, ready, status, close)
- Connection state machine with automatic reconnect policy
- Managed server mode (acceptor, per-session request/response loops,
  concurrent session limit)
- Service-kind-specific typed dispatch helpers built from the Codec layer

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

### 2. Callers do not see transports or transport buffers

Level 2 clients and servers have no visibility into transport details.
They do not know whether the underlying connection is UDS, Named Pipe,
or SHM. They do not know whether their request was chunked. They do not
manage connections, sessions, handshakes, packet boundaries, or batch
directories.

The entire transport layer is invisible. Public Level 2 callers interact
only with:

- Typed request values or request views
- Typed response values or response views
- Service identity and connection policy
- Typed handler registration on the server side

Public Level 2 APIs must not require callers to pass request scratch
buffers, response scratch buffers, batch assembly buffers, or raw payload
byte slices.

### 3. Public Level 2 APIs are strongly typed and single-item by default

Each Level 2 method exposes a typed client call and a typed server
callback contract for exactly one logical request/response item.

The client side:

- Accepts one typed request value or request object
- Returns one typed response value or typed response view
- Never exposes raw payload bytes or method codes

The server side:

- Registers the typed handler surface for one service kind
- Receives one decoded typed request value or typed request view
- Returns one typed response value or fills one typed response builder,
  depending on the message type
- Never exposes raw payload bytes, raw response buffers, or outer
  transport metadata to the callback

Raw request decoding, raw response encoding, and batch extraction are
internal Level 2 concerns implemented using Codec + Level 1 primitives.
Each endpoint is bound to one request kind. Level 2 should not expose a
public “one server dispatches many unrelated request kinds” contract.

### 4. Internal reusable buffers are owned by the library

To keep the hot path allocation-free where the typed message shape allows
it, Level 2 owns and reuses its internal wire buffers.

Examples of internal reusable state:

- Per-client request/response scratch buffers
- Per-session receive buffers and response builders
- Batch assembly and batch extraction scratch

These buffers are implementation details. Their sizing is derived from
the negotiated limits and the service-kind-specific Codec contracts. They are
never part of the public Level 2 API.

Borrowed typed views returned by Level 2 therefore have explicit
lifetime rules:

- Client-side response views are valid until the next typed call on the
  same client context, or until the client is closed
- Server-side request views and response builders are valid only for the
  duration of the current callback invocation

If a caller needs to retain data longer, it must explicitly materialize
or copy the typed data.

### 5. At-least-once call semantics

Level 2 client calls are intentionally at-least-once, not exactly-once.

If a call fails and the session was previously READY, Level 2 must
disconnect, reconnect (including a full handshake), and resend the
request. This means the server may receive the same request more than
once. Duplicate requests are acceptable by contract.

There are two important cases:

- For ordinary transport / peer failures, Level 2 reconnects and retries
  once.
- For overflow-driven resize recovery, Level 2 may reconnect more than
  once while negotiated request/response capacities grow. Recovery stops
  when the call succeeds, reconnect fails, a non-overflow error occurs,
  or a reconnect no longer increases the relevant negotiated capacities.

If the session was NOT previously READY, the call fails immediately
without attempting reconnection.

If recovery fails, Level 2 reports failure to the caller.

### 6. No hidden background threads (client)

Level 2 clients do not spawn background threads for connection management.
Connection state transitions (connect, reconnect, disconnect) happen
inside explicit caller-driven operations: `refresh()` and typed call
methods.

The caller owns the timing of connection work by calling `refresh()`
from its own loop at whatever cadence it chooses.

### 7. Optional dependencies and asynchronous startup

Plugin startup order is not guaranteed.

- a client may start before the provider of its service exists
- a provider may restart or disappear while clients are running
- enrichments from external services are optional by design

Therefore:

- `initialize()` must not require the provider to be running
- `refresh()` owns connection and reconnection attempts
- `ready()` must stay cheap and cached
- callers are expected to tolerate `NOT_FOUND` / disconnected states and
  continue operating without that enrichment until the service appears

## Client context

Level 2 provides one persistent client context per service kind. For example,
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
- **INCOMPATIBLE**: handshake profile mismatch, protocol/layout version
  mismatch, or limit negotiation failed.
- **BROKEN**: the connection was previously READY but has broken.

`ready(ctx)` returns true only for READY.

`status(ctx)` returns a detailed snapshot including the current state,
reconnect counts, and operational counters. This is for diagnostics
and logging, not for hot-path decisions.

### Typed single-item calls

Level 2 exposes service-kind-specific blocking call functions. Each call:

1. Encodes the typed request using the Codec
2. Sends it via Level 1 as a single-item message
3. Receives the response via Level 1
4. Checks outer `transport_status` — if not OK, reports failure
   without attempting to decode
5. Decodes the response payload using the Codec
6. Returns the decoded result directly to the caller

The public call signature is typed. The caller provides typed request
data only. It does not provide transport scratch buffers.

Response ownership is defined per method type:

- Fixed-size simple responses return scalar values (or out-parameters in
  C) with no heap allocation
- Variable-size structured responses should return ephemeral typed views
  that borrow internal reusable client storage
- Owned/materialized results are allowed only when the public method
  contract explicitly promises ownership instead of a borrowed view

There are no callbacks on the client side. Every typed call is a
synchronous function that returns the decoded result. The public shape is
equivalent to:

- **C**: a fixed-shape service may expose `call_request(&client, req, &resp)`.
  A snapshot service may expose `call_snapshot(&client, &view)`.
- **Rust**: a fixed-shape service may expose `client.call(req) -> Result<Resp>`.
  A snapshot service may expose `client.call_snapshot() -> Result<SnapshotView<'_>>`.
- **Go**: a fixed-shape service may expose `client.Call(req) (Resp, error)`.
  A snapshot service may expose `client.CallSnapshot() (*SnapshotView, error)`.

When a client call returns a borrowed response view, that view is valid
until the next typed call on the same client context or until the client
is closed, unless the service-kind-specific contract states a narrower
lifetime.

If the client is not READY, the call fails immediately without I/O.

### Typed batch calls

Level 2 also provides service-kind-specific batch call functions. Each batch
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

The public batch-call signature is also typed. The caller provides typed
request items only. It does not provide raw batch payloads or batch
scratch buffers.

The returned result is service-kind-specific:

- For fixed-size items, a typed collection of values may be returned
- For variable-size items, a typed batch view may be returned
- Owned/materialized collections are allowed only when the public method
  contract explicitly promises ownership

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
- The typed handler implementation for that one service kind

The endpoint identity is fixed after startup. A given listener exports
one service kind only.

### Operation

The managed server internally:

1. Creates a Level 1 listener for the service endpoint
2. Runs an acceptor loop that accepts incoming Level 1 sessions
3. Spawns a thread (C, Rust) or goroutine (Go) per accepted session,
   up to the configured maximum concurrent sessions
4. Each session thread reads one Level 1 message at a time into internal
   reusable per-session storage
5. Level 2 decodes the request for that service kind, invokes the typed
   callback, encodes the typed response, and sends it back via Level 1
6. Per-session isolation: each session has its own internal reusable
   buffers and builders, with no cross-session coordination

### Handler contract

The public managed-server contract is typed.

The caller registers the typed handler surface for that service kind.
The public contract is not “one server with many unrelated methods”.

The typed business-logic callback:

- Receives decoded typed data (not raw bytes)
- For simple services: receives and returns scalar or fixed-shape values
- For snapshot services: receives a decoded request
  and fills a response builder
- Returns success or failure
- Never sees transport details, wire format, raw response buffers, or
  outer message headers
- Never does encode/decode — Level 2 + Codec handle that internally

Internal note:

- Implementations may still validate the request code and call
  service-kind-specific Codec helpers internally
- That internal structure is not part of the public Level 2 contract

Handler failure semantics:

- If the typed callback returns success, the encoded output becomes the
  response payload and the outer envelope carries `transport_status = OK`.
- If the typed callback returns failure, the library sends a response with
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
2. Decodes each request item to its typed request form
3. Calls the typed callback once per item, collecting each typed response
4. Assembles individual responses into one Level 1 batch response
   using the batch builder, preserving request order
5. Sends the batch response as one logical message

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
- **Typed API boundary tests**: public Level 2 clients and servers never
  require caller-managed wire buffers or raw payload bytes.
- **Multi-client tests**: multiple concurrent clients to one managed
  server, independent session failure, correct response routing.
- **Convenience path tests**: call when ready, call when not ready
  (returns no-response), call after disconnect.
- **Integration tests**: Level 2 client calling Level 2 managed server,
  across all language pairs (C, Rust, Go), for every method type.

No exceptions. Level 2 is the integration surface that most Netdata
plugins will use. It must be proven correct under all operational
conditions.

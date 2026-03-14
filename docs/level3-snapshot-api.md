# Level 3: Snapshot API Specification

## Purpose

Level 3 is a caching layer on top of Level 2. It adds zero protocol,
wire format, or transport behavior. Level 2 is fully complete without it.

Level 3 provides local cache management and lookup helpers for services
that publish finite datasets. It uses Level 2 typed calls for refresh
and adds only client-side caching logic: build a local cache from the
response, preserve it on failure, provide cheap lookup by key.

The motivating use case is cgroups.plugin publishing cgroup-to-container
naming data that multiple consumer plugins (ebpf.plugin, apps.plugin,
network-viewer.plugin, and others) need to access frequently in their
hot paths.

Level 3 exists so that each consumer does not have to reimplement the
same refresh/cache/lookup pattern.

## Scope

Level 3 is client-side only. There is no Level 3 server component —
the server side is pure Level 2 (a normal typed request/response
handler that builds a snapshot response through the Codec builder).

Level 3 owns:

- Periodic snapshot refresh orchestration (client side)
- Local cache construction from snapshot responses (client side)
- Local cache lookup by key (client side)
- Cache preservation on refresh failure (client side)

Level 3 does NOT own:

- Transport, framing, sequencing (Level 1)
- Payload encoding/decoding (Codec)
- Connection management, retry policy, typed calls (Level 2)
- Server-side snapshot building or publication (Level 2 + Codec)

## Dependency

Level 3 depends exclusively on Level 2. It uses Level 2 typed calls to
request snapshot refreshes and Level 2 client context lifecycle for
connection management.

Level 3 must NOT bypass Level 2 to access Level 1 directly. This ensures
that retry policy, connection state management, and typed dispatch remain
consistent.

## Principles

### 1. Caller-driven refresh

Level 3 does not spawn background threads or timers. The caller drives
refresh by calling a refresh function from its own loop at whatever
cadence it chooses.

This matches the plugin execution model in Netdata: plugins have their
own collection loops and control their own timing.

### 2. Cache preservation on failure

If a refresh fails (transport error, server unavailable, malformed
response), the Level 3 helper must preserve the previously valid cache.
The consumer continues to use stale-but-valid data rather than losing
its cache.

The cache becomes empty only if no successful refresh has ever occurred.

### 3. Cheap lookup in hot paths

Local cache lookup must be a pure in-memory operation with no I/O, no
syscalls, and no transport interaction. Lookups access only the local
cache that was built from the last successful refresh.

This is critical because lookups happen in hot collection paths (e.g.,
per-PID processing in ebpf.plugin) at rates far higher than refresh.

### 4. One producer, many consumers

The snapshot service model supports one canonical producer (e.g.,
cgroups.plugin) and many independent consumers. Each consumer maintains
its own Level 3 helper with its own local cache. Consumers are
independent — they refresh at their own pace and do not coordinate
with each other.

### 5. Full snapshot refresh first

The first implementation always requests and receives the full snapshot
on every refresh. Generation-aware refresh (send known_generation,
receive unchanged or delta) and push-based update models are future
extensions, not current requirements.

The full-refresh model is simple, correct, and sufficient for the
current dataset sizes.

## Helper lifecycle

### Initialize

Create a Level 3 helper for a specific snapshot service. Configuration
includes:

- Service identity (namespace + name)
- Auth token
- Transport profile preferences
- Directional limits (particularly response payload and batch item
  limits, since the server controls snapshot size)

Initialize does NOT connect and does NOT require the server to be
running. The underlying Level 2 client context is created in
DISCONNECTED state.

### Refresh

The caller calls refresh periodically from its own loop. Refresh:

1. Delegates to the Level 2 client context for connection management
   (connect if disconnected, reconnect if broken)
2. If the Level 2 context is READY, sends a typed snapshot refresh
   request via Level 2
3. If the response is successful:
   - Rebuilds the local cache from the snapshot data
   - Updates the cache generation/timestamp
4. If the response fails (transport error, server error, malformed
   payload):
   - Preserves the previous cache
   - Disconnects the Level 2 context (so the next refresh will
     attempt reconnection)

Refresh will reconnect and retry once if previously READY (inherited from Level 2's
at-least-once semantics). If the retry also fails, the previous cache
is preserved.

### Ready

A cheap cached predicate indicating whether the helper has a usable
cache. Returns true if at least one successful refresh has occurred.
No I/O, no syscalls.

Note: ready means "has cached data," not "is currently connected."
A helper can be ready (has cache) while disconnected (server went
away after last successful refresh).

### Lookup

Look up an item in the local cache by key. For the cgroups snapshot
service, the key is hash + name.

Lookup is a pure in-memory operation. It accesses only the local cache.
It never triggers I/O or refresh.

If the cache is empty (no successful refresh yet), lookup returns
not-found.

### Status

Returns detailed operational state: connection state (from Level 2),
cache state (empty or populated), refresh counts (success, failure),
last successful refresh timestamp.

### Close

Tears down the helper: closes the Level 2 client context, releases the
local cache.

## Cache construction

When a successful snapshot response arrives, Level 3:

1. Iterates over the snapshot items using Codec decode functions
   (accessed via Level 2's typed call path)
2. Copies the needed data from ephemeral views into owned local
   storage (views are only valid during the callback — Level 3 must
   materialize them)
3. Builds or rebuilds the local lookup index (e.g., hash table keyed
   by hash + name)
4. Replaces the previous cache atomically (the old cache remains valid
   until the new one is fully constructed)

Cache construction is a full rebuild on every successful refresh. There
is no incremental merge or differential update in the first
implementation.

## Reconnect behavior

If the server restarts or the connection breaks:

1. The Level 2 context transitions to BROKEN or DISCONNECTED
2. On the next refresh call, Level 2 attempts reconnection (including
   full handshake)
3. If reconnection succeeds, Level 3 requests a fresh full snapshot
4. If the snapshot response is valid, Level 3 rebuilds the cache from
   scratch
5. If reconnection or refresh fails, the previous cache is preserved

The consumer never loses its cache due to a transient server restart.

## Error handling summary

| Scenario | Cache behavior | Connection behavior |
|----------|---------------|-------------------|
| Refresh succeeds | Cache rebuilt from new snapshot | Stays connected |
| Refresh fails, was READY | Previous cache preserved | Disconnects, will retry next refresh |
| Refresh fails, was not READY | Previous cache preserved (or empty if never refreshed) | Stays disconnected, will retry next refresh |
| Server restarts | Previous cache preserved until next successful refresh | Reconnects on next refresh |
| Malformed response | Previous cache preserved | Disconnects (protocol violation) |

## Service-specific helpers

Each snapshot service type gets its own Level 3 helper with
service-specific:

- Refresh call (wraps the specific Level 2 typed call)
- Cache data structure (tailored to the service's item type)
- Lookup function (keyed by the service's identity fields)

The general refresh/cache/lookup lifecycle is the same across all
snapshot services. Only the data shape and lookup key differ.

## Testing requirements

Level 3 must have:

- **Refresh success tests**: fresh cache build from snapshot, cache
  rebuild on subsequent refresh, lookup correctness after refresh.
- **Refresh failure tests**: refresh failure preserves previous cache,
  refresh failure on first attempt leaves cache empty, refresh failure
  after previous success preserves old cache.
- **Reconnect tests**: server restart followed by successful refresh
  rebuilds cache, server restart followed by failed refresh preserves
  old cache.
- **Malformed response tests**: corrupt snapshot payload preserves
  previous cache and disconnects.
- **Lookup tests**: lookup by key returns correct item, lookup for
  missing key returns not-found, lookup on empty cache returns
  not-found.
- **Large dataset tests**: refresh with maximum item count at the
  negotiated limit, refresh with items containing maximum-length
  strings.
- **Lifecycle tests**: initialize without server running, first refresh
  connects and populates cache, close releases all resources.
- **Integration tests**: full Level 3 helper round-trip across all
  language pairs (C, Rust, Go) over baseline and SHM transports.

No exceptions. Level 3 is the integration surface for snapshot-style
services in Netdata. Cache loss or corruption during operational
events (server restarts, transient failures) is unacceptable.

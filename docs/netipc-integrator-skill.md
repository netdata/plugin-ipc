# Netdata `plugin-ipc` Integrator Skill

## Purpose

This document is the single entry point for adding new `plugin-ipc` integrations
to Netdata plugins.

Use it when you need to:

- decide whether a Netdata integration should use L1, L2, or L3
- add a new typed request/response service to the library
- add a new client or server in C, Rust, or Go
- integrate a new service into Netdata plugins on Linux and Windows
- handle startup ordering, reconnects, retries, cache refreshes, and failures
- log enough operational state to make the integration supportable

This is an integrator guide, not a wire-format spec. The specs remain
authoritative:

- [README.md](../README.md)
- [docs/getting-started.md](getting-started.md)
- [docs/level1-transport.md](level1-transport.md)
- [docs/level1-wire-envelope.md](level1-wire-envelope.md)
- [docs/level2-typed-api.md](level2-typed-api.md)
- [docs/level3-snapshot-api.md](level3-snapshot-api.md)

## Core Reality

Before adding anything, internalize these facts:

- The contract is service-oriented, not plugin-oriented.
  - Clients connect to a service name such as `cgroups-snapshot`.
  - Clients do not care which plugin/process serves it.
- One endpoint serves one request kind only.
  - Public L2/L3 is not a generic multi-method RPC router.
- Startup order is not guaranteed.
  - Clients must tolerate the service not existing yet.
  - Providers may restart or disappear during runtime.
- L2 client calls are at-least-once, not exactly-once.
  - A previously READY client may reconnect and resend.
  - Server handlers must therefore be safe for duplicates.
- A successful handshake locks the selected transport profile for that session.
  - There is no same-session fallback.
  - If SHM attach fails on the client side, recovery happens through a new
    connection and a new handshake without SHM.
- Public L2/L3 is transport-agnostic from the caller perspective.
  - Callers should not branch on UDS vs Named Pipe or SHM vs baseline.
  - Transport-specific tuning belongs below the public typed API.

## What Exists Today

Current implemented layers:

- L1 transport
  - POSIX: UDS baseline + POSIX SHM fast path
  - Windows: Named Pipe baseline + Win SHM fast path
- Codec
  - typed encode/decode
  - typed views
  - response builders
- L2 typed API
  - client lifecycle
  - blocking typed calls
  - managed typed server
  - reconnect / retry behavior
- L3 cache
  - typed snapshot refresh
  - local owned cache
  - O(1)-style lookup
  - cache preservation on failure

Important current product fact:

- The current public typed service facade is `cgroups-snapshot`.
- The repo also contains internal raw/test service helpers for `increment` and
  `string-reverse`, but those are not the production public Netdata-facing
  service contract.

Relevant implementation roots:

- C public service API:
  - [src/libnetdata/netipc/include/netipc/netipc_service.h](../src/libnetdata/netipc/include/netipc/netipc_service.h)
  - [src/libnetdata/netipc/include/netipc/netipc_protocol.h](../src/libnetdata/netipc/include/netipc/netipc_protocol.h)
- Rust public service API:
  - [src/crates/netipc/src/service/cgroups.rs](../src/crates/netipc/src/service/cgroups.rs)
- Go public service API:
  - [src/go/pkg/netipc/service/cgroups/](../src/go/pkg/netipc/service/cgroups/)
- Internal raw helpers used to build public typed services:
  - [src/crates/netipc/src/service/raw.rs](../src/crates/netipc/src/service/raw.rs)
  - [src/go/pkg/netipc/service/raw/](../src/go/pkg/netipc/service/raw/)

Important current limitation:

- the public typed API is not generic yet
- today, the production public typed facade is `cgroups-snapshot`
- in C, that means the public typed call/cache/server surface is still
  cgroups-shaped
- if you need a brand-new typed service, the work starts in the upstream
  source-of-truth repository:
  - `github.com/netdata/plugin-ipc`
- do not begin by editing only a vendored copy inside another repository

Non-negotiable rule:

- if your integration needs a new structured data type or a new typed service
  contract, you must add the structured codec in all three supported language
  implementations:
  - C
  - Rust
  - Go
- this is not optional
- do not create a Netdata integration that is C-only at the protocol/codec
  layer while leaving Rust or Go without the same structured contract

## Which Level To Use

### Use L1 only when all of these are true

- You need direct control over transport behavior.
- You are intentionally working with framed byte messages.
- You need something L2 does not expose and should not expose.
- You are prepared to own:
  - handshake handling
  - transport profile behavior
  - retries / reconnects
  - raw payload encode/decode
  - batch and chunk rules

Use L1 for:

- protocol bring-up
- transport experiments
- low-level diagnostics
- tests that intentionally exercise malformed envelopes or transport errors

Do not use L1 in ordinary Netdata plugin business logic if L2 or L3 fits.

### Use L2 when you have a typed request/response service

Use L2 when:

- the interaction is naturally request/response
- the caller wants typed methods, not raw buffers
- the client can tolerate at-least-once semantics
- the caller does not need a persistent local cache

Typical L2 uses:

- request a point-in-time answer
- ask another plugin/helper for enrichment
- expose one typed service endpoint from a provider plugin

### Use L3 when the provider publishes a finite dataset and consumers need hot-path lookup

Use L3 when:

- the service publishes a full snapshot
- consumers refresh periodically
- consumers need pure local lookup afterward
- stale-but-valid cached data is better than no data

Typical L3 uses:

- cgroup metadata
- identity or lookup tables
- datasets reused heavily inside a collection loop

Do not use L3 if the caller only needs occasional typed calls and no cache.

## Netdata Integration Model

The intended Netdata pattern is:

- one producer plugin exposes one typed service
- one or more consumer plugins use L2 or L3 to consume it
- consumer refresh timing is caller-driven from the consumer's own loop

Typical role split:

- Provider plugin:
  - owns data collection
  - exposes an L2 managed server
  - fills typed response builders
- Consumer plugin:
  - uses L2 for occasional typed request/response
  - or uses L3 for periodic refresh + hot-path local lookup

Recommended placement:

- Put the server in the plugin that already owns the authoritative data.
- Put L3 caches in consumers, not in the provider.
- Keep service names stable and independent from plugin binary names.

## Netdata-Specific Glue You Must Use

This section assumes your current working tree is the Netdata repository.

When integrating into Netdata itself, these are the first files to read in the
current Netdata working tree:

- Netdata helper glue:
  - `src/libnetdata/netipc/netipc_netdata.h`
  - `src/libnetdata/netipc/netipc_netdata.c`
- Example Netdata integrations in the current working tree:
  - `src/collectors/cgroups.plugin/cgroup-netipc.c`
  - `src/collectors/ebpf.plugin/ebpf_cgroup.c`

Important ownership rule:

- `netipc_netdata.h` / `netipc_netdata.c` are Netdata integration files
- they are not part of the upstream `github.com/netdata/plugin-ipc` repo
- if the user prompt gives you a local checkout path for
  `github.com/netdata/plugin-ipc`, use that path for upstream library work
- otherwise, reason about upstream ownership by repository, not by assuming a
  fixed local filesystem location

Netdata-specific integration rules:

- include `netipc_netdata.h`, not just the raw public headers
- derive auth with `netipc_auth_token()`
- use `os_run_dir(true)` on the provider side
- use `os_run_dir(false)` on the consumer side
- treat these as a matched pair
- if provider and consumer do not agree on auth token or run dir, the client
  will stay in `AUTH_FAILED` or `NOT_FOUND`

Do not invent your own auth-sharing scheme inside Netdata unless there is an
explicit product decision to do so.

## Library Constraints And Assumptions You Must Not Guess

These are core design constraints of the library. Integrators must treat them
as explicit assumptions, not as implementation details they can reinterpret.

- L2 clients and L3 caches are caller-driven objects.
  - There are no hidden background refresh threads.
  - The caller must decide when `refresh()` happens and from which execution
    context.
  - Evidence:
    - [netipc_service.h](../src/libnetdata/netipc/include/netipc/netipc_service.h)
    - `No hidden threads. Call from your own loop at your own cadence.`
- Managed servers are concurrent.
  - The server accept loop can run multiple client sessions concurrently up to
    `worker_count`.
  - Handler code must therefore assume concurrent execution.
  - Evidence:
    - [netipc_service.h](../src/libnetdata/netipc/include/netipc/netipc_service.h)
    - `max concurrent sessions`
    - [client.go](../src/go/pkg/netipc/service/raw/client.go)
    - `goroutine per session (up to workerCount concurrently)`
- The library does not make provider-owned plugin data thread-safe.
  - If a handler reads mutable plugin state, the integration must define the
    synchronization model explicitly.
  - The library only dispatches requests. It does not protect your plugin's
    data structures for you.
- L2 clients and L3 caches should be treated as single-owner mutable objects
  unless the integrator adds external synchronization.
  - They are not documented as internally synchronized shared objects.
  - On the same object instance, do not concurrently call:
    - `refresh()`
    - blocking typed methods
    - `close()`
    - `lookup()` while a refresh may rebuild the cache
  - In C, the public client/cache types hold mutable state and reusable scratch
    buffers without a public thread-safety contract.
  - In Go, the public/raw client and cache types mutate internal state and do
    not use internal locking.
  - In Rust, mutating operations require `&mut self`, which already pushes the
    caller toward serialized ownership.
  - In practice:
    - C and Go require the integrator to prevent data races explicitly
    - Rust prevents some of these mistakes at compile time, but the ownership
      model still must be designed intentionally
- Returned views and references have validity limits.
  - C typed response views are valid only until the next call on the same
    client context.
  - C cache lookup pointers are valid only until the next successful refresh.
  - Rust follows the same ownership idea through borrowed references.
  - Do not hold these across refresh/call boundaries or share them with other
    threads without copying the needed data first.
- One service endpoint exposes one service kind only.
  - Public L2/L3 is not a generic multi-method RPC router.
  - Do not design one endpoint that multiplexes unrelated method families.
- Platform transport differences are hidden at the public typed boundary.
  - Integrators should not branch business logic on UDS vs Named Pipe or SHM vs
    baseline.
  - Integrators still must think about data availability and semantics
    differences across operating systems.

## Public API Shape By Language

### C

Public L2/L3 config and lifecycle live in:

- [src/libnetdata/netipc/include/netipc/netipc_service.h](../src/libnetdata/netipc/include/netipc/netipc_service.h)

Current public shapes:

- client config:
  - `nipc_client_config_t`
- server config:
  - `nipc_server_config_t`
- client lifecycle:
  - `nipc_client_init()`
  - `nipc_client_refresh()`
  - `nipc_client_ready()`
  - `nipc_client_status()`
  - `nipc_client_close()`
- typed server lifecycle:
  - `nipc_server_init_typed()`
  - `nipc_server_run()`
  - `nipc_server_stop()`
  - `nipc_server_drain()`
  - `nipc_server_destroy()`
- L3 cache lifecycle:
  - `nipc_cgroups_cache_init()`
  - `nipc_cgroups_cache_refresh()`
  - `nipc_cgroups_cache_ready()`
  - `nipc_cgroups_cache_lookup()`
  - `nipc_cgroups_cache_status()`
  - `nipc_cgroups_cache_close()`

### Rust

Public typed service facade lives in:

- [src/crates/netipc/src/service/cgroups.rs](../src/crates/netipc/src/service/cgroups.rs)

Current public shapes:

- client:
  - `CgroupsClient::new()`
  - `refresh()`
  - `ready()`
  - `status()`
  - `call_snapshot()`
  - `close()`
- managed server:
  - `ManagedServer::new()`
  - `ManagedServer::with_workers()`
  - `run()`
  - `stop()`
- L3 cache:
  - `CgroupsCache::new()`
  - `refresh()`
  - `ready()`
  - `lookup()`
  - `status()`
  - `close()`

### Go

Public typed service facade lives in:

- [src/go/pkg/netipc/service/cgroups/](../src/go/pkg/netipc/service/cgroups/)

Current public shapes:

- client:
  - `NewClient()`
  - `Refresh()`
  - `Ready()`
  - `Status()`
  - `CallSnapshot()`
  - `Close()`
- managed server:
  - `NewServer()`
  - `NewServerWithWorkers()`
  - `Run()`
  - `Stop()`
- L3 cache:
  - `NewCache()`
  - `Refresh()`
  - `Ready()`
  - `Lookup()`
  - `Status()`
  - `Close()`

### OS contract

Within each language:

- the public L2/L3 integration source should compile unchanged on Linux and
  Windows
- public L2/L3 caller code should not import platform transport packages
  directly for business logic

The library handles:

- UDS vs Named Pipe baseline transport
- POSIX SHM vs Win SHM fast path
- handshake and profile negotiation

## What Public L2/L3 Does And Does Not Expose

Public L2/L3 does expose:

- connection state
- counters
- typed calls
- typed handlers
- cache refresh / lookup

Public L2/L3 does not currently expose:

- negotiated `selected_profile`
- actual current data-plane proof such as "baseline" vs "SHM attached"
- raw handshake structures
- raw packet size
- raw transport buffers

This matters operationally:

- If a plugin needs to prove which transport is actually in use, public L2/L3
  status is not enough today.
- In that case, add integration-side observability at the plugin boundary.
  Example: log the selected profile and actual SHM-vs-baseline data plane when
  wiring the integration into Netdata, as was done in the Netdata eBPF/cgroups
  integration.

## Failure And Retry Contract

### L2 client state model

The public state machine is:

- `DISCONNECTED`
- `CONNECTING`
- `READY`
- `NOT_FOUND`
- `AUTH_FAILED`
- `INCOMPATIBLE`
- `BROKEN`

Operational meaning:

- `DISCONNECTED`
  - normal idle disconnected state
  - `refresh()` will attempt connect
- `NOT_FOUND`
  - service endpoint does not exist yet
  - `refresh()` will attempt connect again later
- `READY`
  - calls may proceed
- `BROKEN`
  - connection was previously READY and failed
  - `refresh()` will reconnect
- `AUTH_FAILED`
  - handshake auth check failed
  - this is not a transient network condition
  - fix config or auth token
- `INCOMPATIBLE`
  - protocol/layout/negotiation mismatch
  - this is not a transient network condition
  - fix code/config/version mismatch

Practical rule:

- treat `NOT_FOUND` and `BROKEN` as routine operational states
- treat `AUTH_FAILED` and `INCOMPATIBLE` as actionable misconfiguration or code
  mismatch

### L2 retry semantics

L2 typed calls are at-least-once:

- if a call fails and the client was previously READY:
  - disconnect
  - reconnect
  - retry
- ordinary failures retry once
- overflow-driven resize recovery may reconnect multiple times while capacities
  grow

Implication:

- server handlers must be duplicate-safe
- do not design exactly-once business semantics around L2 calls

### SHM attach failure

Current required behavior:

- if handshake selects SHM and the client cannot attach SHM locally:
  - close that session
  - remove SHM from future proposals for that client context
  - reconnect with a new handshake
  - continue on baseline if that reconnect succeeds

This is not same-session fallback.

Normative reference:

- [level1-wire-envelope.md](level1-wire-envelope.md)
- [level1-transport.md](level1-transport.md)

### L3 refresh behavior

L3 refresh:

- drives the underlying L2 client
- requests a fresh typed snapshot
- rebuilds the local cache on success
- preserves the previous cache on failure

Practical rule:

- `Ready()` on L3 means "has usable cached data"
- it does not mean "currently connected"

## Logging And Observability Best Practices

### What to log

For any real Netdata integration, log at least:

- service name
- whether the component is acting as client or server
- state transitions
- first successful connect
- repeated refresh/call failures with suppression or rate limiting
- first successful cache population
- cache item counts / generation on successful snapshot refresh

For failure logs, include:

- current client state
- whether the failure happened during `refresh()` or a typed call
- whether previous cached data was preserved
- the public error/status class

### What not to spam

Do not log every successful refresh or every successful call in steady state.

Prefer:

- log on state change
- log on first success
- log every Nth repeated failure, or on first failure after success

### Transport profile logging

Because public L2/L3 does not currently expose negotiated profile/data-plane
status, use this rule:

- if the integration needs human-verifiable transport proof, add explicit
  integration-side logs near the plugin boundary
- log both:
  - negotiated profile if available in that integration path
  - actual data-plane use if the integration can observe SHM attach success

Do not invent transport-state logs from public L2/L3 status if the information
is not actually exposed there.

## Refresh Strategy Guidelines

### L2-only clients

Use L2 directly when:

- calls are occasional
- you do not need a hot-path local cache
- stale local data is not useful

Recommended pattern:

- initialize once at startup
- call `refresh()` from the plugin loop
- call typed methods only when `Ready()` is true
- tolerate `NOT_FOUND` and continue operating without the enrichment

### L3 clients

Use L3 when:

- refresh cadence is much lower than lookup cadence
- the plugin needs frequent in-memory lookup between refreshes
- the consumer can afford holding a local owned copy of the provider snapshot

Recommended pattern:

- initialize once at startup
- refresh from the plugin’s normal loop
- do all hot-path lookup against the local cache only
- preserve old cache on failure
- do not trigger refresh from the hot path
- if refresh is repeatedly failing, add caller-side backoff or suppression
  instead of tight retry loops

Operational note:

- L3 caches keep an owned copy of the current dataset in the consumer address
  space
- for large datasets, treat cache size and rebuild cost as part of the
  integration design

### Provider server loops

Recommended pattern:

- initialize the managed server once
- run it in the plugin’s server thread
- stop it cleanly on shutdown
- use graceful drain where shutdown correctness matters

### Function-only / on-demand consumers

Some Netdata plugins do not have a normal background refresh loop. They answer
functions on demand and may emit many rows in one request.

Use this rule:

- if one function call will render many enriched rows, do not do one L2 call
  per row
- prefer one refresh at function entry, then pure local lookup while rendering
- that usually means:
  - L3 snapshot/cache if the provider publishes a finite dataset
  - or a local temporary map built from one L2 response if you intentionally do
    not want a persistent cache

Typical example:

- a plugin that renders a large table keyed by PID, cgroup id, interface name,
  or socket identity should usually refresh once and then look up locally
- a plugin that only asks one occasional question can stay on direct L2

Concurrency note:

- if the function endpoint itself may run concurrently, do not let multiple
  invocations race on the same mutable client or cache object
- either:
  - give each invocation its own temporary client/cache object
  - or protect a shared object with one explicit synchronization strategy

## How To Add A New Typed Service

This is the critical workflow for extending the upstream source-of-truth library
at `github.com/netdata/plugin-ipc`.

### Step 1: decide whether a new service is really needed

Ask:

- Is this a new service kind, or just another consumer of an existing one?
- Is it request/response, or full-snapshot publication?
- Does the consumer need L2 or L3?
- Does the hot path need a local owned cache?

If it is just another consumer of an existing typed service, do not add a new
wire method.

### Step 2: define the service contract first

Before code:

- define the service name
- define request and response semantics
- define whether the service is:
  - fixed-size request/response
  - variable-size structured response
  - full-snapshot response
- define duplicate-call safety
- define who owns authoritative data
- define whether consumers need L3

Also decide these up front:

- the lookup key the consumer will actually need in the hot path
- whether the response is complete or may be truncated
- whether the provider must expose freshness metadata
- whether the key is stable across namespaces or process reuse

If you do not decide these before code, you will usually redesign the wire
format later.

### Step 2.5: split ownership before implementation

For a new typed service, there are usually two layers of work:

- upstream source-of-truth work in `github.com/netdata/plugin-ipc`
  - method code
  - codec
  - typed dispatch
  - public typed facade
  - optional L3 cache
  - tests in C, Rust, and Go
- Netdata integration work in the current working tree
  - provider handler logic
  - consumer refresh / lookup wiring
  - plugin lifecycle wiring
  - plugin logs
  - build-system integration

Practical rule:

- if the public typed API does not already expose your new service kind, do the
  upstream library work in `github.com/netdata/plugin-ipc` first
- only then wire the provider and consumer into Netdata

### Step 3: add the typed codec in all three languages

This is a hard requirement.

You must add the typed request/response codec consistently across:

- C
- Rust
- Go

Typical tasks:

- add method code constants
- add typed request/response structs or views
- add encode/decode helpers
- add builder helpers if the response is variable-sized or snapshot-based
- add dispatch helper(s)

Current patterns to mirror:

- C:
  - [netipc_protocol.h](../src/libnetdata/netipc/include/netipc/netipc_protocol.h)
  - [netipc_protocol.c](../src/libnetdata/netipc/src/protocol/netipc_protocol.c)
- Rust:
  - [src/crates/netipc/src/protocol/](../src/crates/netipc/src/protocol/)
- Go:
  - [src/go/pkg/netipc/protocol/](../src/go/pkg/netipc/protocol/)

Do not add the method in only one language.

Reality check:

- if the new integration introduces a new structured wire contract, the work is
  incomplete until the structured codec exists in:
  - C
  - Rust
  - Go
- a review that accepts the new service in only one language is wrong
- do not continue to typed integration wiring in Netdata until this step is
  complete in all three languages

### Step 4: add L2 raw helpers

Once the codec exists:

- add typed raw client call support
- add typed dispatch support on the server side
- add envelope validation for the new method code

Patterns to mirror:

- Rust raw client/server:
  - [src/crates/netipc/src/service/raw.rs](../src/crates/netipc/src/service/raw.rs)
- Go raw client/server:
  - [src/go/pkg/netipc/service/raw/](../src/go/pkg/netipc/service/raw/)
- C managed server / typed call path:
  - [src/libnetdata/netipc/src/service/](../src/libnetdata/netipc/src/service/)

Reality check:

- in C today, `nipc_server_init_typed()` is still wired for the existing typed
  production service
- a brand-new typed service therefore requires upstream library work before a
  Netdata plugin can consume it through the public typed API

### Step 5: add the public typed service facade

If the service is intended for real plugin integration, do not stop at raw
helpers.

Add a public typed facade like the current `cgroups-snapshot` service:

- public service-level config types
- public typed client
- public typed managed server
- public typed handler type
- optional public typed cache if the service is snapshot-based

This is what Netdata plugin code should consume.

### Step 6: add L3 only if the data model needs it

Add L3 only if:

- the service returns a finite dataset snapshot
- the consumer wants local owned lookup after refresh

Do not add L3 for ordinary point request/response methods.

### Step 7: add tests before claiming the service is ready

You need:

- codec tests
- malformed decode tests
- dispatch tests
- typed L2 client/server tests
- cross-language interop tests
- Linux and Windows coverage
- if L3 exists:
  - refresh success tests
  - refresh failure preserves cache tests
  - reconnect/recovery tests
  - lookup tests

## How To Integrate A New Service Into Netdata

### Server integration checklist

1. Put the server in the plugin that owns the authoritative data.
2. Keep the service name stable and explicit.
3. Expose one service endpoint for one request kind only.
4. Make the handler duplicate-safe.
5. Make startup independent from consumers.
6. Add supportable logs for:
   - start
   - stop
   - first client success if needed
   - repeated failures with suppression

### Provider synchronization checklist

Do not skip this.

Managed server handlers run outside the plugin's main collection loop. If the
provider reads mutable plugin-owned state, you must define the synchronization
model explicitly.

Choose one of these patterns:

- lock the provider's authoritative state while copying the data needed for the
  response
- publish a stable snapshot from the provider loop and let the handler serve
  that published snapshot

Prefer:

- copy-under-lock, build-after-unlock

That keeps the lock hold time short and avoids making response serialization a
collector hot-path stall.

Do not:

- iterate live mutable structures lock-free from a worker thread
- hold a global collector lock while doing large response serialization

### Client and cache concurrency checklist

Do not skip this either.

Public L2 clients and L3 caches are mutable objects. The library does not
document them as generally safe for concurrent shared access.

Use these rules:

- do not call `refresh()` concurrently on the same client object
- do not call blocking typed methods concurrently on the same client object
  unless that specific API explicitly promises it
- do not perform `lookup()` concurrently with `refresh()` on the same L3 cache
  object unless you add external synchronization
- do not share C/Rust response views or cache item references across threads
  without copying the needed data
- if multiple threads need the same enrichment data:
  - either give each thread its own client/cache object
  - or wrap shared access in one explicit synchronization strategy

Practical consequences:

- a function-only consumer should usually refresh once, then do all lookups from
  one stable local state while rendering
- if a plugin has one hot path and one refresh path on different threads, the
  cache ownership and handoff model must be designed explicitly
- if you need lock-free read paths, publish immutable snapshots instead of
  refreshing a shared mutable cache in place

### Client integration checklist

1. Initialize once at startup.
2. Do not assume the provider is already running.
3. Drive `refresh()` from the plugin’s own loop.
4. Treat `NOT_FOUND` as normal.
5. Treat `AUTH_FAILED` / `INCOMPATIBLE` as actionable breakage.
6. If you need hot-path lookup, build an L3 cache.
7. If you need only occasional typed requests, use L2 directly.

For function-only consumers:

- refresh once at function entry if the response will render many rows
- render from local lookup after that
- degrade to "unknown" or preserved stale cache when enrichment is absent
- do not issue one IPC call per rendered row

### Provider/consumer ownership checklist

- Provider owns:
  - data collection
  - typed handler logic
  - response construction
- Consumer owns:
  - refresh cadence
  - local cache policy
  - degradation behavior when provider is absent

### Generic design checks before coding

Before you define a new service or integrate one into a plugin, answer these:

- Data availability:
  - does the provider have equivalent data on Linux and Windows?
  - if not, what is the documented behavior on the weaker platform?
- Identity:
  - is the lookup key stable enough?
  - do you need namespace identity, start-time identity, or another discriminator
    in addition to a simple pid/name/hash?
- Completeness:
  - can the dataset be truncated or partial?
  - how does the consumer detect that?
- Freshness:
  - how stale can cached data be before it becomes misleading?
  - what does the consumer show when enrichment is absent or stale?
- Concurrency:
  - which thread owns refresh?
  - which thread owns lookup?
  - does the provider read live mutable state, and how is that synchronized?
- Failure behavior:
  - what happens on `NOT_FOUND`, `BROKEN`, `AUTH_FAILED`, and `INCOMPATIBLE`?
  - what is routine and what is actionable?
- Observability:
  - what must be logged once, on state change, and on repeated failure?
  - if transport proof matters, where will that be logged at the integration
    boundary?

## Platform Notes

### Linux / POSIX

- baseline transport is UDS `SOCK_SEQPACKET`
- fast path is POSIX SHM

### Windows

- baseline transport is Named Pipe
- fast path is Windows SHM

### Cross-platform rule

For L2/L3 caller code:

- do not write separate plugin business logic for Linux and Windows unless the
  business semantics truly differ
- if you find yourself branching on transport details in public typed caller
  code, the design is probably wrong

### Data availability and semantics

Transport is abstracted. Data semantics are not.

Integrators must check and document:

- whether the provider can produce equivalent data on Linux and Windows
- whether the same field means the same thing on both operating systems
- whether one platform has weaker identifiers, weaker timing precision, or
  missing counters
- whether a consumer should:
  - degrade gracefully
  - expose partial capability
  - or reject the integration on the weaker platform

Do not assume that because the IPC layer is cross-platform, the provider's data
model is automatically cross-platform too.

## Anti-Patterns

Avoid these:

- using L1 directly in ordinary plugin business logic when L2 or L3 fits
- designing non-idempotent handlers without accounting for at-least-once retry
- assuming the provider exists at startup
- assuming SHM will always be used
- assuming public L2/L3 status exposes transport-profile proof
- doing cache rebuild logic in every consumer separately when the pattern is a
  reusable L3 helper
- exposing raw payload buffers or transport scratch to plugin-facing callers
- adding a generic multi-method public service facade instead of one service
  kind per endpoint

## Minimal Decision Tree

Use this when starting new work:

1. Do I need direct transport control or malformed-envelope testing?
   - yes: use L1
   - no: continue
2. Do I need typed request/response only?
   - yes: use L2
   - no: continue
3. Do I need periodic snapshot refresh plus hot-path local lookup?
   - yes: use L3
4. Is there already a public typed service for this?
   - yes: integrate it
   - no: extend codec + raw helpers + public typed facade

5. Is my consumer on-demand but about to render many rows?
   - yes: refresh once, then use local lookup
   - if the provider publishes a finite dataset, that usually means L3

## Assistant Execution Checklist

When an assistant is asked to add a new Netdata integration using this library,
it should:

1. Identify the producer plugin and consumer plugin(s).
2. Decide whether the job is L1, L2, or L3.
3. Verify whether an existing typed service already solves it.
4. If not, define the typed service contract first.
5. Extend the structured codec and typed dispatch in C, Rust, and Go in
   `github.com/netdata/plugin-ipc`.
   - Do not stop after editing only one language.
   - Do not wire only the vendored copy in Netdata and defer upstream work.
6. Add the public typed facade, not just raw helpers.
7. Add L3 only if the dataset needs local owned cache + lookup.
8. Add tests for codec, L2, interop, and L3 if applicable.
9. Add operational logs at the plugin boundary.
10. Validate Linux and Windows behavior before claiming the integration is ready.
11. State the provider synchronization model explicitly.
12. State the client/cache ownership model explicitly.
13. Verify view/reference validity boundaries before sharing any returned data.
14. Check platform-specific data availability before finalizing the schema.

## Do Not Miss These

Before implementation is accepted, confirm all of these explicitly:

- The library constraints and assumptions were read and followed.
- The service remains payload-agnostic at the skill level; plugin-specific
  schema content was decided separately by the integrator.
- The upstream-vs-Netdata ownership split is clear.
- The provider's shared mutable data has an explicit synchronization model.
- The client or cache object has an explicit ownership/concurrency model.
- No code performs concurrent `refresh()` / `lookup()` / typed calls on the same
  mutable object without external synchronization.
- No response view or cache item reference is held past its validity boundary.
- Startup ordering, retry states, and degradation behavior are documented.
- Platform-specific data availability and semantic differences were checked.
- Transport observability, if needed, is logged at the integration boundary.

## Final Rule

The library exists to keep Netdata plugin integrations:

- typed
- service-oriented
- transport-agnostic at the caller boundary
- resilient to startup ordering and provider restarts
- supportable in production

If a proposed integration makes plugin business logic reason about transport
internals, raw payload bytes, plugin identity, or exactly-once semantics, stop
and redesign it.

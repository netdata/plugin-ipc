# Code Organization Guide

## Purpose

This document defines how the plugin-ipc codebase is structured, how to
add new transports, message types, and snapshot helpers, and how to
preserve separation of concerns between the architectural layers.

This guide is derived from the Level 1, Codec, Level 2, and Level 3
specifications. It is not an independent design — it enforces the
boundaries those specs define.

## Repository layout

The repository mirrors Netdata's destination structure so that future
integration requires minimal structural changes:

```
src/
  libnetdata/netipc/          # C library
    include/netipc/           # C public headers
    src/
      protocol/               # Codec: wire format encode/decode/builders
      transport/
        posix/                # L1: UDS SEQPACKET, POSIX SHM
        windows/              # L1: Named Pipe, Windows SHM
      service/                # L2/L3: typed client/server helpers

  crates/netipc/              # Rust library (Cargo crate)
    src/
      protocol.rs             # Codec
      transport/
        posix.rs              # L1: UDS, SHM
        windows.rs            # L1: Named Pipe, SHM
      service/                # L2/L3: typed client/server helpers

  go/pkg/netipc/              # Go library (Go package)
    protocol/                 # Codec
    transport/
      posix/                  # L1: UDS, SHM
      windows/                # L1: Named Pipe, SHM
    service/                  # L2/L3: typed client/server helpers

tests/
  fixtures/                   # Helper binaries for live testing
    c/
    rust/
    go/
  *.sh                        # Validation and smoke scripts

bench/
  drivers/                    # Benchmark helper binaries

docs/                         # Specifications (this directory)
```

## Module boundaries

### Codec modules (`protocol/`)

Codec modules contain:

- Wire format constants (method codes, layout versions, sizes)
- Encode functions (typed structure to payload bytes)
- Decode functions (payload bytes to ephemeral view)
- Response builders
- Copy/materialize helpers

Codec modules must NOT contain:

- Any transport or I/O code
- Any connection or session state
- Any Level 2 or Level 3 logic
- Any OS-specific code (codec is platform-independent)

Codec modules must NOT import transport modules.

### Transport modules (`transport/`)

Transport modules contain:

- Connection lifecycle (connect, listen, accept, close)
- Handshake implementation (auth, profile negotiation, limits)
- Message send/receive (outer envelope framing)
- Batch assembly/extraction (item directory management)
- Chunking (transparent splitting/reassembly)
- Sequencing and message_id tracking
- Transport-specific mechanics (socket ops, pipe ops, SHM regions)
- Native wait-object exposure

Transport modules must NOT contain:

- Typed method knowledge (no method-specific encode/decode)
- Callback dispatch or handler registration
- Retry or reconnect policy
- Cache or snapshot logic

Transport modules import the protocol module for L1 wire primitives
(header encode/decode, chunk header, batch directory validation) but
must NOT use method-specific codec functions. Level 1 treats all
method payloads as opaque bytes.

### Service modules (`service/`)

Service modules contain:

- Level 2: typed client contexts, managed server mode, retry policy,
  callback dispatch, batch-to-individual-item orchestration
- Level 3: snapshot refresh helpers, local cache management, lookup
  functions

Service modules import both transport and codec modules. They compose
Level 1 + Codec into the convenience surface.

Service modules must NOT contain:

- Transport implementation details (no direct socket/pipe/SHM code)
- Wire format encoding/decoding (that belongs in codec)

## Separation rules

### Protocol module is shared infrastructure

The protocol module (`protocol/`) contains both L1 wire primitives
(header, chunk header, batch directory) and method-specific codec
functions. Transport modules import the protocol module for L1 wire
primitives only. They must not call method-specific codec functions.
Codec modules never call transport functions.

### No upward dependencies

- Protocol depends on nothing
- Level 1 depends on protocol (wire primitives only)
- Level 2 depends on Level 1 + protocol (wire + method codecs)
- Level 3 depends on Level 2

No module may depend on a higher layer. Transport code must never import
service code. Codec code must never import service code.

### One transport implementation is the source of truth

High-level managed server mode (Level 2) is a thin wrapper over Level 1
listener/session primitives. It must not reimplement transport, framing,
or negotiation logic. If the managed server needs a transport capability,
that capability must exist as a Level 1 primitive.

### Service modules are strictly layered

Level 3 snapshot helpers must use Level 2 typed call functions. They must
not bypass Level 2 to call Level 1 directly. This ensures that retry
policy, connection management, and typed dispatch remain consistent.

## How to add a new transport

1. Add the transport implementation under `transport/<platform>/` in each
   language.
2. The transport must implement the Level 1 transport interface: connect,
   listen, accept, send, receive, close, wait-object exposure.
3. The transport must support the handshake protocol (auth, profile
   negotiation, directional limits, packet size).
4. The transport must handle transparent chunking if it has packet size
   limits.
5. Register the transport profile in the negotiation bitmask.
6. Add the transport contract document under `docs/level1-<name>.md`.
7. Add tests: unit, interop (all language pairs), fuzz (header/chunk
   parsing), abnormal path (disconnect, stale recovery).
8. No codec or service module changes are needed — transports carry
   opaque bytes.

## How to add a new message type

1. Define the wire layout contract in a new `docs/codec-<name>.md` file:
   method code, request payload layout, response payload layout, result
   codes, validation rules.
2. Add the encode/decode/builder functions in the codec module
   (`protocol/`) of each language (C, Rust, Go).
3. These codec functions are immediately available to Level 1 integrators.
4. Add Level 2 typed wrappers in the service module if convenience
   client/server support is needed.
   Public Level 2 client calls must accept and return typed values or
   typed views only. Public Level 2 server APIs must register typed
   callbacks only. Any request/response scratch buffers remain internal
   to Level 2.
5. Add tests: round-trip (all languages), cross-language interop (all
   pairs), fuzz (decode), boundary, validation rejection.
6. No transport module changes are needed — the new message type uses
   the existing Level 1 send/receive with opaque payloads.

## How to add a Level 3 snapshot helper

1. The snapshot helper sits in the service module, built on Level 2
   typed calls.
2. It manages: periodic refresh, local cache construction, lookup by
   key, cache preservation on refresh failure.
3. It must not bypass Level 2 for transport access.
4. Add the helper specification in `docs/level3-<name>.md`.
5. Add tests: refresh success, refresh failure with cache preservation,
   reconnect after failure, lookup correctness, empty cache behavior,
   large dataset behavior.

## File size discipline

Transport and protocol modules must not grow into monolithic files that
mix multiple concerns. Guidelines:

- Separate client and server types into different files where the
  language supports it.
- Separate SHM transport from baseline transport into distinct files.
- Keep handshake/negotiation logic isolated from data-plane send/receive.
- Keep tests in dedicated test files (or test modules in Rust).

If a single file exceeds ~500 lines, consider whether it mixes concerns
that should be separated.

## Build system

CMake is the top-level orchestrator. Language-native manifests remain
for their respective ecosystems:

- `CMakeLists.txt`: top-level build, test registration, benchmark targets
- `Cargo.toml`: Rust crate metadata (used by CMake via cargo invocation)
- `go.mod`: Go module metadata (used by CMake via go invocation)

All validation and benchmark workflows must be runnable through CMake
targets. Shell scripts remain thin orchestrators that CMake invokes.

## Cross-language consistency

All three language implementations must:

- Produce identical wire bytes for the same typed input
- Accept identical wire bytes and produce equivalent typed views
- Use the same method codes, layout versions, and field offsets
- Apply the same validation rules during decoding
- Use the same negotiation bitmasks and profile constants
- Pass the same cross-language interop test suite

The wire contract documents in `docs/` are the authority. If two
implementations produce different bytes, at least one is wrong — check
it against the spec.

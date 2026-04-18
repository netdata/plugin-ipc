# plugin-ipc Specification

These documents are the authoritative specification for the plugin-ipc
library. Implementation must align with them. When code and spec disagree,
the spec is right unless explicitly revised.

## Architecture

The library is organized into four independent concerns:

```
┌─────────────────────────────────────────────┐
│              Level 3: Snapshot               │
│    Refresh, caching, lookup helpers          │
│            (built on Level 2)                │
├─────────────────────────────────────────────┤
│           Level 2: Orchestration             │
│   Client context, managed server, retry      │
│          (built on Level 1 + Codec)          │
├──────────────────────┬──────────────────────┤
│    Level 1:          │       Codec:          │
│    Transport         │   Wire ↔ Typed        │
│                      │                       │
│  Connections, send,  │  Encode, decode,      │
│  receive, framing,   │  builders, views.     │
│  chunking, batching, │  Pure bytes. No I/O.  │
│  pipelining, auth,   │  No transport.        │
│  sequencing, SHM.    │                       │
└──────────────────────┴──────────────────────┘
```

- **Level 1** and **Codec** are parallel — neither depends on the other.
- **Level 2** depends on both Level 1 and Codec.
- **Level 3** depends on Level 2.
- Each message type adds Codec functions that are usable from Level 1
  directly, without Level 2.

## Service-Oriented Discovery

The public identity model is service-oriented:

- clients connect to `service_namespace + service_name`
- clients do not bind to plugin/process identity
- service names are the stable public contract
- one service endpoint serves one request kind only

Examples of service kinds:

- `cgroups-snapshot`
- `ip-to-asn`
- `pid-traffic`

Runtime model:

- providers may start late
- providers may restart or disappear
- enrichments are optional
- clients must tolerate absence and reconnect from their own loop

## Documents

### Level 1: Transport

| Document | Description |
|----------|-------------|
| [level1-transport.md](level1-transport.md) | Principles, features, capabilities, and operational semantics of the transport layer. |
| [level1-wire-envelope.md](level1-wire-envelope.md) | Common message framing: 32-byte outer header, batch item directory, chunk continuation header, handshake payloads. Shared by all transports. |
| [level1-posix-uds.md](level1-posix-uds.md) | UDS SEQPACKET transport contract: socket path derivation, packet boundaries, chunking trigger. |
| [level1-posix-shm.md](level1-posix-shm.md) | POSIX SHM interop contract: shared region layout, control header, futex synchronization protocol, region lifecycle. |
| [level1-windows-np.md](level1-windows-np.md) | Named Pipe transport contract: pipe name derivation, message mode, chunking trigger. |
| [level1-windows-shm.md](level1-windows-shm.md) | Windows SHM interop contract: file mapping layout, control header, synchronization protocol, region lifecycle. |

### Codec

| Document | Description |
|----------|-------------|
| [codec.md](codec.md) | Principles, encode/decode contracts, builder contract, view lifetime rules, naming discipline. |
| [codec-cgroups-snapshot.md](codec-cgroups-snapshot.md) | cgroups snapshot message type: request/response wire layout, field definitions, validation rules. |

### Level 2: Typed Convenience API

| Document | Description |
|----------|-------------|
| [level2-typed-api.md](level2-typed-api.md) | Orchestration layer: client context lifecycle, managed server mode, retry policy, typed callback dispatch. |
| [netipc-integrator-skill.md](netipc-integrator-skill.md) | Practical integration guide for adding new Netdata clients, servers, and typed services using L1/L2/L3 across C, Rust, and Go on Linux and Windows. |

### Level 3: Snapshot API

| Document | Description |
|----------|-------------|
| [level3-snapshot-api.md](level3-snapshot-api.md) | Snapshot refresh, local cache management, lookup helpers. Built on Level 2. |

### Code Organization

| Document | Description |
|----------|-------------|
| [code-organization.md](code-organization.md) | How to add transports, message types, and snapshot helpers. File layout, module boundaries, separation of concerns. |

## Languages

All specifications must be implemented in:

- **C** (POSIX and Windows/MSYS)
- **Rust** (POSIX and Windows native)
- **Go** (POSIX and Windows native, pure Go, no cgo)

Cross-language interoperability is mandatory. The wire contracts in these
specifications are the authority for interop correctness.

## Testing

Every spec document includes its own testing requirements section. The
universal rules are:

- High test coverage with enforced minimums (90%+ line coverage
  on POSIX via `run-coverage-{c,rust,go}.sh`; Windows coverage is
  verified via unit tests and cross-language interop, not lcov/llvm-cov)
- Fuzz testing for all parsing/decoding paths
- Cross-language interop tests for all wire contracts, over both
  baseline and SHM transports
- Abnormal path coverage for all failure modes
- No exceptions — nothing integrates into Netdata without this coverage

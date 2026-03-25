# Getting Started

Quick guide to using the plugin-ipc library from C, Rust, and Go.

For full specifications, see the other docs in this directory:
`codec.md`, `level1-*.md`, `level2-typed-api.md`, `level3-snapshot-api.md`.

## Architecture Overview

The library has four layers:

- **Codec** (`protocol/`): wire format encode/decode. Platform-independent.
- **Level 1** (`transport/`): connection lifecycle, send/receive, chunking,
  handshake. Treats payloads as opaque bytes.
- **Level 2** (`service/`): typed client context with retry, managed server
  with multi-client worker dispatch. Composes L1 + Codec.
- **Level 3** (`service/`): snapshot cache with O(1) hash lookup, periodic
  refresh, cache preservation on failure. Built on L2.

Transports: UDS + SHM (POSIX/Linux), Named Pipe + Win SHM (Windows).
SHM upgrade is negotiated during handshake and transparent to L2/L3.

The Level 2 examples below are schematic. They show the intended public
API shape from the specifications:

- Clients use typed calls only
- Servers expose one service kind only
- Request/response scratch buffers remain internal to the library

## Service-Oriented Discovery

Clients connect to services, not plugins.

- the client knows `service_namespace + service_name`
- the client does not know which plugin/process implements that service
- one service endpoint serves one request kind only

Examples:

- `cgroups-snapshot`
- `ip-to-asn`
- `pid-traffic`

Runtime expectations:

- plugins may start in any order
- a client may start before the service exists
- a provider may restart or disappear
- optional enrichments must not break the caller
- clients should call `Refresh()` periodically and tolerate temporary absence

## Client (L2 typed calls)

### C

```c
#include "netipc/netipc_service.h"

nipc_uds_client_config_t cfg = {
    .supported_profiles        = NIPC_PROFILE_BASELINE,
    .max_request_payload_bytes = 4096,
    .max_request_batch_items   = 1,
    .max_response_payload_bytes = 65536,
    .max_response_batch_items  = 1,
    .auth_token                = 0xDEADBEEFCAFEBABE,
};

nipc_client_ctx_t client;
nipc_client_init(&client, "/run/netdata", "cgroups-snapshot", &cfg);
nipc_client_refresh(&client);  /* attempt connect */

if (nipc_client_ready(&client)) {
    nipc_cgroups_resp_view_t view;
    nipc_error_t err = nipc_client_call_cgroups_snapshot(&client, &view);

    if (err == NIPC_OK) {
        /* view.item_count, view.generation, etc. */
        for (uint32_t i = 0; i < view.item_count; i++) {
            nipc_cgroups_item_view_t item;
            nipc_cgroups_resp_item(&view, i, &item);
            /* item.hash, item.name.ptr/len, item.path.ptr/len */
        }
        /* view is valid until the next typed call on this client */
    }
}

nipc_client_close(&client);
```

### Rust

```rust
use netipc::service::cgroups::{CgroupsClient, ClientConfig};
use netipc::protocol::PROFILE_BASELINE;

let config = ClientConfig {
    supported_profiles: PROFILE_BASELINE,
    max_request_payload_bytes: 4096,
    max_request_batch_items: 1,
    max_response_payload_bytes: 65536,
    max_response_batch_items: 1,
    auth_token: 0xDEADBEEFCAFEBABE,
    ..ClientConfig::default()
};

let mut client = CgroupsClient::new("/run/netdata", "cgroups-snapshot", config);
client.refresh();  // attempt connect

if client.ready() {
    match client.call_snapshot() {
        Ok(view) => {
            for i in 0..view.item_count {
                let item = view.item(i).unwrap();
                // item.hash, item.name.as_str(), item.path.as_str()
            }
            // view is valid until the next typed call on this client
        }
        Err(e) => eprintln!("call failed: {:?}", e),
    }
}

client.close();
```

### Go

```go
import "github.com/netdata/plugin-ipc/go/pkg/netipc/service/cgroups"
import "github.com/netdata/plugin-ipc/go/pkg/netipc/transport/posix"

config := posix.ClientConfig{
    SupportedProfiles:       posix.ProfileBaseline,
    MaxRequestPayloadBytes:  4096,
    MaxRequestBatchItems:    1,
    MaxResponsePayloadBytes: 65536,
    MaxResponseBatchItems:   1,
    AuthToken:               0xDEADBEEFCAFEBABE,
}

client := cgroups.NewClient("/run/netdata", "cgroups-snapshot", config)
defer client.Close()

client.Refresh() // attempt connect

if client.Ready() {
    view, err := client.CallSnapshot()
    if err == nil {
        for i := uint32(0); i < view.ItemCount; i++ {
            item, _ := view.Item(i)
            // item.Hash, item.Name, item.Path
        }
        // view is valid until the next typed call on this client
    }
}
```

## Managed Server (L2)

The managed server receives wire messages, but that is internal to the
library. The public L2 server contract is typed:

- You expose one service kind only
- Callbacks receive decoded request data
- Callbacks return typed response data or fill typed builders
- User code never switches on plugin identity, `method_code`, or raw payload
  bytes or raw response buffers

### C

```c
#include "netipc/netipc_service.h"

bool on_cgroups(void *user, const nipc_cgroups_req_t *req,
                nipc_cgroups_builder_t *builder) {
    /* Fill builder with cgroup items */
    return true;
}

/* Public L2 shape: one service kind, one typed handler surface */
nipc_cgroups_service_handler_t service_handler = {
    .handle = on_cgroups,
    .user                = NULL,
};

nipc_uds_server_config_t scfg = {
    .supported_profiles        = NIPC_PROFILE_BASELINE,
    .max_request_payload_bytes = 4096,
    .max_request_batch_items   = 1,
    .max_response_payload_bytes = 65536,
    .max_response_batch_items  = 1,
    .auth_token                = 0xDEADBEEFCAFEBABE,
    .backlog                   = 16,
};

nipc_managed_server_t server;
nipc_server_init_typed(&server, "/run/netdata", "cgroups-snapshot", &scfg,
                       4, &service_handler);

/* Blocking: accepts clients, performs typed dispatch internally */
nipc_server_run(&server);

/* From another thread: */
nipc_server_stop(&server);
/* Or graceful drain (waits up to 5s for in-flight requests): */
nipc_server_drain(&server, 5000);

nipc_server_destroy(&server);
```

### Rust

```rust
use netipc::service::cgroups::{Handler, ManagedServer, ServerConfig};

let handler = Handler {
    handle: Some(Box::new(|req, builder| {
        // Fill builder with cgroup items
        true
    })),
    ..Default::default()
};

let config = ServerConfig { /* ... */ };
let mut server = ManagedServer::new(
    "/run/netdata", "cgroups-snapshot", config, handler);

server.run().unwrap();
server.stop();
```

### Go

```go
handler := cgroups.Handler{
    Handle: func(req protocol.CgroupsRequestView, builder *protocol.CgroupsBuilder) bool {
        // Fill builder with cgroup items
        return true
    },
}

config := posix.ServerConfig{ /* ... */ }
server := cgroups.NewServerWithWorkers(
    "/run/netdata", "cgroups-snapshot", config, handler, 4)

go server.Run()
server.Stop()
```

## L3 Cache (snapshot with hash lookup)

The L3 cache wraps an L2 client, maintains a local owned copy of the
latest snapshot, and provides O(1) lookup by hash + name.

### C

```c
nipc_cgroups_cache_t cache;
nipc_uds_client_config_t ccfg = { /* ... */ };
nipc_cgroups_cache_init(&cache, "/run/netdata", "cgroups-snapshot", &ccfg);

/* Call periodically from your loop */
if (nipc_cgroups_cache_refresh(&cache)) {
    printf("cache updated, %u items\n", cache.item_count);
}

/* O(1) lookup (hash table, no I/O) */
const nipc_cgroups_cache_item_t *item =
    nipc_cgroups_cache_lookup(&cache, hash, "docker-abc123");
if (item) {
    /* item->name, item->path, item->enabled, item->options */
}

nipc_cgroups_cache_close(&cache);
```

### Rust

```rust
use netipc::service::cgroups::CgroupsCache;

let mut cache = CgroupsCache::new("/run/netdata", "cgroups-snapshot", config);

// Call periodically
if cache.refresh() {
    println!("cache updated");
}

// O(1) lookup
if let Some(item) = cache.lookup(hash, "docker-abc123") {
    println!("{}: {}", item.name, item.path);
}

cache.close();
```

### Go

```go
cache := cgroups.NewCache("/run/netdata", "cgroups-snapshot", config)
defer cache.Close()

// Call periodically
if cache.Refresh() {
    fmt.Println("cache updated")
}

// O(1) lookup
if item, found := cache.Lookup(hash, "docker-abc123"); found {
    fmt.Printf("%s: %s\n", item.Name, item.Path)
}
```

## Key Design Points

- **Caller-driven**: no hidden threads, no timers. You call `Refresh()`
  from your own loop at your own cadence.
- **Service-oriented**: the client binds to a service kind, not to a plugin.
- **One endpoint, one request kind**: a `cgroups-snapshot` server serves
  snapshot requests only.
- **Asynchronous startup**: providers may appear late or disappear. Callers
  must tolerate absence and reconnect through their normal loop.
- **At-least-once retry**: if a call fails and the client was READY,
  it automatically disconnects, reconnects (full handshake), and
  retries once before returning an error.
- **Cache preservation**: on refresh failure, the previous cache is
  preserved. The cache is empty only if no refresh has ever succeeded.
- **Transport negotiation**: SHM is negotiated during handshake and
  used transparently if both sides support it.
- **Cross-language**: all three implementations produce identical wire
  bytes and pass cross-language interop tests.

## Building

```bash
# Full build + tests
mkdir build && cd build
cmake ..
cmake --build .
ctest

# Rust
cd src/crates/netipc && cargo test

# Go
cd src/go && go test ./...
```

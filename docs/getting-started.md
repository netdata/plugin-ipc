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
nipc_client_init(&client, "/run/netdata", "cgroups", &cfg);
nipc_client_refresh(&client);  /* attempt connect */

if (nipc_client_ready(&client)) {
    uint8_t req_buf[64], resp_buf[65536];
    nipc_cgroups_resp_view_t view;
    nipc_error_t err = nipc_client_call_cgroups_snapshot(
        &client, req_buf, resp_buf, sizeof(resp_buf), &view);

    if (err == NIPC_OK) {
        /* view.item_count, view.generation, etc. */
        for (uint32_t i = 0; i < view.item_count; i++) {
            nipc_cgroups_item_view_t item;
            nipc_cgroups_resp_item(&view, i, &item);
            /* item.hash, item.name.ptr/len, item.path.ptr/len */
        }
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

let mut client = CgroupsClient::new("/run/netdata", "cgroups", config);
client.refresh();  // attempt connect

if client.ready() {
    let mut resp_buf = vec![0u8; 65536];
    match client.call_snapshot(&mut resp_buf) {
        Ok(view) => {
            for i in 0..view.item_count {
                let item = view.item(i).unwrap();
                // item.hash, item.name.as_str(), item.path.as_str()
            }
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

client := cgroups.NewClient("/run/netdata", "cgroups", config)
defer client.Close()

client.Refresh() // attempt connect

if client.Ready() {
    responseBuf := make([]byte, 65536)
    view, err := client.CallSnapshot(responseBuf)
    if err == nil {
        for i := uint32(0); i < view.ItemCount; i++ {
            item, _ := view.Item(i)
            // item.Hash, item.Name, item.Path
        }
    }
}
```

## Managed Server (L2)

The server handler dispatches by method code and delegates to typed
dispatch helpers. Typed handlers receive decoded data and never touch
wire format.

### C

```c
#include "netipc/netipc_service.h"

/* Typed business-logic handlers — pure logic, no wire format */
bool on_increment(void *user, uint64_t req, uint64_t *resp) {
    *resp = req + 1;
    return true;
}

bool on_cgroups(void *user, const nipc_cgroups_req_t *req,
                nipc_cgroups_builder_t *builder) {
    /* Fill builder with cgroup items */
    return true;
}

/* Raw handler dispatches to typed helpers */
bool my_handler(void *user, uint16_t method_code,
                const uint8_t *req, size_t req_len,
                uint8_t *resp, size_t resp_size,
                size_t *resp_len_out) {
    switch (method_code) {
    case NIPC_METHOD_INCREMENT:
        return nipc_dispatch_increment(req, req_len, resp, resp_size,
                                       resp_len_out, on_increment, user);
    default:
        return false;
    }
}

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
nipc_server_init(&server, "/run/netdata", "cgroups", &scfg,
                 4,       /* max concurrent sessions */
                 65536,   /* per-session response buffer */
                 my_handler, NULL);

/* Blocking: accepts clients, dispatches to handler threads */
nipc_server_run(&server);

/* From another thread: */
nipc_server_stop(&server);
/* Or graceful drain (waits up to 5s for in-flight requests): */
nipc_server_drain(&server, 5000);

nipc_server_destroy(&server);
```

### Rust

```rust
use netipc::service::cgroups::{CgroupsServer, ServerConfig};
use netipc::protocol;

let handler = Arc::new(|method: u16, req: &[u8]| -> Option<Vec<u8>> {
    match method {
        protocol::METHOD_INCREMENT => {
            protocol::dispatch_increment(req, &mut resp, |v| Some(v + 1))
                .map(|n| resp[..n].to_vec())
        }
        _ => None,
    }
});

let config = ServerConfig { /* ... */ };
let mut server = CgroupsServer::new(
    "/run/netdata", "cgroups", config, 65536, handler);

server.run().unwrap();
server.stop();
```

### Go

```go
handler := func(methodCode uint16, req []byte) ([]byte, bool) {
    switch methodCode {
    case protocol.MethodIncrement:
        resp := make([]byte, 8)
        n, ok := protocol.DispatchIncrement(req, resp,
            func(v uint64) (uint64, bool) { return v + 1, true })
        if !ok { return nil, false }
        return resp[:n], true
    default:
        return nil, false
    }
}

config := posix.ServerConfig{ /* ... */ }
server := cgroups.NewServerWithWorkers(
    "/run/netdata", "cgroups", config, handler, 4)

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
nipc_cgroups_cache_init(&cache, "/run/netdata", "cgroups", &ccfg);

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

let mut cache = CgroupsCache::new("/run/netdata", "cgroups", config);

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
cache := cgroups.NewCache("/run/netdata", "cgroups", config)
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

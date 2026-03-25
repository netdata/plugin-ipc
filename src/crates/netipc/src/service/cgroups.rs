//! L2 cgroups-snapshot service facade.
//!
//! The public service surface is service-kind specific: one endpoint, one
//! request kind. The request code remains in the outer envelope only for
//! validation, not for public multi-method dispatch.

use super::raw;
use crate::protocol::{CgroupsResponseView, NipcError, METHOD_CGROUPS_SNAPSHOT};

#[cfg(unix)]
use crate::transport::posix::{ClientConfig, ServerConfig};

#[cfg(windows)]
use crate::transport::windows::{ClientConfig, ServerConfig};

use std::sync::atomic::AtomicBool;
use std::sync::Arc;

pub use raw::{CgroupsCacheItem, CgroupsCacheStatus, ClientState, ClientStatus, SnapshotHandler};

/// L2 client context for the cgroups-snapshot service.
pub struct CgroupsClient {
    inner: raw::RawClient,
}

impl CgroupsClient {
    /// Create a new client context. Does NOT connect. Does NOT require the
    /// server to be running.
    pub fn new(run_dir: &str, service_name: &str, config: ClientConfig) -> Self {
        Self {
            inner: raw::RawClient::new_snapshot(run_dir, service_name, config),
        }
    }

    /// Attempt connect if DISCONNECTED/NOT_FOUND, reconnect if BROKEN.
    /// Returns true if the state changed.
    pub fn refresh(&mut self) -> bool {
        self.inner.refresh()
    }

    /// Cheap cached boolean. No I/O, no syscalls.
    #[inline]
    pub fn ready(&self) -> bool {
        self.inner.ready()
    }

    /// Detailed status snapshot for diagnostics.
    pub fn status(&self) -> ClientStatus {
        self.inner.status()
    }

    /// Blocking typed call for the cgroups-snapshot service.
    pub fn call_snapshot(&mut self) -> Result<CgroupsResponseView<'_>, NipcError> {
        self.inner.call_snapshot()
    }

    /// Tear down connection and release resources.
    pub fn close(&mut self) {
        self.inner.close();
    }
}

impl Drop for CgroupsClient {
    fn drop(&mut self) {
        self.close();
    }
}

/// Typed server handler surface for the cgroups-snapshot service.
#[derive(Clone, Default)]
pub struct Handler {
    pub handle: Option<SnapshotHandler>,
    pub snapshot_max_items: u32,
}

/// Managed server for the cgroups-snapshot service kind.
pub struct ManagedServer {
    inner: raw::ManagedServer,
}

impl ManagedServer {
    /// Create a new managed server. Does NOT start listening yet.
    pub fn new(run_dir: &str, service_name: &str, config: ServerConfig, handler: Handler) -> Self {
        Self::with_workers(run_dir, service_name, config, handler, 8)
    }

    /// Create a server with an explicit worker count limit.
    pub fn with_workers(
        run_dir: &str,
        service_name: &str,
        config: ServerConfig,
        handler: Handler,
        worker_count: usize,
    ) -> Self {
        let raw_handler = handler
            .handle
            .map(|handle| raw::snapshot_dispatch(handle, handler.snapshot_max_items));

        Self {
            inner: raw::ManagedServer::with_workers(
                run_dir,
                service_name,
                config,
                METHOD_CGROUPS_SNAPSHOT,
                raw_handler,
                worker_count,
            ),
        }
    }

    /// Run the acceptor loop. Blocking. Returns when `stop()` is called or on
    /// fatal error.
    pub fn run(&mut self) -> Result<(), NipcError> {
        self.inner.run()
    }

    /// Signal shutdown.
    pub fn stop(&self) {
        self.inner.stop();
    }

    /// Clone of the internal running flag for diagnostics and test helpers.
    ///
    /// For reliable shutdown, call `stop()`. On Windows, changing this flag
    /// alone does not wake a blocking listener accept.
    pub fn running_flag(&self) -> Arc<AtomicBool> {
        self.inner.running_flag()
    }
}

/// L3 client-side cgroups snapshot cache.
pub struct CgroupsCache {
    inner: raw::CgroupsCache,
}

impl CgroupsCache {
    /// Create a new L3 cache. Creates the underlying L2 client context.
    /// Does NOT connect. Does NOT require the server to be running.
    pub fn new(run_dir: &str, service_name: &str, config: ClientConfig) -> Self {
        Self {
            inner: raw::CgroupsCache::new(run_dir, service_name, config),
        }
    }

    /// Refresh the cache. Returns true if the cache was updated.
    pub fn refresh(&mut self) -> bool {
        self.inner.refresh()
    }

    /// Returns true if at least one successful refresh has occurred.
    #[inline]
    pub fn ready(&self) -> bool {
        self.inner.ready()
    }

    /// Look up a cached item by hash + name. O(1), no I/O.
    pub fn lookup(&self, hash: u32, name: &str) -> Option<&CgroupsCacheItem> {
        self.inner.lookup(hash, name)
    }

    /// Fill a status snapshot for diagnostics.
    pub fn status(&self) -> CgroupsCacheStatus {
        self.inner.status()
    }

    /// Close the cache and underlying L2 client.
    pub fn close(&mut self) {
        self.inner.close();
    }
}

impl Drop for CgroupsCache {
    fn drop(&mut self) {
        self.close();
    }
}

#[cfg(all(test, unix))]
#[path = "cgroups_unix_tests.rs"]
mod tests;

#[cfg(all(test, windows))]
#[path = "cgroups_windows_tests.rs"]
mod windows_tests;

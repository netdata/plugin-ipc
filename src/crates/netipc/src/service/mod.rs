//! L2 orchestration: client context and managed server.
//!
//! Pure convenience layer. Uses L1 transport + Codec exclusively.
//! Adds zero wire behavior. Provides lifecycle management, typed calls,
//! and a multi-client server with handler dispatch.

pub mod cgroups;

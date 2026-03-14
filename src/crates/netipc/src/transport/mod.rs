//! L1 transport backends.
//!
//! Each transport provides connection lifecycle, handshake, and
//! send/receive with transparent chunking.

#[cfg(unix)]
pub mod posix;

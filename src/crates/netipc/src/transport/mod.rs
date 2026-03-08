//! Transport implementations for the Rust crate.

#[cfg(unix)]
pub mod posix;
#[cfg(windows)]
pub mod windows;

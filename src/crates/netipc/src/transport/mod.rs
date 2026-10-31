//! L1 transport backends.
//!
//! Each transport provides connection lifecycle, handshake, and
//! send/receive with transparent chunking.

#[cfg(unix)]
pub mod posix;

#[cfg(target_os = "linux")]
pub mod shm;

#[cfg(windows)]
pub mod windows;

#[cfg(windows)]
pub mod win_shm;

#[cfg(windows)]
pub(crate) mod win_security;

/// True when a HELLO_ACK selected exactly one of the profiles a client offered.
pub(crate) fn selected_profile_offered(selected: u32, offered: u32) -> bool {
    selected.is_power_of_two() && selected & offered != 0
}

//! Netdata-aligned Rust crate for plugin IPC.
//!
//! The reusable crate lives here. Test fixtures and benchmark drivers live
//! outside `src/crates/` so the library surface stays clean.

pub mod protocol;
pub mod transport;

pub use protocol::{
    decode_increment_request, decode_increment_response, encode_increment_request,
    encode_increment_response, Frame, IncrementRequest, IncrementResponse, FRAME_SIZE,
    STATUS_BAD_REQUEST, STATUS_INTERNAL_ERROR, STATUS_OK,
};

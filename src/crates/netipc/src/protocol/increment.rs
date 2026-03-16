//! INCREMENT codec (method 1) -- 8-byte payload: { u64 value }

use super::NipcError;

pub const INCREMENT_PAYLOAD_SIZE: usize = 8;

pub fn increment_encode(value: u64, buf: &mut [u8]) -> usize {
    if buf.len() < INCREMENT_PAYLOAD_SIZE { return 0; }
    buf[..8].copy_from_slice(&value.to_ne_bytes());
    INCREMENT_PAYLOAD_SIZE
}

pub fn increment_decode(buf: &[u8]) -> Result<u64, NipcError> {
    if buf.len() < INCREMENT_PAYLOAD_SIZE { return Err(NipcError::Truncated); }
    Ok(u64::from_ne_bytes(buf[..8].try_into().unwrap()))
}

/// INCREMENT dispatch: decode -> handler -> encode.
pub fn dispatch_increment<F>(req: &[u8], resp: &mut [u8], handler: F) -> Option<usize>
where
    F: FnOnce(u64) -> Option<u64>,
{
    let value = increment_decode(req).ok()?;
    let result = handler(value)?;
    let n = increment_encode(result, resp);
    if n == 0 {
        return None;
    }
    Some(n)
}

//! STRING_REVERSE codec (method 3) -- variable-length payload:
//!   [0:4] u32 str_offset (from payload start, always 8)
//!   [4:8] u32 str_length (excluding NUL)
//!   [8:N+1] string data + NUL

use super::NipcError;

pub const STRING_REVERSE_HDR_SIZE: usize = 8;

/// Ephemeral view into a decoded STRING_REVERSE payload.
#[derive(Debug, Clone)]
pub struct StringReverseView<'a> {
    pub str_data: &'a [u8],  // slice into payload, NUL-terminated
    pub str_len: u32,
}

impl<'a> StringReverseView<'a> {
    pub fn as_str(&self) -> &'a str {
        std::str::from_utf8(self.str_data).unwrap_or("")
    }
}

pub fn string_reverse_encode(s: &[u8], buf: &mut [u8]) -> usize {
    let total = STRING_REVERSE_HDR_SIZE + s.len() + 1;
    if buf.len() < total { return 0; }
    let offset: u32 = STRING_REVERSE_HDR_SIZE as u32;
    let length: u32 = s.len() as u32;
    buf[0..4].copy_from_slice(&offset.to_ne_bytes());
    buf[4..8].copy_from_slice(&length.to_ne_bytes());
    if !s.is_empty() {
        buf[8..8 + s.len()].copy_from_slice(s);
    }
    buf[8 + s.len()] = 0; // NUL
    total
}

pub fn string_reverse_decode(buf: &[u8]) -> Result<StringReverseView<'_>, NipcError> {
    if buf.len() < STRING_REVERSE_HDR_SIZE { return Err(NipcError::Truncated); }
    let str_offset = u32::from_ne_bytes(buf[0..4].try_into().unwrap()) as usize;
    let str_length = u32::from_ne_bytes(buf[4..8].try_into().unwrap()) as usize;
    if str_offset + str_length + 1 > buf.len() { return Err(NipcError::OutOfBounds); }
    if buf[str_offset + str_length] != 0 { return Err(NipcError::MissingNul); }
    Ok(StringReverseView {
        str_data: &buf[str_offset..str_offset + str_length],
        str_len: str_length as u32,
    })
}

/// STRING_REVERSE dispatch: decode -> handler -> encode.
pub fn dispatch_string_reverse<F>(req: &[u8], resp: &mut [u8], handler: F) -> Option<usize>
where
    F: FnOnce(&[u8]) -> Option<Vec<u8>>,
{
    let view = string_reverse_decode(req).ok()?;
    let result = handler(view.str_data)?;
    let n = string_reverse_encode(&result, resp);
    if n == 0 {
        return None;
    }
    Some(n)
}

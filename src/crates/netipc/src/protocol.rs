//! Wire protocol definitions shared by the Rust crate.

use std::io;

pub const FRAME_MAGIC: u32 = 0x4e49_5043;
pub const FRAME_VERSION: u16 = 1;
pub const FRAME_SIZE: usize = 64;

pub const FRAME_KIND_REQUEST: u16 = 1;
pub const FRAME_KIND_RESPONSE: u16 = 2;

pub const METHOD_INCREMENT: u16 = 1;
pub const INCREMENT_PAYLOAD_LEN: u32 = 12;

pub const STATUS_OK: i32 = 0;
pub const STATUS_BAD_REQUEST: i32 = 1;
pub const STATUS_INTERNAL_ERROR: i32 = 2;

const OFF_MAGIC: usize = 0;
const OFF_VERSION: usize = 4;
const OFF_KIND: usize = 6;
const OFF_METHOD: usize = 8;
const OFF_PAYLOAD_LEN: usize = 12;
const OFF_REQUEST_ID: usize = 16;
const OFF_VALUE: usize = 24;
const OFF_STATUS: usize = 32;

pub type Frame = [u8; FRAME_SIZE];

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct IncrementRequest {
    pub value: u64,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct IncrementResponse {
    pub status: i32,
    pub value: u64,
}

fn protocol_error(message: &'static str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, message)
}

fn write_u16_le(frame: &mut Frame, off: usize, value: u16) {
    frame[off..off + 2].copy_from_slice(&value.to_le_bytes());
}

fn write_u32_le(frame: &mut Frame, off: usize, value: u32) {
    frame[off..off + 4].copy_from_slice(&value.to_le_bytes());
}

fn write_u64_le(frame: &mut Frame, off: usize, value: u64) {
    frame[off..off + 8].copy_from_slice(&value.to_le_bytes());
}

fn read_u16_le(frame: &Frame, off: usize) -> u16 {
    u16::from_le_bytes([frame[off], frame[off + 1]])
}

fn read_u32_le(frame: &Frame, off: usize) -> u32 {
    u32::from_le_bytes([frame[off], frame[off + 1], frame[off + 2], frame[off + 3]])
}

fn read_u64_le(frame: &Frame, off: usize) -> u64 {
    u64::from_le_bytes([
        frame[off],
        frame[off + 1],
        frame[off + 2],
        frame[off + 3],
        frame[off + 4],
        frame[off + 5],
        frame[off + 6],
        frame[off + 7],
    ])
}

fn read_i32_le(frame: &Frame, off: usize) -> i32 {
    i32::from_le_bytes([frame[off], frame[off + 1], frame[off + 2], frame[off + 3]])
}

fn encode_base(kind: u16, request_id: u64) -> Frame {
    let mut frame = [0u8; FRAME_SIZE];
    write_u32_le(&mut frame, OFF_MAGIC, FRAME_MAGIC);
    write_u16_le(&mut frame, OFF_VERSION, FRAME_VERSION);
    write_u16_le(&mut frame, OFF_KIND, kind);
    write_u16_le(&mut frame, OFF_METHOD, METHOD_INCREMENT);
    write_u32_le(&mut frame, OFF_PAYLOAD_LEN, INCREMENT_PAYLOAD_LEN);
    write_u64_le(&mut frame, OFF_REQUEST_ID, request_id);
    frame
}

fn validate_header(frame: &Frame, expected_kind: u16) -> io::Result<()> {
    let magic = read_u32_le(frame, OFF_MAGIC);
    let version = read_u16_le(frame, OFF_VERSION);
    let kind = read_u16_le(frame, OFF_KIND);
    let method = read_u16_le(frame, OFF_METHOD);
    let payload_len = read_u32_le(frame, OFF_PAYLOAD_LEN);

    if magic != FRAME_MAGIC || version != FRAME_VERSION {
        return Err(protocol_error("invalid frame magic/version"));
    }
    if kind != expected_kind || method != METHOD_INCREMENT || payload_len != INCREMENT_PAYLOAD_LEN {
        return Err(protocol_error("invalid frame kind/method/payload"));
    }

    Ok(())
}

pub fn encode_increment_request(request_id: u64, request: &IncrementRequest) -> Frame {
    let mut frame = encode_base(FRAME_KIND_REQUEST, request_id);
    write_u64_le(&mut frame, OFF_VALUE, request.value);
    write_u32_le(&mut frame, OFF_STATUS, 0);
    frame
}

pub fn decode_increment_request(frame: &Frame) -> io::Result<(u64, IncrementRequest)> {
    validate_header(frame, FRAME_KIND_REQUEST)?;
    Ok((
        read_u64_le(frame, OFF_REQUEST_ID),
        IncrementRequest {
            value: read_u64_le(frame, OFF_VALUE),
        },
    ))
}

pub fn encode_increment_response(request_id: u64, response: &IncrementResponse) -> Frame {
    let mut frame = encode_base(FRAME_KIND_RESPONSE, request_id);
    write_u64_le(&mut frame, OFF_VALUE, response.value);
    write_u32_le(&mut frame, OFF_STATUS, response.status as u32);
    frame
}

pub fn decode_increment_response(frame: &Frame) -> io::Result<(u64, IncrementResponse)> {
    validate_header(frame, FRAME_KIND_RESPONSE)?;
    Ok((
        read_u64_le(frame, OFF_REQUEST_ID),
        IncrementResponse {
            status: read_i32_le(frame, OFF_STATUS),
            value: read_u64_le(frame, OFF_VALUE),
        },
    ))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn request_roundtrip() {
        let frame = encode_increment_request(42, &IncrementRequest { value: 100 });
        let (request_id, request) = decode_increment_request(&frame).expect("decode request");
        assert_eq!(request_id, 42);
        assert_eq!(request.value, 100);
    }

    #[test]
    fn response_roundtrip() {
        let frame = encode_increment_response(
            7,
            &IncrementResponse {
                status: STATUS_OK,
                value: 8,
            },
        );
        let (request_id, response) = decode_increment_response(&frame).expect("decode response");
        assert_eq!(request_id, 7);
        assert_eq!(response.status, STATUS_OK);
        assert_eq!(response.value, 8);
    }

    #[test]
    fn rejects_invalid_magic() {
        let mut frame = encode_increment_request(1, &IncrementRequest { value: 1 });
        frame[OFF_MAGIC] = 0;
        assert!(decode_increment_request(&frame).is_err());
    }
}

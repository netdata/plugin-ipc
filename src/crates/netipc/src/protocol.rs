//! Wire envelope and codec for the netipc protocol.
//!
//! Pure byte-layout encode/decode. No I/O, no transport, no allocation on
//! decode. All multi-byte fields are little-endian on the wire.
//!
//! Decoded `View` types borrow the underlying buffer and are valid only while
//! that buffer lives. Copy immediately if the data is needed later.

// ---------------------------------------------------------------------------
//  Constants
// ---------------------------------------------------------------------------

pub const MAGIC_MSG: u32 = 0x4e495043; // "NIPC"
pub const MAGIC_CHUNK: u32 = 0x4e43484b; // "NCHK"
pub const VERSION: u16 = 1;
pub const HEADER_LEN: u16 = 32;
pub const HEADER_SIZE: usize = 32;

// Message kinds
pub const KIND_REQUEST: u16 = 1;
pub const KIND_RESPONSE: u16 = 2;
pub const KIND_CONTROL: u16 = 3;

// Flags
pub const FLAG_BATCH: u16 = 0x0001;

// Transport status
pub const STATUS_OK: u16 = 0;
pub const STATUS_BAD_ENVELOPE: u16 = 1;
pub const STATUS_AUTH_FAILED: u16 = 2;
pub const STATUS_INCOMPATIBLE: u16 = 3;
pub const STATUS_UNSUPPORTED: u16 = 4;
pub const STATUS_LIMIT_EXCEEDED: u16 = 5;
pub const STATUS_INTERNAL_ERROR: u16 = 6;

// Control opcodes
pub const CODE_HELLO: u16 = 1;
pub const CODE_HELLO_ACK: u16 = 2;

// Method codes
pub const METHOD_INCREMENT: u16 = 1;
pub const METHOD_CGROUPS_SNAPSHOT: u16 = 2;

// Profile bits
pub const PROFILE_BASELINE: u32 = 0x01;
pub const PROFILE_SHM_HYBRID: u32 = 0x02;
pub const PROFILE_SHM_FUTEX: u32 = 0x04;
pub const PROFILE_SHM_WAITADDR: u32 = 0x08;

// Defaults
pub const MAX_PAYLOAD_DEFAULT: u32 = 1024;

// Alignment
pub const ALIGNMENT: usize = 8;

// Payload sizes
const HELLO_SIZE: usize = 44;
const HELLO_ACK_SIZE: usize = 36;
const CGROUPS_REQ_SIZE: usize = 4;
const CGROUPS_RESP_HDR_SIZE: usize = 24;
const CGROUPS_DIR_ENTRY_SIZE: usize = 8;
const CGROUPS_ITEM_HDR_SIZE: usize = 32;

// ---------------------------------------------------------------------------
//  Errors
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum NipcError {
    /// Buffer too short for the expected structure.
    Truncated,
    /// Magic value mismatch.
    BadMagic,
    /// Unsupported version.
    BadVersion,
    /// header_len != 32.
    BadHeaderLen,
    /// Unknown message kind.
    BadKind,
    /// Unknown layout_version in a payload.
    BadLayout,
    /// Offset+length exceeds available data.
    OutOfBounds,
    /// String not NUL-terminated.
    MissingNul,
    /// Item not 8-byte aligned.
    BadAlignment,
    /// Directory inconsistent with payload size.
    BadItemCount,
    /// Builder ran out of space.
    Overflow,
}

impl core::fmt::Display for NipcError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            NipcError::Truncated => write!(f, "buffer too short"),
            NipcError::BadMagic => write!(f, "magic value mismatch"),
            NipcError::BadVersion => write!(f, "unsupported version"),
            NipcError::BadHeaderLen => write!(f, "header_len != 32"),
            NipcError::BadKind => write!(f, "unknown message kind"),
            NipcError::BadLayout => write!(f, "unknown layout_version"),
            NipcError::OutOfBounds => write!(f, "offset+length exceeds data"),
            NipcError::MissingNul => write!(f, "string not NUL-terminated"),
            NipcError::BadAlignment => write!(f, "item not 8-byte aligned"),
            NipcError::BadItemCount => write!(f, "item count inconsistent"),
            NipcError::Overflow => write!(f, "builder out of space"),
        }
    }
}

impl std::error::Error for NipcError {}

// ---------------------------------------------------------------------------
//  Utility
// ---------------------------------------------------------------------------

/// Round `v` up to the next multiple of 8.
#[inline]
pub fn align8(v: usize) -> usize {
    (v + 7) & !7
}

// ---------------------------------------------------------------------------
//  Outer message header (32 bytes)
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct Header {
    pub magic: u32,
    pub version: u16,
    pub header_len: u16,
    pub kind: u16,
    pub flags: u16,
    pub code: u16,
    pub transport_status: u16,
    pub payload_len: u32,
    pub item_count: u32,
    pub message_id: u64,
}

impl Header {
    /// Encode into `buf`. Returns 32 on success, 0 if buf is too small.
    pub fn encode(&self, buf: &mut [u8]) -> usize {
        if buf.len() < HEADER_SIZE {
            return 0;
        }
        buf[0..4].copy_from_slice(&self.magic.to_le_bytes());
        buf[4..6].copy_from_slice(&self.version.to_le_bytes());
        buf[6..8].copy_from_slice(&self.header_len.to_le_bytes());
        buf[8..10].copy_from_slice(&self.kind.to_le_bytes());
        buf[10..12].copy_from_slice(&self.flags.to_le_bytes());
        buf[12..14].copy_from_slice(&self.code.to_le_bytes());
        buf[14..16].copy_from_slice(&self.transport_status.to_le_bytes());
        buf[16..20].copy_from_slice(&self.payload_len.to_le_bytes());
        buf[20..24].copy_from_slice(&self.item_count.to_le_bytes());
        buf[24..32].copy_from_slice(&self.message_id.to_le_bytes());
        HEADER_SIZE
    }

    /// Decode from `buf`. Validates magic, version, header_len, kind.
    pub fn decode(buf: &[u8]) -> Result<Self, NipcError> {
        if buf.len() < HEADER_SIZE {
            return Err(NipcError::Truncated);
        }
        let hdr = Header {
            magic: u32::from_le_bytes(buf[0..4].try_into().unwrap()),
            version: u16::from_le_bytes(buf[4..6].try_into().unwrap()),
            header_len: u16::from_le_bytes(buf[6..8].try_into().unwrap()),
            kind: u16::from_le_bytes(buf[8..10].try_into().unwrap()),
            flags: u16::from_le_bytes(buf[10..12].try_into().unwrap()),
            code: u16::from_le_bytes(buf[12..14].try_into().unwrap()),
            transport_status: u16::from_le_bytes(buf[14..16].try_into().unwrap()),
            payload_len: u32::from_le_bytes(buf[16..20].try_into().unwrap()),
            item_count: u32::from_le_bytes(buf[20..24].try_into().unwrap()),
            message_id: u64::from_le_bytes(buf[24..32].try_into().unwrap()),
        };

        if hdr.magic != MAGIC_MSG {
            return Err(NipcError::BadMagic);
        }
        if hdr.version != VERSION {
            return Err(NipcError::BadVersion);
        }
        if hdr.header_len != HEADER_LEN {
            return Err(NipcError::BadHeaderLen);
        }
        if hdr.kind < KIND_REQUEST || hdr.kind > KIND_CONTROL {
            return Err(NipcError::BadKind);
        }
        Ok(hdr)
    }
}

// ---------------------------------------------------------------------------
//  Chunk continuation header (32 bytes)
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct ChunkHeader {
    pub magic: u32,
    pub version: u16,
    pub flags: u16,
    pub message_id: u64,
    pub total_message_len: u32,
    pub chunk_index: u32,
    pub chunk_count: u32,
    pub chunk_payload_len: u32,
}

impl ChunkHeader {
    /// Encode into `buf`. Returns 32 on success, 0 if buf is too small.
    pub fn encode(&self, buf: &mut [u8]) -> usize {
        if buf.len() < HEADER_SIZE {
            return 0;
        }
        buf[0..4].copy_from_slice(&self.magic.to_le_bytes());
        buf[4..6].copy_from_slice(&self.version.to_le_bytes());
        buf[6..8].copy_from_slice(&self.flags.to_le_bytes());
        buf[8..16].copy_from_slice(&self.message_id.to_le_bytes());
        buf[16..20].copy_from_slice(&self.total_message_len.to_le_bytes());
        buf[20..24].copy_from_slice(&self.chunk_index.to_le_bytes());
        buf[24..28].copy_from_slice(&self.chunk_count.to_le_bytes());
        buf[28..32].copy_from_slice(&self.chunk_payload_len.to_le_bytes());
        HEADER_SIZE
    }

    /// Decode from `buf`. Validates magic and version.
    pub fn decode(buf: &[u8]) -> Result<Self, NipcError> {
        if buf.len() < HEADER_SIZE {
            return Err(NipcError::Truncated);
        }
        let chk = ChunkHeader {
            magic: u32::from_le_bytes(buf[0..4].try_into().unwrap()),
            version: u16::from_le_bytes(buf[4..6].try_into().unwrap()),
            flags: u16::from_le_bytes(buf[6..8].try_into().unwrap()),
            message_id: u64::from_le_bytes(buf[8..16].try_into().unwrap()),
            total_message_len: u32::from_le_bytes(buf[16..20].try_into().unwrap()),
            chunk_index: u32::from_le_bytes(buf[20..24].try_into().unwrap()),
            chunk_count: u32::from_le_bytes(buf[24..28].try_into().unwrap()),
            chunk_payload_len: u32::from_le_bytes(buf[28..32].try_into().unwrap()),
        };

        if chk.magic != MAGIC_CHUNK {
            return Err(NipcError::BadMagic);
        }
        if chk.version != VERSION {
            return Err(NipcError::BadVersion);
        }
        if chk.flags != 0 {
            return Err(NipcError::BadLayout);
        }
        if chk.chunk_payload_len == 0 {
            return Err(NipcError::BadLayout);
        }
        Ok(chk)
    }
}

// ---------------------------------------------------------------------------
//  Batch item directory
// ---------------------------------------------------------------------------

/// One entry in a batch item directory (8 bytes on wire).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct BatchEntry {
    pub offset: u32,
    pub length: u32,
}

/// Encode `entries` into `buf`. Returns total bytes written (entries.len() * 8),
/// or 0 if buf is too small.
pub fn batch_dir_encode(entries: &[BatchEntry], buf: &mut [u8]) -> usize {
    let need = entries.len() * 8;
    if buf.len() < need {
        return 0;
    }
    for (i, e) in entries.iter().enumerate() {
        let base = i * 8;
        buf[base..base + 4].copy_from_slice(&e.offset.to_le_bytes());
        buf[base + 4..base + 8].copy_from_slice(&e.length.to_le_bytes());
    }
    need
}

/// Decode `item_count` directory entries from `buf`. Validates alignment and
/// that each entry falls within `packed_area_len`.
pub fn batch_dir_decode(
    buf: &[u8],
    item_count: u32,
    packed_area_len: u32,
) -> Result<Vec<BatchEntry>, NipcError> {
    let count = item_count as usize;
    let dir_size = count * 8;
    if buf.len() < dir_size {
        return Err(NipcError::Truncated);
    }

    let mut out = Vec::with_capacity(count);
    for i in 0..count {
        let base = i * 8;
        let offset = u32::from_le_bytes(buf[base..base + 4].try_into().unwrap());
        let length = u32::from_le_bytes(buf[base + 4..base + 8].try_into().unwrap());

        if (offset as usize) % ALIGNMENT != 0 {
            return Err(NipcError::BadAlignment);
        }
        if (offset as u64) + (length as u64) > packed_area_len as u64 {
            return Err(NipcError::OutOfBounds);
        }
        out.push(BatchEntry { offset, length });
    }
    Ok(out)
}

/// Extract a single batch item by index from a complete batch payload.
/// Returns (item_slice, item_len) on success.
pub fn batch_item_get(
    payload: &[u8],
    item_count: u32,
    index: u32,
) -> Result<(&[u8], u32), NipcError> {
    if index >= item_count {
        return Err(NipcError::OutOfBounds);
    }

    let dir_size = item_count as usize * 8;
    let dir_aligned = align8(dir_size);

    if payload.len() < dir_aligned {
        return Err(NipcError::Truncated);
    }

    let idx = index as usize;
    let base = idx * 8;
    let off = u32::from_le_bytes(payload[base..base + 4].try_into().unwrap());
    let len = u32::from_le_bytes(payload[base + 4..base + 8].try_into().unwrap());

    let packed_area_start = dir_aligned;
    let packed_area_len = payload.len() - packed_area_start;

    if (off as usize) % ALIGNMENT != 0 {
        return Err(NipcError::BadAlignment);
    }
    if (off as u64) + (len as u64) > packed_area_len as u64 {
        return Err(NipcError::OutOfBounds);
    }

    let start = packed_area_start + off as usize;
    let end = start + len as usize;
    Ok((&payload[start..end], len))
}

// ---------------------------------------------------------------------------
//  Batch builder
// ---------------------------------------------------------------------------

/// Builds a batch payload: [directory] [align-pad] [packed items].
pub struct BatchBuilder<'a> {
    buf: &'a mut [u8],
    item_count: u32,
    max_items: u32,
    dir_end: usize,    // byte offset where directory reservation ends
    data_offset: usize, // current offset within the packed data area (relative)
}

impl<'a> BatchBuilder<'a> {
    /// Create a new batch builder. `buf` must be large enough for
    /// `max_items * 8` (directory) + packed data.
    pub fn new(buf: &'a mut [u8], max_items: u32) -> Self {
        let dir_end = align8(max_items as usize * 8);
        BatchBuilder {
            buf,
            item_count: 0,
            max_items,
            dir_end,
            data_offset: 0,
        }
    }

    /// Add an item payload. Handles alignment padding.
    pub fn add(&mut self, item: &[u8]) -> Result<(), NipcError> {
        if self.item_count >= self.max_items {
            return Err(NipcError::Overflow);
        }

        let aligned_off = align8(self.data_offset);
        let abs_pos = self.dir_end + aligned_off;

        if abs_pos + item.len() > self.buf.len() {
            return Err(NipcError::Overflow);
        }

        // Zero alignment padding
        if aligned_off > self.data_offset {
            let pad_start = self.dir_end + self.data_offset;
            let pad_end = self.dir_end + aligned_off;
            self.buf[pad_start..pad_end].fill(0);
        }

        self.buf[abs_pos..abs_pos + item.len()].copy_from_slice(item);

        // Write directory entry
        let idx = self.item_count as usize;
        let dir_base = idx * 8;
        self.buf[dir_base..dir_base + 4]
            .copy_from_slice(&(aligned_off as u32).to_le_bytes());
        self.buf[dir_base + 4..dir_base + 8]
            .copy_from_slice(&(item.len() as u32).to_le_bytes());

        self.data_offset = aligned_off + item.len();
        self.item_count += 1;
        Ok(())
    }

    /// Finalize the batch. Returns (total_payload_size, item_count).
    /// Compacts if fewer items were added than max_items.
    pub fn finish(self) -> (usize, u32) {
        let count = self.item_count;
        let final_dir_aligned = align8(count as usize * 8);

        if final_dir_aligned < self.dir_end && self.data_offset > 0 {
            // Shift packed data left
            self.buf.copy_within(
                self.dir_end..self.dir_end + self.data_offset,
                final_dir_aligned,
            );
        }

        let total = final_dir_aligned + align8(self.data_offset);
        (total, count)
    }
}

// ---------------------------------------------------------------------------
//  Hello payload (44 bytes)
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct Hello {
    pub layout_version: u16,
    pub flags: u16,
    pub supported_profiles: u32,
    pub preferred_profiles: u32,
    pub max_request_payload_bytes: u32,
    pub max_request_batch_items: u32,
    pub max_response_payload_bytes: u32,
    pub max_response_batch_items: u32,
    pub auth_token: u64,
    pub packet_size: u32,
}

impl Hello {
    /// Encode into `buf`. Returns 44 on success, 0 if buf is too small.
    pub fn encode(&self, buf: &mut [u8]) -> usize {
        if buf.len() < HELLO_SIZE {
            return 0;
        }
        buf[0..2].copy_from_slice(&self.layout_version.to_le_bytes());
        buf[2..4].copy_from_slice(&self.flags.to_le_bytes());
        buf[4..8].copy_from_slice(&self.supported_profiles.to_le_bytes());
        buf[8..12].copy_from_slice(&self.preferred_profiles.to_le_bytes());
        buf[12..16].copy_from_slice(&self.max_request_payload_bytes.to_le_bytes());
        buf[16..20].copy_from_slice(&self.max_request_batch_items.to_le_bytes());
        buf[20..24].copy_from_slice(&self.max_response_payload_bytes.to_le_bytes());
        buf[24..28].copy_from_slice(&self.max_response_batch_items.to_le_bytes());
        buf[28..32].copy_from_slice(&0u32.to_le_bytes()); // padding
        buf[32..40].copy_from_slice(&self.auth_token.to_le_bytes());
        buf[40..44].copy_from_slice(&self.packet_size.to_le_bytes());
        HELLO_SIZE
    }

    /// Decode from `buf`. Validates layout_version.
    pub fn decode(buf: &[u8]) -> Result<Self, NipcError> {
        if buf.len() < HELLO_SIZE {
            return Err(NipcError::Truncated);
        }
        let h = Hello {
            layout_version: u16::from_le_bytes(buf[0..2].try_into().unwrap()),
            flags: u16::from_le_bytes(buf[2..4].try_into().unwrap()),
            supported_profiles: u32::from_le_bytes(buf[4..8].try_into().unwrap()),
            preferred_profiles: u32::from_le_bytes(buf[8..12].try_into().unwrap()),
            max_request_payload_bytes: u32::from_le_bytes(buf[12..16].try_into().unwrap()),
            max_request_batch_items: u32::from_le_bytes(buf[16..20].try_into().unwrap()),
            max_response_payload_bytes: u32::from_le_bytes(buf[20..24].try_into().unwrap()),
            max_response_batch_items: u32::from_le_bytes(buf[24..28].try_into().unwrap()),
            // buf[28..32] is reserved padding, must be zero
            auth_token: u64::from_le_bytes(buf[32..40].try_into().unwrap()),
            packet_size: u32::from_le_bytes(buf[40..44].try_into().unwrap()),
        };

        if h.layout_version != 1 {
            return Err(NipcError::BadLayout);
        }

        // Validate padding bytes 28..32 are zero
        if u32::from_le_bytes(buf[28..32].try_into().unwrap()) != 0 {
            return Err(NipcError::BadLayout);
        }

        Ok(h)
    }
}

// ---------------------------------------------------------------------------
//  Hello-ack payload (36 bytes)
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct HelloAck {
    pub layout_version: u16,
    pub flags: u16,
    pub server_supported_profiles: u32,
    pub intersection_profiles: u32,
    pub selected_profile: u32,
    pub agreed_max_request_payload_bytes: u32,
    pub agreed_max_request_batch_items: u32,
    pub agreed_max_response_payload_bytes: u32,
    pub agreed_max_response_batch_items: u32,
    pub agreed_packet_size: u32,
}

impl HelloAck {
    /// Encode into `buf`. Returns 36 on success, 0 if buf is too small.
    pub fn encode(&self, buf: &mut [u8]) -> usize {
        if buf.len() < HELLO_ACK_SIZE {
            return 0;
        }
        buf[0..2].copy_from_slice(&self.layout_version.to_le_bytes());
        buf[2..4].copy_from_slice(&self.flags.to_le_bytes());
        buf[4..8].copy_from_slice(&self.server_supported_profiles.to_le_bytes());
        buf[8..12].copy_from_slice(&self.intersection_profiles.to_le_bytes());
        buf[12..16].copy_from_slice(&self.selected_profile.to_le_bytes());
        buf[16..20].copy_from_slice(&self.agreed_max_request_payload_bytes.to_le_bytes());
        buf[20..24].copy_from_slice(&self.agreed_max_request_batch_items.to_le_bytes());
        buf[24..28].copy_from_slice(&self.agreed_max_response_payload_bytes.to_le_bytes());
        buf[28..32].copy_from_slice(&self.agreed_max_response_batch_items.to_le_bytes());
        buf[32..36].copy_from_slice(&self.agreed_packet_size.to_le_bytes());
        HELLO_ACK_SIZE
    }

    /// Decode from `buf`. Validates layout_version.
    pub fn decode(buf: &[u8]) -> Result<Self, NipcError> {
        if buf.len() < HELLO_ACK_SIZE {
            return Err(NipcError::Truncated);
        }
        let h = HelloAck {
            layout_version: u16::from_le_bytes(buf[0..2].try_into().unwrap()),
            flags: u16::from_le_bytes(buf[2..4].try_into().unwrap()),
            server_supported_profiles: u32::from_le_bytes(buf[4..8].try_into().unwrap()),
            intersection_profiles: u32::from_le_bytes(buf[8..12].try_into().unwrap()),
            selected_profile: u32::from_le_bytes(buf[12..16].try_into().unwrap()),
            agreed_max_request_payload_bytes: u32::from_le_bytes(
                buf[16..20].try_into().unwrap(),
            ),
            agreed_max_request_batch_items: u32::from_le_bytes(
                buf[20..24].try_into().unwrap(),
            ),
            agreed_max_response_payload_bytes: u32::from_le_bytes(
                buf[24..28].try_into().unwrap(),
            ),
            agreed_max_response_batch_items: u32::from_le_bytes(
                buf[28..32].try_into().unwrap(),
            ),
            agreed_packet_size: u32::from_le_bytes(buf[32..36].try_into().unwrap()),
        };

        if h.layout_version != 1 {
            return Err(NipcError::BadLayout);
        }
        if h.flags != 0 {
            return Err(NipcError::BadLayout);
        }
        Ok(h)
    }
}

// ---------------------------------------------------------------------------
//  Cgroups snapshot request (4 bytes)
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct CgroupsRequest {
    pub layout_version: u16,
    pub flags: u16,
}

impl CgroupsRequest {
    /// Encode into `buf`. Returns 4 on success, 0 if buf is too small.
    pub fn encode(&self, buf: &mut [u8]) -> usize {
        if buf.len() < CGROUPS_REQ_SIZE {
            return 0;
        }
        buf[0..2].copy_from_slice(&self.layout_version.to_le_bytes());
        buf[2..4].copy_from_slice(&self.flags.to_le_bytes());
        CGROUPS_REQ_SIZE
    }

    /// Decode from `buf`. Validates layout_version.
    pub fn decode(buf: &[u8]) -> Result<Self, NipcError> {
        if buf.len() < CGROUPS_REQ_SIZE {
            return Err(NipcError::Truncated);
        }
        let r = CgroupsRequest {
            layout_version: u16::from_le_bytes(buf[0..2].try_into().unwrap()),
            flags: u16::from_le_bytes(buf[2..4].try_into().unwrap()),
        };
        if r.layout_version != 1 {
            return Err(NipcError::BadLayout);
        }
        // flags must be zero (reserved for future use)
        if r.flags != 0 {
            return Err(NipcError::BadLayout);
        }
        Ok(r)
    }
}

// ---------------------------------------------------------------------------
//  Cgroups snapshot response
// ---------------------------------------------------------------------------

/// Borrowed string view into the payload buffer.
/// Valid only while the underlying payload buffer is alive.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct StrView<'a> {
    /// Slice into payload, NUL-terminated.
    pub bytes: &'a [u8],
    /// Length excluding the NUL.
    pub len: u32,
}

impl<'a> StrView<'a> {
    /// Return the string content as a `&str`, or a UTF-8 error.
    pub fn as_str(&self) -> Result<&'a str, core::str::Utf8Error> {
        core::str::from_utf8(&self.bytes[..self.len as usize])
    }

    /// Return the string content as a byte slice (without the NUL).
    pub fn as_bytes(&self) -> &'a [u8] {
        &self.bytes[..self.len as usize]
    }
}

/// Per-item view -- ephemeral, borrows the payload buffer.
/// Valid only while the payload buffer is alive.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct CgroupsItemView<'a> {
    pub layout_version: u16,
    pub flags: u16,
    pub hash: u32,
    pub options: u32,
    pub enabled: u32,
    pub name: StrView<'a>,
    pub path: StrView<'a>,
}

/// Full snapshot view -- ephemeral, borrows the payload buffer.
/// Valid only during the current library call or callback.
/// Copy immediately if the data is needed later.
#[derive(Debug)]
pub struct CgroupsResponseView<'a> {
    pub layout_version: u16,
    pub flags: u16,
    pub item_count: u32,
    pub systemd_enabled: u32,
    pub generation: u64,
    payload: &'a [u8],
}

impl<'a> CgroupsResponseView<'a> {
    /// Decode the snapshot response header and validate the item directory.
    /// On success, use `item()` to access individual items.
    pub fn decode(buf: &'a [u8]) -> Result<Self, NipcError> {
        if buf.len() < CGROUPS_RESP_HDR_SIZE {
            return Err(NipcError::Truncated);
        }

        let layout_version = u16::from_le_bytes(buf[0..2].try_into().unwrap());
        let flags = u16::from_le_bytes(buf[2..4].try_into().unwrap());
        let item_count = u32::from_le_bytes(buf[4..8].try_into().unwrap());
        let systemd_enabled = u32::from_le_bytes(buf[8..12].try_into().unwrap());
        // buf[12..16] reserved, must be zero
        let reserved = u32::from_le_bytes(buf[12..16].try_into().unwrap());
        let generation = u64::from_le_bytes(buf[16..24].try_into().unwrap());

        if layout_version != 1 {
            return Err(NipcError::BadLayout);
        }

        // flags must be zero
        if flags != 0 {
            return Err(NipcError::BadLayout);
        }

        // reserved field must be zero
        if reserved != 0 {
            return Err(NipcError::BadLayout);
        }

        // Validate directory fits
        let dir_size = item_count as usize * CGROUPS_DIR_ENTRY_SIZE;
        let dir_end = CGROUPS_RESP_HDR_SIZE + dir_size;
        if dir_end > buf.len() {
            return Err(NipcError::Truncated);
        }

        let packed_area_len = buf.len() - dir_end;

        // Validate each directory entry
        for i in 0..item_count as usize {
            let base = CGROUPS_RESP_HDR_SIZE + i * 8;
            let off = u32::from_le_bytes(buf[base..base + 4].try_into().unwrap());
            let len = u32::from_le_bytes(buf[base + 4..base + 8].try_into().unwrap());

            if (off as usize) % ALIGNMENT != 0 {
                return Err(NipcError::BadAlignment);
            }
            if (off as u64) + (len as u64) > packed_area_len as u64 {
                return Err(NipcError::OutOfBounds);
            }
            if (len as usize) < CGROUPS_ITEM_HDR_SIZE {
                return Err(NipcError::Truncated);
            }
        }

        Ok(CgroupsResponseView {
            layout_version,
            flags,
            item_count,
            systemd_enabled,
            generation,
            payload: buf,
        })
    }

    /// Access item at `index`. Returns an ephemeral item view.
    pub fn item(&self, index: u32) -> Result<CgroupsItemView<'a>, NipcError> {
        if index >= self.item_count {
            return Err(NipcError::OutOfBounds);
        }

        let dir_start = CGROUPS_RESP_HDR_SIZE;
        let dir_size = self.item_count as usize * CGROUPS_DIR_ENTRY_SIZE;
        let packed_area_start = dir_start + dir_size;

        let dir_base = dir_start + index as usize * 8;
        let item_off =
            u32::from_le_bytes(self.payload[dir_base..dir_base + 4].try_into().unwrap());
        let item_len = u32::from_le_bytes(
            self.payload[dir_base + 4..dir_base + 8].try_into().unwrap(),
        );

        let item_start = packed_area_start + item_off as usize;
        let item = &self.payload[item_start..item_start + item_len as usize];

        let layout_version = u16::from_le_bytes(item[0..2].try_into().unwrap());
        let flags = u16::from_le_bytes(item[2..4].try_into().unwrap());
        let hash = u32::from_le_bytes(item[4..8].try_into().unwrap());
        let options = u32::from_le_bytes(item[8..12].try_into().unwrap());
        let enabled = u32::from_le_bytes(item[12..16].try_into().unwrap());

        let name_off = u32::from_le_bytes(item[16..20].try_into().unwrap()) as usize;
        let name_len = u32::from_le_bytes(item[20..24].try_into().unwrap());
        let path_off = u32::from_le_bytes(item[24..28].try_into().unwrap()) as usize;
        let path_len = u32::from_le_bytes(item[28..32].try_into().unwrap());

        if layout_version != 1 {
            return Err(NipcError::BadLayout);
        }

        // item flags must be zero
        if flags != 0 {
            return Err(NipcError::BadLayout);
        }

        // Validate name string
        if name_off < CGROUPS_ITEM_HDR_SIZE {
            return Err(NipcError::OutOfBounds);
        }
        if (name_off as u64) + (name_len as u64) + 1 > item_len as u64 {
            return Err(NipcError::OutOfBounds);
        }
        if item[name_off + name_len as usize] != 0 {
            return Err(NipcError::MissingNul);
        }

        // Validate path string
        if path_off < CGROUPS_ITEM_HDR_SIZE {
            return Err(NipcError::OutOfBounds);
        }
        if (path_off as u64) + (path_len as u64) + 1 > item_len as u64 {
            return Err(NipcError::OutOfBounds);
        }
        if item[path_off + path_len as usize] != 0 {
            return Err(NipcError::MissingNul);
        }

        let name = StrView {
            bytes: &item[name_off..name_off + name_len as usize + 1],
            len: name_len,
        };
        let path = StrView {
            bytes: &item[path_off..path_off + path_len as usize + 1],
            len: path_len,
        };

        Ok(CgroupsItemView {
            layout_version,
            flags,
            hash,
            options,
            enabled,
            name,
            path,
        })
    }
}

// ---------------------------------------------------------------------------
//  Cgroups snapshot response builder
// ---------------------------------------------------------------------------

/// Builds a cgroups snapshot response payload.
///
/// Layout during building (max_items directory slots reserved):
///   [24-byte header space] [max_items*8 directory] [packed items]
///
/// Layout after finish (compacted to actual item_count):
///   [24-byte header] [item_count*8 directory] [packed items]
pub struct CgroupsBuilder<'a> {
    buf: &'a mut [u8],
    systemd_enabled: u32,
    generation: u64,
    item_count: u32,
    max_items: u32,
    data_offset: usize, // current write position (absolute in buf)
}

impl<'a> CgroupsBuilder<'a> {
    /// Initialize the builder. `buf` must be caller-owned and large enough
    /// for the expected snapshot.
    pub fn new(
        buf: &'a mut [u8],
        max_items: u32,
        systemd_enabled: u32,
        generation: u64,
    ) -> Self {
        let data_offset = CGROUPS_RESP_HDR_SIZE
            + max_items as usize * CGROUPS_DIR_ENTRY_SIZE;
        CgroupsBuilder {
            buf,
            systemd_enabled,
            generation,
            item_count: 0,
            max_items,
            data_offset,
        }
    }

    /// Add one cgroup item. Handles offset bookkeeping, NUL termination,
    /// and alignment.
    pub fn add(
        &mut self,
        hash: u32,
        options: u32,
        enabled: u32,
        name: &[u8],
        path: &[u8],
    ) -> Result<(), NipcError> {
        if self.item_count >= self.max_items {
            return Err(NipcError::Overflow);
        }

        // Align item start to 8 bytes
        let item_start = align8(self.data_offset);

        // Item payload: 32-byte header + name + NUL + path + NUL
        let item_size = CGROUPS_ITEM_HDR_SIZE + name.len() + 1 + path.len() + 1;

        if item_start + item_size > self.buf.len() {
            return Err(NipcError::Overflow);
        }

        // Zero alignment padding
        if item_start > self.data_offset {
            self.buf[self.data_offset..item_start].fill(0);
        }

        let name_offset = CGROUPS_ITEM_HDR_SIZE as u32;
        let path_offset = CGROUPS_ITEM_HDR_SIZE as u32 + name.len() as u32 + 1;

        // Write item header
        let p = item_start;
        self.buf[p..p + 2].copy_from_slice(&1u16.to_le_bytes()); // layout_version
        self.buf[p + 2..p + 4].copy_from_slice(&0u16.to_le_bytes()); // flags
        self.buf[p + 4..p + 8].copy_from_slice(&hash.to_le_bytes());
        self.buf[p + 8..p + 12].copy_from_slice(&options.to_le_bytes());
        self.buf[p + 12..p + 16].copy_from_slice(&enabled.to_le_bytes());
        self.buf[p + 16..p + 20].copy_from_slice(&name_offset.to_le_bytes());
        self.buf[p + 20..p + 24].copy_from_slice(&(name.len() as u32).to_le_bytes());
        self.buf[p + 24..p + 28].copy_from_slice(&path_offset.to_le_bytes());
        self.buf[p + 28..p + 32].copy_from_slice(&(path.len() as u32).to_le_bytes());

        // Write strings with NUL terminators
        let name_start = p + name_offset as usize;
        self.buf[name_start..name_start + name.len()].copy_from_slice(name);
        self.buf[name_start + name.len()] = 0;

        let path_start = p + path_offset as usize;
        self.buf[path_start..path_start + path.len()].copy_from_slice(path);
        self.buf[path_start + path.len()] = 0;

        // Write directory entry (absolute offset stored temporarily)
        let dir_entry = CGROUPS_RESP_HDR_SIZE
            + self.item_count as usize * CGROUPS_DIR_ENTRY_SIZE;
        self.buf[dir_entry..dir_entry + 4]
            .copy_from_slice(&(item_start as u32).to_le_bytes());
        self.buf[dir_entry + 4..dir_entry + 8]
            .copy_from_slice(&(item_size as u32).to_le_bytes());

        self.data_offset = item_start + item_size;
        self.item_count += 1;
        Ok(())
    }

    /// Finalize the builder. Returns the total payload size. The buffer now
    /// contains a complete, decodable cgroups snapshot response payload.
    pub fn finish(self) -> usize {
        let p = &mut *{ self.buf };

        if self.item_count == 0 {
            p[0..2].copy_from_slice(&1u16.to_le_bytes());
            p[2..4].copy_from_slice(&0u16.to_le_bytes());
            p[4..8].copy_from_slice(&0u32.to_le_bytes());
            p[8..12].copy_from_slice(&self.systemd_enabled.to_le_bytes());
            p[12..16].copy_from_slice(&0u32.to_le_bytes());
            p[16..24].copy_from_slice(&self.generation.to_le_bytes());
            return CGROUPS_RESP_HDR_SIZE;
        }

        // Where the decoder expects packed data to start
        let final_packed_start = CGROUPS_RESP_HDR_SIZE
            + self.item_count as usize * CGROUPS_DIR_ENTRY_SIZE;

        // Read the first directory entry to find where packed data begins
        let first_item_abs = u32::from_le_bytes(
            p[CGROUPS_RESP_HDR_SIZE..CGROUPS_RESP_HDR_SIZE + 4]
                .try_into()
                .unwrap(),
        ) as usize;

        let packed_data_len = self.data_offset - first_item_abs;

        if final_packed_start < first_item_abs {
            // Shift packed data left
            p.copy_within(first_item_abs..first_item_abs + packed_data_len,
                          final_packed_start);
        }

        // Convert directory entries from absolute to relative offsets
        let dir_base = CGROUPS_RESP_HDR_SIZE;
        for i in 0..self.item_count as usize {
            let entry = dir_base + i * CGROUPS_DIR_ENTRY_SIZE;
            let abs_off = u32::from_le_bytes(
                p[entry..entry + 4].try_into().unwrap(),
            );
            let rel_off = abs_off - first_item_abs as u32;
            p[entry..entry + 4].copy_from_slice(&rel_off.to_le_bytes());
            // length stays the same
        }

        // Write snapshot header
        p[0..2].copy_from_slice(&1u16.to_le_bytes());
        p[2..4].copy_from_slice(&0u16.to_le_bytes());
        p[4..8].copy_from_slice(&self.item_count.to_le_bytes());
        p[8..12].copy_from_slice(&self.systemd_enabled.to_le_bytes());
        p[12..16].copy_from_slice(&0u32.to_le_bytes());
        p[16..24].copy_from_slice(&self.generation.to_le_bytes());

        final_packed_start + packed_data_len
    }
}

// ===========================================================================
//  Tests
// ===========================================================================

#[cfg(test)]
mod tests {
    use super::*;

    // -----------------------------------------------------------------------
    //  Outer message header tests
    // -----------------------------------------------------------------------

    #[test]
    fn header_roundtrip() {
        let h = Header {
            magic: MAGIC_MSG,
            version: VERSION,
            header_len: HEADER_LEN,
            kind: KIND_REQUEST,
            flags: FLAG_BATCH,
            code: METHOD_CGROUPS_SNAPSHOT,
            transport_status: STATUS_OK,
            payload_len: 12345,
            item_count: 42,
            message_id: 0xDEAD_BEEF_CAFE_BABE,
        };

        let mut buf = [0u8; 64];
        let n = h.encode(&mut buf);
        assert_eq!(n, 32);

        let out = Header::decode(&buf[..n]).unwrap();
        assert_eq!(out, h);
    }

    #[test]
    fn header_encode_too_small() {
        let h = Header::default();
        let mut buf = [0u8; 16];
        assert_eq!(h.encode(&mut buf), 0);
    }

    #[test]
    fn header_decode_truncated() {
        let buf = [0u8; 31];
        assert_eq!(Header::decode(&buf), Err(NipcError::Truncated));
    }

    #[test]
    fn header_decode_bad_magic() {
        let h = Header {
            magic: 0x12345678,
            version: VERSION,
            header_len: HEADER_LEN,
            kind: KIND_REQUEST,
            ..Default::default()
        };
        let mut buf = [0u8; 32];
        h.encode(&mut buf);
        assert_eq!(Header::decode(&buf), Err(NipcError::BadMagic));
    }

    #[test]
    fn header_decode_bad_version() {
        let h = Header {
            magic: MAGIC_MSG,
            version: 99,
            header_len: HEADER_LEN,
            kind: KIND_REQUEST,
            ..Default::default()
        };
        let mut buf = [0u8; 32];
        h.encode(&mut buf);
        assert_eq!(Header::decode(&buf), Err(NipcError::BadVersion));
    }

    #[test]
    fn header_decode_bad_header_len() {
        let h = Header {
            magic: MAGIC_MSG,
            version: VERSION,
            header_len: 64,
            kind: KIND_REQUEST,
            ..Default::default()
        };
        let mut buf = [0u8; 32];
        h.encode(&mut buf);
        assert_eq!(Header::decode(&buf), Err(NipcError::BadHeaderLen));
    }

    #[test]
    fn header_decode_bad_kind() {
        // kind = 0
        let h = Header {
            magic: MAGIC_MSG,
            version: VERSION,
            header_len: HEADER_LEN,
            kind: 0,
            ..Default::default()
        };
        let mut buf = [0u8; 32];
        h.encode(&mut buf);
        assert_eq!(Header::decode(&buf), Err(NipcError::BadKind));

        // kind = 4
        let h2 = Header { kind: 4, ..h };
        h2.encode(&mut buf);
        assert_eq!(Header::decode(&buf), Err(NipcError::BadKind));
    }

    #[test]
    fn header_all_kinds() {
        for k in KIND_REQUEST..=KIND_CONTROL {
            let h = Header {
                magic: MAGIC_MSG,
                version: VERSION,
                header_len: HEADER_LEN,
                kind: k,
                ..Default::default()
            };
            let mut buf = [0u8; 32];
            h.encode(&mut buf);
            let out = Header::decode(&buf).unwrap();
            assert_eq!(out.kind, k);
        }
    }

    #[test]
    fn header_wire_bytes() {
        let h = Header {
            magic: MAGIC_MSG,
            version: VERSION,
            header_len: HEADER_LEN,
            kind: KIND_REQUEST,
            flags: 0,
            code: METHOD_CGROUPS_SNAPSHOT,
            transport_status: STATUS_OK,
            payload_len: 4,
            item_count: 1,
            message_id: 1,
        };

        let mut buf = [0u8; 32];
        h.encode(&mut buf);

        // magic = 0x4e495043 LE: 43 50 49 4e
        assert_eq!(&buf[0..4], &[0x43, 0x50, 0x49, 0x4e]);
        // version = 1 LE: 01 00
        assert_eq!(&buf[4..6], &[0x01, 0x00]);
        // header_len = 32 LE: 20 00
        assert_eq!(&buf[6..8], &[0x20, 0x00]);
        // kind = 1 LE: 01 00
        assert_eq!(&buf[8..10], &[0x01, 0x00]);
        // code = 2 LE: 02 00
        assert_eq!(&buf[12..14], &[0x02, 0x00]);
    }

    // -----------------------------------------------------------------------
    //  Chunk continuation header tests
    // -----------------------------------------------------------------------

    #[test]
    fn chunk_header_roundtrip() {
        let c = ChunkHeader {
            magic: MAGIC_CHUNK,
            version: VERSION,
            flags: 0,
            message_id: 0x1234_5678_90AB_CDEF,
            total_message_len: 100000,
            chunk_index: 3,
            chunk_count: 10,
            chunk_payload_len: 8192,
        };

        let mut buf = [0u8; 64];
        let n = c.encode(&mut buf);
        assert_eq!(n, 32);

        let out = ChunkHeader::decode(&buf[..n]).unwrap();
        assert_eq!(out, c);
    }

    #[test]
    fn chunk_decode_truncated() {
        let buf = [0u8; 31];
        assert_eq!(ChunkHeader::decode(&buf), Err(NipcError::Truncated));
    }

    #[test]
    fn chunk_decode_bad_magic() {
        let c = ChunkHeader {
            magic: MAGIC_MSG, // wrong magic for chunk
            version: VERSION,
            ..Default::default()
        };
        let mut buf = [0u8; 32];
        c.encode(&mut buf);
        assert_eq!(ChunkHeader::decode(&buf), Err(NipcError::BadMagic));
    }

    #[test]
    fn chunk_decode_bad_version() {
        let c = ChunkHeader {
            magic: MAGIC_CHUNK,
            version: 2,
            ..Default::default()
        };
        let mut buf = [0u8; 32];
        c.encode(&mut buf);
        assert_eq!(ChunkHeader::decode(&buf), Err(NipcError::BadVersion));
    }

    #[test]
    fn chunk_encode_too_small() {
        let c = ChunkHeader::default();
        let mut buf = [0u8; 16];
        assert_eq!(c.encode(&mut buf), 0);
    }

    #[test]
    fn chunk_wire_bytes() {
        let c = ChunkHeader {
            magic: MAGIC_CHUNK,
            version: VERSION,
            flags: 0,
            message_id: 1,
            total_message_len: 256,
            chunk_index: 1,
            chunk_count: 3,
            chunk_payload_len: 100,
        };

        let mut buf = [0u8; 32];
        c.encode(&mut buf);

        // magic = 0x4e43484b LE: 4b 48 43 4e
        assert_eq!(&buf[0..4], &[0x4b, 0x48, 0x43, 0x4e]);
    }

    // -----------------------------------------------------------------------
    //  Batch item directory tests
    // -----------------------------------------------------------------------

    #[test]
    fn batch_dir_roundtrip() {
        let entries = [
            BatchEntry { offset: 0, length: 100 },
            BatchEntry { offset: 104, length: 200 },
            BatchEntry { offset: 304, length: 50 },
        ];

        let mut buf = [0u8; 64];
        let n = batch_dir_encode(&entries, &mut buf);
        assert_eq!(n, 24);

        let out = batch_dir_decode(&buf[..n], 3, 400).unwrap();
        assert_eq!(out[0], entries[0]);
        assert_eq!(out[1], entries[1]);
        assert_eq!(out[2], entries[2]);
    }

    #[test]
    fn batch_dir_decode_truncated() {
        let buf = [0u8; 12];
        assert_eq!(
            batch_dir_decode(&buf, 2, 1000),
            Err(NipcError::Truncated)
        );
    }

    #[test]
    fn batch_dir_decode_oob() {
        let e = BatchEntry { offset: 0, length: 200 };
        let mut buf = [0u8; 8];
        batch_dir_encode(&[e], &mut buf);
        assert_eq!(
            batch_dir_decode(&buf, 1, 100),
            Err(NipcError::OutOfBounds)
        );
    }

    #[test]
    fn batch_dir_decode_bad_alignment() {
        let mut buf = [0u8; 8];
        // Manually write unaligned offset
        buf[0..4].copy_from_slice(&3u32.to_le_bytes());
        buf[4..8].copy_from_slice(&10u32.to_le_bytes());
        assert_eq!(
            batch_dir_decode(&buf, 1, 100),
            Err(NipcError::BadAlignment)
        );
    }

    // -----------------------------------------------------------------------
    //  Batch builder + extraction tests
    // -----------------------------------------------------------------------

    #[test]
    fn batch_builder_roundtrip() {
        let mut buf = [0u8; 1024];
        let mut b = BatchBuilder::new(&mut buf, 4);

        let item1 = [1u8, 2, 3, 4, 5];
        let item2 = [10u8, 20, 30];
        let item3 = [0xAAu8, 0xBB];

        b.add(&item1).unwrap();
        b.add(&item2).unwrap();
        b.add(&item3).unwrap();

        let (total, count) = b.finish();
        assert_eq!(count, 3);
        assert!(total > 0);

        // Extract items
        let (data, len) = batch_item_get(&buf[..total], 3, 0).unwrap();
        assert_eq!(len as usize, item1.len());
        assert_eq!(data, &item1);

        let (data, len) = batch_item_get(&buf[..total], 3, 1).unwrap();
        assert_eq!(len as usize, item2.len());
        assert_eq!(data, &item2);

        let (data, len) = batch_item_get(&buf[..total], 3, 2).unwrap();
        assert_eq!(len as usize, item3.len());
        assert_eq!(data, &item3);
    }

    #[test]
    fn batch_builder_overflow() {
        let mut buf = [0u8; 32];
        let mut b = BatchBuilder::new(&mut buf, 1);
        let item = [1u8];
        b.add(&item).unwrap();
        assert_eq!(b.add(&item), Err(NipcError::Overflow));
    }

    #[test]
    fn batch_builder_buf_overflow() {
        let mut buf = [0u8; 24];
        let mut b = BatchBuilder::new(&mut buf, 1);
        let big = [0u8; 100];
        assert_eq!(b.add(&big), Err(NipcError::Overflow));
    }

    #[test]
    fn batch_item_get_oob_index() {
        let mut buf = [0u8; 64];
        let mut b = BatchBuilder::new(&mut buf, 2);
        b.add(&[1u8]).unwrap();
        let (total, count) = b.finish();
        assert_eq!(
            batch_item_get(&buf[..total], count, 5),
            Err(NipcError::OutOfBounds)
        );
    }

    #[test]
    fn batch_empty() {
        let mut buf = [0u8; 64];
        let b = BatchBuilder::new(&mut buf, 4);
        let (total, count) = b.finish();
        assert_eq!(count, 0);
        assert_eq!(total, 0);
    }

    // -----------------------------------------------------------------------
    //  Hello payload tests
    // -----------------------------------------------------------------------

    #[test]
    fn hello_roundtrip() {
        let h = Hello {
            layout_version: 1,
            flags: 0,
            supported_profiles: PROFILE_BASELINE | PROFILE_SHM_FUTEX,
            preferred_profiles: PROFILE_SHM_FUTEX,
            max_request_payload_bytes: 4096,
            max_request_batch_items: 100,
            max_response_payload_bytes: 1048576,
            max_response_batch_items: 1,
            auth_token: 0xAABB_CCDD_EEFF_0011,
            packet_size: 65536,
        };

        let mut buf = [0u8; 64];
        let n = h.encode(&mut buf);
        assert_eq!(n, 44);

        let out = Hello::decode(&buf[..n]).unwrap();
        assert_eq!(out, h);
    }

    #[test]
    fn hello_decode_truncated() {
        let buf = [0u8; 43];
        assert_eq!(Hello::decode(&buf), Err(NipcError::Truncated));
    }

    #[test]
    fn hello_decode_bad_layout() {
        let h = Hello {
            layout_version: 99,
            ..Default::default()
        };
        let mut buf = [0u8; 44];
        h.encode(&mut buf);
        assert_eq!(Hello::decode(&buf), Err(NipcError::BadLayout));
    }

    #[test]
    fn hello_encode_too_small() {
        let h = Hello::default();
        let mut buf = [0u8; 10];
        assert_eq!(h.encode(&mut buf), 0);
    }

    // -----------------------------------------------------------------------
    //  Hello-ack payload tests
    // -----------------------------------------------------------------------

    #[test]
    fn hello_ack_roundtrip() {
        let h = HelloAck {
            layout_version: 1,
            flags: 0,
            server_supported_profiles: 0x07,
            intersection_profiles: 0x05,
            selected_profile: PROFILE_SHM_FUTEX,
            agreed_max_request_payload_bytes: 2048,
            agreed_max_request_batch_items: 50,
            agreed_max_response_payload_bytes: 65536,
            agreed_max_response_batch_items: 1,
            agreed_packet_size: 32768,
        };

        let mut buf = [0u8; 64];
        let n = h.encode(&mut buf);
        assert_eq!(n, 36);

        let out = HelloAck::decode(&buf[..n]).unwrap();
        assert_eq!(out, h);
    }

    #[test]
    fn hello_ack_decode_truncated() {
        let buf = [0u8; 35];
        assert_eq!(HelloAck::decode(&buf), Err(NipcError::Truncated));
    }

    #[test]
    fn hello_ack_decode_bad_layout() {
        let h = HelloAck {
            layout_version: 0,
            ..Default::default()
        };
        let mut buf = [0u8; 36];
        h.encode(&mut buf);
        assert_eq!(HelloAck::decode(&buf), Err(NipcError::BadLayout));
    }

    #[test]
    fn hello_ack_encode_too_small() {
        let h = HelloAck::default();
        let mut buf = [0u8; 10];
        assert_eq!(h.encode(&mut buf), 0);
    }

    // -----------------------------------------------------------------------
    //  Cgroups snapshot request tests
    // -----------------------------------------------------------------------

    #[test]
    fn cgroups_req_roundtrip() {
        let r = CgroupsRequest {
            layout_version: 1,
            flags: 0,
        };

        let mut buf = [0u8; 16];
        let n = r.encode(&mut buf);
        assert_eq!(n, 4);

        let out = CgroupsRequest::decode(&buf[..n]).unwrap();
        assert_eq!(out, r);
    }

    #[test]
    fn cgroups_req_decode_truncated() {
        let buf = [0u8; 3];
        assert_eq!(CgroupsRequest::decode(&buf), Err(NipcError::Truncated));
    }

    #[test]
    fn cgroups_req_decode_bad_layout() {
        let r = CgroupsRequest {
            layout_version: 5,
            flags: 0,
        };
        let mut buf = [0u8; 4];
        r.encode(&mut buf);
        assert_eq!(CgroupsRequest::decode(&buf), Err(NipcError::BadLayout));
    }

    #[test]
    fn cgroups_req_encode_too_small() {
        let r = CgroupsRequest::default();
        let mut buf = [0u8; 2];
        assert_eq!(r.encode(&mut buf), 0);
    }

    // -----------------------------------------------------------------------
    //  Cgroups snapshot response tests
    // -----------------------------------------------------------------------

    #[test]
    fn cgroups_resp_empty() {
        let mut buf = [0u8; 4096];
        let b = CgroupsBuilder::new(&mut buf, 0, 1, 42);
        let total = b.finish();
        assert_eq!(total, 24);

        let view = CgroupsResponseView::decode(&buf[..total]).unwrap();
        assert_eq!(view.item_count, 0);
        assert_eq!(view.systemd_enabled, 1);
        assert_eq!(view.generation, 42);
    }

    #[test]
    fn cgroups_resp_single_item() {
        let mut buf = [0u8; 4096];
        let mut b = CgroupsBuilder::new(&mut buf, 1, 0, 100);

        let name = b"docker-abc123";
        let path = b"/sys/fs/cgroup/docker/abc123";
        b.add(12345, 0x01, 1, name, path).unwrap();

        let total = b.finish();
        assert!(total > 24);

        let view = CgroupsResponseView::decode(&buf[..total]).unwrap();
        assert_eq!(view.item_count, 1);
        assert_eq!(view.systemd_enabled, 0);
        assert_eq!(view.generation, 100);

        let item = view.item(0).unwrap();
        assert_eq!(item.hash, 12345);
        assert_eq!(item.options, 0x01);
        assert_eq!(item.enabled, 1);
        assert_eq!(item.name.len as usize, name.len());
        assert_eq!(item.name.as_bytes(), name);
        assert_eq!(item.name.bytes[name.len()], 0); // NUL
        assert_eq!(item.path.len as usize, path.len());
        assert_eq!(item.path.as_bytes(), path);
        assert_eq!(item.path.bytes[path.len()], 0); // NUL
    }

    #[test]
    fn cgroups_resp_multiple_items() {
        let mut buf = [0u8; 8192];
        let mut b = CgroupsBuilder::new(&mut buf, 5, 1, 999);

        // Item 0
        let n0 = b"init.scope";
        let p0 = b"/sys/fs/cgroup/init.scope";
        b.add(100, 0, 1, n0, p0).unwrap();

        // Item 1
        let n1 = b"system.slice/docker-abc.scope";
        let p1 = b"/sys/fs/cgroup/system.slice/docker-abc.scope";
        b.add(200, 0x02, 0, n1, p1).unwrap();

        // Item 2 - empty strings
        b.add(300, 0, 1, b"", b"").unwrap();

        let total = b.finish();

        let view = CgroupsResponseView::decode(&buf[..total]).unwrap();
        assert_eq!(view.item_count, 3);
        assert_eq!(view.systemd_enabled, 1);
        assert_eq!(view.generation, 999);

        // Verify item 0
        let item = view.item(0).unwrap();
        assert_eq!(item.hash, 100);
        assert_eq!(item.name.len as usize, n0.len());
        assert_eq!(item.name.as_bytes(), n0);
        assert_eq!(item.path.len as usize, p0.len());
        assert_eq!(item.path.as_bytes(), p0);

        // Verify item 1
        let item = view.item(1).unwrap();
        assert_eq!(item.hash, 200);
        assert_eq!(item.options, 0x02);
        assert_eq!(item.enabled, 0);
        assert_eq!(item.name.len as usize, n1.len());
        assert_eq!(item.name.as_bytes(), n1);

        // Verify item 2 (empty strings)
        let item = view.item(2).unwrap();
        assert_eq!(item.hash, 300);
        assert_eq!(item.name.len, 0);
        assert_eq!(item.name.bytes[0], 0); // NUL
        assert_eq!(item.path.len, 0);
        assert_eq!(item.path.bytes[0], 0); // NUL

        // Out-of-bounds index
        assert_eq!(view.item(3), Err(NipcError::OutOfBounds));
    }

    #[test]
    fn cgroups_resp_decode_truncated_header() {
        let buf = [0u8; 23];
        assert_eq!(
            CgroupsResponseView::decode(&buf).unwrap_err(),
            NipcError::Truncated
        );
    }

    #[test]
    fn cgroups_resp_decode_bad_layout() {
        let mut buf = [0u8; 24];
        buf[0..2].copy_from_slice(&99u16.to_le_bytes());
        assert_eq!(
            CgroupsResponseView::decode(&buf).unwrap_err(),
            NipcError::BadLayout
        );
    }

    #[test]
    fn cgroups_resp_decode_truncated_dir() {
        // Header says item_count=2 but payload is only 24 bytes
        let mut buf = [0u8; 24];
        buf[0..2].copy_from_slice(&1u16.to_le_bytes());
        buf[4..8].copy_from_slice(&2u32.to_le_bytes());
        assert_eq!(
            CgroupsResponseView::decode(&buf).unwrap_err(),
            NipcError::Truncated
        );
    }

    #[test]
    fn cgroups_resp_decode_oob_dir() {
        // Header + 1 dir entry pointing beyond payload
        let mut buf = [0u8; 64];
        buf[0..2].copy_from_slice(&1u16.to_le_bytes());
        buf[4..8].copy_from_slice(&1u32.to_le_bytes());
        // Dir entry at offset 24: offset=0, length=9999
        buf[24..28].copy_from_slice(&0u32.to_le_bytes());
        buf[28..32].copy_from_slice(&9999u32.to_le_bytes());
        assert_eq!(
            CgroupsResponseView::decode(&buf).unwrap_err(),
            NipcError::OutOfBounds
        );
    }

    #[test]
    fn cgroups_resp_decode_item_too_small() {
        // Dir entry with length < 32
        let mut buf = [0u8; 64];
        buf[0..2].copy_from_slice(&1u16.to_le_bytes());
        buf[4..8].copy_from_slice(&1u32.to_le_bytes());
        buf[24..28].copy_from_slice(&0u32.to_le_bytes());
        buf[28..32].copy_from_slice(&16u32.to_le_bytes());
        assert_eq!(
            CgroupsResponseView::decode(&buf).unwrap_err(),
            NipcError::Truncated
        );
    }

    #[test]
    fn cgroups_resp_item_missing_nul() {
        // Build valid snapshot then corrupt the NUL terminator
        let mut buf = [0u8; 4096];
        let mut b = CgroupsBuilder::new(&mut buf, 1, 0, 1);
        b.add(1, 0, 1, b"test", b"/test").unwrap();
        let total = b.finish();

        // Find item data and corrupt the name's NUL terminator
        let dir_end = CGROUPS_RESP_HDR_SIZE + 1 * CGROUPS_DIR_ENTRY_SIZE;
        let item_off = u32::from_le_bytes(
            buf[CGROUPS_RESP_HDR_SIZE..CGROUPS_RESP_HDR_SIZE + 4]
                .try_into()
                .unwrap(),
        ) as usize;
        let item_start = dir_end + item_off;

        let noff = u32::from_le_bytes(
            buf[item_start + 16..item_start + 20].try_into().unwrap(),
        ) as usize;
        let nlen = u32::from_le_bytes(
            buf[item_start + 20..item_start + 24].try_into().unwrap(),
        ) as usize;

        buf[item_start + noff + nlen] = b'X'; // corrupt NUL

        // Re-decode after corruption -- header/dir still valid
        let view = CgroupsResponseView::decode(&buf[..total]).unwrap();
        assert_eq!(view.item(0).unwrap_err(), NipcError::MissingNul);
    }

    #[test]
    fn cgroups_resp_item_string_oob() {
        // Build valid snapshot then corrupt string length to be huge
        let mut buf = [0u8; 4096];
        let mut b = CgroupsBuilder::new(&mut buf, 1, 0, 1);
        b.add(1, 0, 1, b"test", b"/test").unwrap();
        let total = b.finish();

        // Corrupt name_length to huge value
        let dir_end = CGROUPS_RESP_HDR_SIZE + 1 * CGROUPS_DIR_ENTRY_SIZE;
        let item_off = u32::from_le_bytes(
            buf[CGROUPS_RESP_HDR_SIZE..CGROUPS_RESP_HDR_SIZE + 4]
                .try_into()
                .unwrap(),
        ) as usize;
        let item_start = dir_end + item_off;

        buf[item_start + 20..item_start + 24]
            .copy_from_slice(&99999u32.to_le_bytes());

        // Re-decode after corruption
        let view = CgroupsResponseView::decode(&buf[..total]).unwrap();
        assert_eq!(view.item(0).unwrap_err(), NipcError::OutOfBounds);
    }

    #[test]
    fn cgroups_builder_overflow() {
        let mut buf = [0u8; 64]; // too small for any real item
        let mut b = CgroupsBuilder::new(&mut buf, 1, 0, 0);
        let long_name = [b'A'; 200];
        assert_eq!(
            b.add(1, 0, 1, &long_name, b""),
            Err(NipcError::Overflow)
        );
    }

    #[test]
    fn cgroups_builder_max_items_exceeded() {
        let mut buf = [0u8; 4096];
        let mut b = CgroupsBuilder::new(&mut buf, 1, 0, 0);
        b.add(1, 0, 1, b"a", b"b").unwrap();
        assert_eq!(
            b.add(2, 0, 1, b"c", b"d"),
            Err(NipcError::Overflow)
        );
    }

    #[test]
    fn cgroups_builder_compaction() {
        let mut buf = [0u8; 4096];
        // Reserve 10 directory slots but only add 2 items
        let mut b = CgroupsBuilder::new(&mut buf, 10, 1, 77);

        b.add(10, 0, 1, b"slice-a", b"/cgroup/slice-a").unwrap();
        b.add(20, 0, 0, b"slice-b", b"/cgroup/slice-b").unwrap();

        let total = b.finish();

        let view = CgroupsResponseView::decode(&buf[..total]).unwrap();
        assert_eq!(view.item_count, 2);
        assert_eq!(view.generation, 77);

        let item = view.item(0).unwrap();
        assert_eq!(item.hash, 10);
        assert_eq!(item.name.as_bytes(), b"slice-a");

        let item = view.item(1).unwrap();
        assert_eq!(item.hash, 20);
        assert_eq!(item.name.as_bytes(), b"slice-b");
    }

    // -----------------------------------------------------------------------
    //  Alignment utility test
    // -----------------------------------------------------------------------

    #[test]
    fn test_align8() {
        assert_eq!(align8(0), 0);
        assert_eq!(align8(1), 8);
        assert_eq!(align8(7), 8);
        assert_eq!(align8(8), 8);
        assert_eq!(align8(9), 16);
        assert_eq!(align8(16), 16);
        assert_eq!(align8(17), 24);
    }

    // -----------------------------------------------------------------------
    //  Cross-language wire compatibility: C-Rust byte identity
    //
    //  These tests encode in Rust and verify the exact bytes match what the
    //  C implementation produces for the same inputs. This ensures identical
    //  wire output across languages.
    // -----------------------------------------------------------------------

    #[test]
    fn c_rust_header_bytes_identical() {
        // Encode in Rust
        let h = Header {
            magic: MAGIC_MSG,
            version: VERSION,
            header_len: HEADER_LEN,
            kind: KIND_REQUEST,
            flags: FLAG_BATCH,
            code: METHOD_CGROUPS_SNAPSHOT,
            transport_status: STATUS_OK,
            payload_len: 12345,
            item_count: 42,
            message_id: 0xDEAD_BEEF_CAFE_BABE,
        };
        let mut rust_buf = [0u8; 32];
        h.encode(&mut rust_buf);

        // Known LE bytes for this header
        let expected: [u8; 32] = [
            0x43, 0x50, 0x49, 0x4e, // magic
            0x01, 0x00,             // version
            0x20, 0x00,             // header_len
            0x01, 0x00,             // kind
            0x01, 0x00,             // flags
            0x02, 0x00,             // code
            0x00, 0x00,             // transport_status
            0x39, 0x30, 0x00, 0x00, // payload_len = 12345
            0x2a, 0x00, 0x00, 0x00, // item_count = 42
            0xbe, 0xba, 0xfe, 0xca, 0xef, 0xbe, 0xad, 0xde, // message_id
        ];
        assert_eq!(rust_buf, expected);
    }

    #[test]
    fn c_rust_chunk_bytes_identical() {
        let c = ChunkHeader {
            magic: MAGIC_CHUNK,
            version: VERSION,
            flags: 0,
            message_id: 1,
            total_message_len: 256,
            chunk_index: 1,
            chunk_count: 3,
            chunk_payload_len: 100,
        };
        let mut rust_buf = [0u8; 32];
        c.encode(&mut rust_buf);

        let expected: [u8; 32] = [
            0x4b, 0x48, 0x43, 0x4e, // magic
            0x01, 0x00,             // version
            0x00, 0x00,             // flags
            0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // message_id
            0x00, 0x01, 0x00, 0x00, // total_message_len = 256
            0x01, 0x00, 0x00, 0x00, // chunk_index
            0x03, 0x00, 0x00, 0x00, // chunk_count
            0x64, 0x00, 0x00, 0x00, // chunk_payload_len = 100
        ];
        assert_eq!(rust_buf, expected);
    }

    #[test]
    fn c_rust_hello_bytes_identical() {
        let h = Hello {
            layout_version: 1,
            flags: 0,
            supported_profiles: PROFILE_BASELINE | PROFILE_SHM_FUTEX,
            preferred_profiles: PROFILE_SHM_FUTEX,
            max_request_payload_bytes: 4096,
            max_request_batch_items: 100,
            max_response_payload_bytes: 1048576,
            max_response_batch_items: 1,
            auth_token: 0xAABB_CCDD_EEFF_0011,
            packet_size: 65536,
        };

        let mut rust_buf = [0u8; 44];
        h.encode(&mut rust_buf);

        // Verify key byte positions
        assert_eq!(&rust_buf[0..2], &[0x01, 0x00]); // layout_version
        assert_eq!(&rust_buf[2..4], &[0x00, 0x00]); // flags
        assert_eq!(&rust_buf[4..8], &[0x05, 0x00, 0x00, 0x00]); // supported = 0x05
        assert_eq!(&rust_buf[8..12], &[0x04, 0x00, 0x00, 0x00]); // preferred = 0x04
        assert_eq!(&rust_buf[28..32], &[0x00, 0x00, 0x00, 0x00]); // padding = 0
        assert_eq!(
            &rust_buf[32..40],
            &[0x11, 0x00, 0xFF, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA]
        ); // auth_token

        // Round-trip
        let out = Hello::decode(&rust_buf).unwrap();
        assert_eq!(out, h);
    }

    #[test]
    fn c_rust_hello_ack_bytes_identical() {
        let h = HelloAck {
            layout_version: 1,
            flags: 0,
            server_supported_profiles: 0x07,
            intersection_profiles: 0x05,
            selected_profile: PROFILE_SHM_FUTEX,
            agreed_max_request_payload_bytes: 2048,
            agreed_max_request_batch_items: 50,
            agreed_max_response_payload_bytes: 65536,
            agreed_max_response_batch_items: 1,
            agreed_packet_size: 32768,
        };
        let mut rust_buf = [0u8; 36];
        h.encode(&mut rust_buf);

        assert_eq!(&rust_buf[0..2], &[0x01, 0x00]);
        assert_eq!(&rust_buf[4..8], &[0x07, 0x00, 0x00, 0x00]); // server_supported
        assert_eq!(&rust_buf[12..16], &[0x04, 0x00, 0x00, 0x00]); // selected = SHM_FUTEX

        let out = HelloAck::decode(&rust_buf).unwrap();
        assert_eq!(out, h);
    }

    #[test]
    fn c_rust_cgroups_req_bytes_identical() {
        let r = CgroupsRequest {
            layout_version: 1,
            flags: 0,
        };
        let mut rust_buf = [0u8; 4];
        r.encode(&mut rust_buf);

        assert_eq!(rust_buf, [0x01, 0x00, 0x00, 0x00]);

        let out = CgroupsRequest::decode(&rust_buf).unwrap();
        assert_eq!(out, r);
    }

    #[test]
    fn c_rust_cgroups_snapshot_bytes_identical() {
        // Build a snapshot with the exact same inputs as the C test
        let mut buf = [0u8; 4096];
        let mut b = CgroupsBuilder::new(&mut buf, 1, 0, 100);
        b.add(12345, 0x01, 1, b"docker-abc123", b"/sys/fs/cgroup/docker/abc123")
            .unwrap();
        let total = b.finish();

        // Verify the snapshot header bytes
        assert_eq!(&buf[0..2], &[0x01, 0x00]); // layout_version
        assert_eq!(&buf[2..4], &[0x00, 0x00]); // flags
        assert_eq!(&buf[4..8], &[0x01, 0x00, 0x00, 0x00]); // item_count
        assert_eq!(&buf[8..12], &[0x00, 0x00, 0x00, 0x00]); // systemd_enabled
        assert_eq!(&buf[12..16], &[0x00, 0x00, 0x00, 0x00]); // reserved
        assert_eq!(&buf[16..24], &[0x64, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00]); // generation

        // Verify it decodes correctly
        let view = CgroupsResponseView::decode(&buf[..total]).unwrap();
        assert_eq!(view.item_count, 1);
        assert_eq!(view.generation, 100);

        let item = view.item(0).unwrap();
        assert_eq!(item.hash, 12345);
        assert_eq!(item.name.as_bytes(), b"docker-abc123");
        assert_eq!(item.path.as_bytes(), b"/sys/fs/cgroup/docker/abc123");
    }

    #[test]
    fn cgroups_resp_dir_bad_alignment() {
        // Dir entry with unaligned offset
        let mut buf = [0u8; 128];
        buf[0..2].copy_from_slice(&1u16.to_le_bytes());
        buf[4..8].copy_from_slice(&1u32.to_le_bytes());
        // offset=3 (not 8-byte aligned), length=32
        buf[24..28].copy_from_slice(&3u32.to_le_bytes());
        buf[28..32].copy_from_slice(&32u32.to_le_bytes());
        assert_eq!(
            CgroupsResponseView::decode(&buf).unwrap_err(),
            NipcError::BadAlignment
        );
    }

    #[test]
    fn cgroups_resp_item_bad_layout_version() {
        // Build valid snapshot, then corrupt the item's layout_version
        let mut buf = [0u8; 4096];
        let mut b = CgroupsBuilder::new(&mut buf, 1, 0, 1);
        b.add(1, 0, 1, b"test", b"/test").unwrap();
        let total = b.finish();

        // Corrupt item layout_version
        let dir_end = CGROUPS_RESP_HDR_SIZE + 1 * CGROUPS_DIR_ENTRY_SIZE;
        let item_off = u32::from_le_bytes(
            buf[CGROUPS_RESP_HDR_SIZE..CGROUPS_RESP_HDR_SIZE + 4]
                .try_into()
                .unwrap(),
        ) as usize;
        let item_start = dir_end + item_off;
        buf[item_start..item_start + 2].copy_from_slice(&99u16.to_le_bytes());

        // Re-decode after corruption
        let view = CgroupsResponseView::decode(&buf[..total]).unwrap();
        assert_eq!(view.item(0).unwrap_err(), NipcError::BadLayout);
    }

    #[test]
    fn cgroups_resp_item_name_off_below_header() {
        // Build valid snapshot, then set name_offset < 32
        let mut buf = [0u8; 4096];
        let mut b = CgroupsBuilder::new(&mut buf, 1, 0, 1);
        b.add(1, 0, 1, b"test", b"/test").unwrap();
        let total = b.finish();

        let dir_end = CGROUPS_RESP_HDR_SIZE + 1 * CGROUPS_DIR_ENTRY_SIZE;
        let item_off = u32::from_le_bytes(
            buf[CGROUPS_RESP_HDR_SIZE..CGROUPS_RESP_HDR_SIZE + 4]
                .try_into()
                .unwrap(),
        ) as usize;
        let item_start = dir_end + item_off;
        // Set name_offset to 0 (below header)
        buf[item_start + 16..item_start + 20].copy_from_slice(&0u32.to_le_bytes());

        let view = CgroupsResponseView::decode(&buf[..total]).unwrap();
        assert_eq!(view.item(0).unwrap_err(), NipcError::OutOfBounds);
    }

    #[test]
    fn cgroups_resp_item_path_off_below_header() {
        let mut buf = [0u8; 4096];
        let mut b = CgroupsBuilder::new(&mut buf, 1, 0, 1);
        b.add(1, 0, 1, b"test", b"/test").unwrap();
        let total = b.finish();

        let dir_end = CGROUPS_RESP_HDR_SIZE + 1 * CGROUPS_DIR_ENTRY_SIZE;
        let item_off = u32::from_le_bytes(
            buf[CGROUPS_RESP_HDR_SIZE..CGROUPS_RESP_HDR_SIZE + 4]
                .try_into()
                .unwrap(),
        ) as usize;
        let item_start = dir_end + item_off;
        // Set path_offset to 16 (below header)
        buf[item_start + 24..item_start + 28].copy_from_slice(&16u32.to_le_bytes());

        let view = CgroupsResponseView::decode(&buf[..total]).unwrap();
        assert_eq!(view.item(0).unwrap_err(), NipcError::OutOfBounds);
    }

    #[test]
    fn cgroups_resp_item_path_missing_nul() {
        let mut buf = [0u8; 4096];
        let mut b = CgroupsBuilder::new(&mut buf, 1, 0, 1);
        b.add(1, 0, 1, b"test", b"/test").unwrap();
        let total = b.finish();

        // Corrupt path NUL
        let dir_end = CGROUPS_RESP_HDR_SIZE + 1 * CGROUPS_DIR_ENTRY_SIZE;
        let item_off = u32::from_le_bytes(
            buf[CGROUPS_RESP_HDR_SIZE..CGROUPS_RESP_HDR_SIZE + 4]
                .try_into()
                .unwrap(),
        ) as usize;
        let item_start = dir_end + item_off;
        let poff = u32::from_le_bytes(
            buf[item_start + 24..item_start + 28].try_into().unwrap(),
        ) as usize;
        let plen = u32::from_le_bytes(
            buf[item_start + 28..item_start + 32].try_into().unwrap(),
        ) as usize;
        buf[item_start + poff + plen] = b'X';

        // Re-decode after corruption
        let view = CgroupsResponseView::decode(&buf[..total]).unwrap();
        assert_eq!(view.item(0).unwrap_err(), NipcError::MissingNul);
    }

    // -------------------------------------------------------------------
    //  Proptest: fuzz / property-based tests for all decode paths
    // -------------------------------------------------------------------

    mod proptests {
        use super::*;
        use proptest::prelude::*;

        // Arbitrary bytes -- no decode path may panic on any input.

        proptest! {
            #[test]
            fn decode_header_never_panics(data: Vec<u8>) {
                let _ = Header::decode(&data);
            }

            #[test]
            fn decode_chunk_header_never_panics(data: Vec<u8>) {
                let _ = ChunkHeader::decode(&data);
            }

            #[test]
            fn decode_hello_never_panics(data: Vec<u8>) {
                let _ = Hello::decode(&data);
            }

            #[test]
            fn decode_hello_ack_never_panics(data: Vec<u8>) {
                let _ = HelloAck::decode(&data);
            }

            #[test]
            fn decode_cgroups_request_never_panics(data: Vec<u8>) {
                let _ = CgroupsRequest::decode(&data);
            }

            #[test]
            fn decode_cgroups_response_never_panics(data: Vec<u8>) {
                let result = CgroupsResponseView::decode(&data);
                if let Ok(view) = result {
                    // Exercise item access on valid decodes.
                    let limit = view.item_count.min(64);
                    for i in 0..limit {
                        let _ = view.item(i);
                    }
                    // Out-of-bounds must not panic.
                    let _ = view.item(view.item_count);
                }
            }

            #[test]
            fn batch_dir_decode_never_panics(
                data: Vec<u8>,
                item_count in 0u32..128,
                packed_area_len in 0u32..65536,
            ) {
                let _ = batch_dir_decode(&data, item_count, packed_area_len);
            }

            #[test]
            fn batch_item_get_never_panics(
                data: Vec<u8>,
                item_count in 0u32..128,
                index in 0u32..128,
            ) {
                let _ = batch_item_get(&data, item_count, index);
            }
        }

        // Roundtrip tests: encode random valid values, decode, verify match.

        proptest! {
            #[test]
            fn encode_decode_header_roundtrip(
                kind in 1u16..=3,
                flags in any::<u16>(),
                code in any::<u16>(),
                transport_status in any::<u16>(),
                payload_len in any::<u32>(),
                item_count in any::<u32>(),
                message_id in any::<u64>(),
            ) {
                let h = Header {
                    magic: MAGIC_MSG,
                    version: VERSION,
                    header_len: HEADER_LEN,
                    kind,
                    flags,
                    code,
                    transport_status,
                    payload_len,
                    item_count,
                    message_id,
                };
                let mut buf = [0u8; 64];
                let n = h.encode(&mut buf);
                prop_assert_eq!(n, HEADER_SIZE);
                let decoded = Header::decode(&buf[..n]).unwrap();
                prop_assert_eq!(decoded, h);
            }

            #[test]
            fn encode_decode_hello_roundtrip(
                supported in any::<u32>(),
                preferred in any::<u32>(),
                max_req_payload in any::<u32>(),
                max_req_batch in any::<u32>(),
                max_resp_payload in any::<u32>(),
                max_resp_batch in any::<u32>(),
                auth_token in any::<u64>(),
                packet_size in any::<u32>(),
            ) {
                let h = Hello {
                    layout_version: 1,
                    flags: 0,
                    supported_profiles: supported,
                    preferred_profiles: preferred,
                    max_request_payload_bytes: max_req_payload,
                    max_request_batch_items: max_req_batch,
                    max_response_payload_bytes: max_resp_payload,
                    max_response_batch_items: max_resp_batch,
                    auth_token,
                    packet_size,
                };
                let mut buf = [0u8; 64];
                let n = h.encode(&mut buf);
                prop_assert_eq!(n, HELLO_SIZE);
                let decoded = Hello::decode(&buf[..n]).unwrap();
                prop_assert_eq!(decoded, h);
            }
        }
    }
}

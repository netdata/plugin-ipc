//! Wire protocol definitions shared by the Rust crate.

use std::io;

pub const MESSAGE_MAGIC: u32 = 0x4e49_5043;
pub const MESSAGE_VERSION: u16 = 1;
pub const MESSAGE_HEADER_LEN: usize = 32;
pub const MESSAGE_ITEM_REF_LEN: usize = 8;
pub const MESSAGE_ITEM_ALIGNMENT: usize = 8;
pub const MESSAGE_FLAG_BATCH: u16 = 0x0001;

pub const MESSAGE_KIND_REQUEST: u16 = 1;
pub const MESSAGE_KIND_RESPONSE: u16 = 2;
pub const MESSAGE_KIND_CONTROL: u16 = 3;

pub const CONTROL_HELLO: u16 = 1;
pub const CONTROL_HELLO_ACK: u16 = 2;
pub const CONTROL_HELLO_PAYLOAD_LEN: usize = 40;
pub const CONTROL_HELLO_ACK_PAYLOAD_LEN: usize = 32;

pub const TRANSPORT_STATUS_OK: u16 = 0;
pub const TRANSPORT_STATUS_BAD_ENVELOPE: u16 = 1;
pub const TRANSPORT_STATUS_AUTH_FAILED: u16 = 2;
pub const TRANSPORT_STATUS_INCOMPATIBLE: u16 = 3;
pub const TRANSPORT_STATUS_UNSUPPORTED: u16 = 4;
pub const TRANSPORT_STATUS_LIMIT_EXCEEDED: u16 = 5;
pub const TRANSPORT_STATUS_INTERNAL_ERROR: u16 = 6;

pub const MAX_PAYLOAD_DEFAULT: u32 = 1024;

// Legacy fixed-frame increment API stays exported while transports migrate.
pub const FRAME_MAGIC: u32 = MESSAGE_MAGIC;
pub const FRAME_VERSION: u16 = MESSAGE_VERSION;
pub const FRAME_SIZE: usize = 64;
pub const FRAME_KIND_REQUEST: u16 = MESSAGE_KIND_REQUEST;
pub const FRAME_KIND_RESPONSE: u16 = MESSAGE_KIND_RESPONSE;

pub const METHOD_INCREMENT: u16 = 1;
pub const METHOD_CGROUPS_SNAPSHOT: u16 = 2;
pub const INCREMENT_PAYLOAD_LEN: u32 = 12;

pub const CGROUPS_SNAPSHOT_LAYOUT_VERSION: u16 = 1;
pub const CGROUPS_SNAPSHOT_REQUEST_PAYLOAD_LEN: usize = 4;
pub const CGROUPS_SNAPSHOT_RESPONSE_HEADER_LEN: usize = 16;
pub const CGROUPS_SNAPSHOT_ITEM_HEADER_LEN: usize = 32;

pub const STATUS_OK: i32 = 0;
pub const STATUS_BAD_REQUEST: i32 = 1;
pub const STATUS_INTERNAL_ERROR: i32 = 2;

const OFF_MAGIC: usize = 0;
const OFF_VERSION: usize = 4;
const OFF_HEADER_LEN: usize = 6;
const OFF_KIND: usize = 8;
const OFF_FLAGS: usize = 10;
const OFF_CODE: usize = 12;
const OFF_TRANSPORT_STATUS: usize = 14;
const OFF_PAYLOAD_LEN: usize = 16;
const OFF_ITEM_COUNT: usize = 20;
const OFF_MESSAGE_ID: usize = 24;

const OFF_ITEM_REF_OFFSET: usize = 0;
const OFF_ITEM_REF_LENGTH: usize = 4;

const OFF_HELLO_LAYOUT_VERSION: usize = 0;
const OFF_HELLO_FLAGS: usize = 2;
const OFF_HELLO_SUPPORTED_PROFILES: usize = 4;
const OFF_HELLO_PREFERRED_PROFILES: usize = 8;
const OFF_HELLO_MAX_REQUEST_PAYLOAD_BYTES: usize = 12;
const OFF_HELLO_MAX_REQUEST_BATCH_ITEMS: usize = 16;
const OFF_HELLO_MAX_RESPONSE_PAYLOAD_BYTES: usize = 20;
const OFF_HELLO_MAX_RESPONSE_BATCH_ITEMS: usize = 24;
const OFF_HELLO_AUTH_TOKEN: usize = 32;

const OFF_HELLO_ACK_LAYOUT_VERSION: usize = 0;
const OFF_HELLO_ACK_FLAGS: usize = 2;
const OFF_HELLO_ACK_SERVER_SUPPORTED_PROFILES: usize = 4;
const OFF_HELLO_ACK_INTERSECTION_PROFILES: usize = 8;
const OFF_HELLO_ACK_SELECTED_PROFILE: usize = 12;
const OFF_HELLO_ACK_AGREED_MAX_REQUEST_PAYLOAD_BYTES: usize = 16;
const OFF_HELLO_ACK_AGREED_MAX_REQUEST_BATCH_ITEMS: usize = 20;
const OFF_HELLO_ACK_AGREED_MAX_RESPONSE_PAYLOAD_BYTES: usize = 24;
const OFF_HELLO_ACK_AGREED_MAX_RESPONSE_BATCH_ITEMS: usize = 28;

const OFF_CGROUPS_SNAPSHOT_REQ_LAYOUT_VERSION: usize = 0;
const OFF_CGROUPS_SNAPSHOT_REQ_FLAGS: usize = 2;

const OFF_CGROUPS_SNAPSHOT_RESP_LAYOUT_VERSION: usize = 0;
const OFF_CGROUPS_SNAPSHOT_RESP_FLAGS: usize = 2;
const OFF_CGROUPS_SNAPSHOT_RESP_SYSTEMD_ENABLED: usize = 4;
const OFF_CGROUPS_SNAPSHOT_RESP_GENERATION: usize = 8;

const OFF_CGROUPS_SNAPSHOT_ITEM_LAYOUT_VERSION: usize = 0;
const OFF_CGROUPS_SNAPSHOT_ITEM_FLAGS: usize = 2;
const OFF_CGROUPS_SNAPSHOT_ITEM_HASH: usize = 4;
const OFF_CGROUPS_SNAPSHOT_ITEM_OPTIONS: usize = 8;
const OFF_CGROUPS_SNAPSHOT_ITEM_ENABLED: usize = 12;
const OFF_CGROUPS_SNAPSHOT_ITEM_NAME_OFF: usize = 16;
const OFF_CGROUPS_SNAPSHOT_ITEM_NAME_LEN: usize = 20;
const OFF_CGROUPS_SNAPSHOT_ITEM_PATH_OFF: usize = 24;
const OFF_CGROUPS_SNAPSHOT_ITEM_PATH_LEN: usize = 28;

const OFF_VALUE: usize = 32;
const OFF_STATUS: usize = 40;

pub type Frame = [u8; FRAME_SIZE];

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct MessageHeader {
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

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ItemRef {
    pub offset: u32,
    pub length: u32,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct HelloPayload {
    pub layout_version: u16,
    pub flags: u16,
    pub supported_profiles: u32,
    pub preferred_profiles: u32,
    pub max_request_payload_bytes: u32,
    pub max_request_batch_items: u32,
    pub max_response_payload_bytes: u32,
    pub max_response_batch_items: u32,
    pub auth_token: u64,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct HelloAckPayload {
    pub layout_version: u16,
    pub flags: u16,
    pub server_supported_profiles: u32,
    pub intersection_profiles: u32,
    pub selected_profile: u32,
    pub agreed_max_request_payload_bytes: u32,
    pub agreed_max_request_batch_items: u32,
    pub agreed_max_response_payload_bytes: u32,
    pub agreed_max_response_batch_items: u32,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct IncrementRequest {
    pub value: u64,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct IncrementResponse {
    pub status: i32,
    pub value: u64,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CgroupsSnapshotRequest {
    pub flags: u16,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CgroupsSnapshotRequestView {
    pub layout_version: u16,
    pub flags: u16,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CStringView<'a> {
    bytes: &'a [u8],
}

impl<'a> CStringView<'a> {
    pub fn as_bytes(&self) -> &'a [u8] {
        self.bytes
    }

    pub fn len(&self) -> usize {
        self.bytes.len()
    }

    pub fn is_empty(&self) -> bool {
        self.bytes.is_empty()
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CgroupsSnapshotItem<'a> {
    pub hash: u32,
    pub options: u32,
    pub enabled: bool,
    pub name: &'a [u8],
    pub path: &'a [u8],
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CgroupsSnapshotItemView<'a> {
    pub layout_version: u16,
    pub flags: u16,
    pub hash: u32,
    pub options: u32,
    pub enabled: bool,
    pub name_view: CStringView<'a>,
    pub path_view: CStringView<'a>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CgroupsSnapshotView<'a> {
    pub layout_version: u16,
    pub flags: u16,
    pub generation: u64,
    pub systemd_enabled: bool,
    pub item_count: u32,
    item_refs: &'a [u8],
    items: &'a [u8],
}

#[derive(Debug)]
pub struct CgroupsSnapshotResponseBuilder<'a> {
    buf: &'a mut [u8],
    payload_len: usize,
    packed_offset: usize,
    expected_items: u32,
    item_count: u32,
    flags: u16,
    systemd_enabled: bool,
    generation: u64,
}

fn protocol_error(message: &'static str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, message)
}

fn write_u16_le(buf: &mut [u8], off: usize, value: u16) {
    buf[off..off + 2].copy_from_slice(&value.to_le_bytes());
}

fn write_u32_le(buf: &mut [u8], off: usize, value: u32) {
    buf[off..off + 4].copy_from_slice(&value.to_le_bytes());
}

fn write_u64_le(buf: &mut [u8], off: usize, value: u64) {
    buf[off..off + 8].copy_from_slice(&value.to_le_bytes());
}

fn read_u16_le(buf: &[u8], off: usize) -> u16 {
    u16::from_le_bytes([buf[off], buf[off + 1]])
}

fn read_u32_le(buf: &[u8], off: usize) -> u32 {
    u32::from_le_bytes([buf[off], buf[off + 1], buf[off + 2], buf[off + 3]])
}

fn read_u64_le(buf: &[u8], off: usize) -> u64 {
    u64::from_le_bytes([
        buf[off],
        buf[off + 1],
        buf[off + 2],
        buf[off + 3],
        buf[off + 4],
        buf[off + 5],
        buf[off + 6],
        buf[off + 7],
    ])
}

fn read_i32_le(buf: &[u8], off: usize) -> i32 {
    i32::from_le_bytes([buf[off], buf[off + 1], buf[off + 2], buf[off + 3]])
}

fn validate_message_header(header: &MessageHeader) -> io::Result<()> {
    if header.magic != MESSAGE_MAGIC || header.version != MESSAGE_VERSION {
        return Err(protocol_error("invalid message magic/version"));
    }
    if header.header_len as usize != MESSAGE_HEADER_LEN {
        return Err(protocol_error("invalid message header_len"));
    }
    match header.kind {
        MESSAGE_KIND_REQUEST | MESSAGE_KIND_RESPONSE | MESSAGE_KIND_CONTROL => {}
        _ => return Err(protocol_error("invalid message kind")),
    }
    Ok(())
}

pub fn message_total_size(header: &MessageHeader) -> io::Result<usize> {
    validate_message_header(header)?;
    MESSAGE_HEADER_LEN
        .checked_add(header.payload_len as usize)
        .ok_or_else(|| protocol_error("message size overflow"))
}

pub fn aligned_item_size(payload_len: u32) -> io::Result<usize> {
    let payload_len = payload_len as usize;
    let rem = payload_len % MESSAGE_ITEM_ALIGNMENT;
    if rem == 0 {
        return Ok(payload_len);
    }
    payload_len
        .checked_add(MESSAGE_ITEM_ALIGNMENT - rem)
        .ok_or_else(|| protocol_error("aligned item size overflow"))
}

pub fn max_batch_payload_len(max_item_payload_len: u32, max_items: u32) -> io::Result<usize> {
    if max_item_payload_len == 0 || max_items == 0 {
        return Err(protocol_error("invalid negotiated max payload/items"));
    }

    let max_items = max_items as usize;
    let table_len = max_items
        .checked_mul(MESSAGE_ITEM_REF_LEN)
        .ok_or_else(|| protocol_error("item-ref table size overflow"))?;
    let aligned_item_len = aligned_item_size(max_item_payload_len)?;
    let payload_area_len = aligned_item_len
        .checked_mul(max_items)
        .ok_or_else(|| protocol_error("payload area size overflow"))?;

    table_len
        .checked_add(payload_area_len)
        .ok_or_else(|| protocol_error("batch payload size overflow"))
}

pub fn max_batch_total_size(max_item_payload_len: u32, max_items: u32) -> io::Result<usize> {
    let payload_len = max_batch_payload_len(max_item_payload_len, max_items)?;
    MESSAGE_HEADER_LEN
        .checked_add(payload_len)
        .ok_or_else(|| protocol_error("batch total size overflow"))
}

pub fn encode_message_header(header: &MessageHeader) -> io::Result<[u8; MESSAGE_HEADER_LEN]> {
    validate_message_header(header)?;

    let mut buf = [0u8; MESSAGE_HEADER_LEN];
    write_u32_le(&mut buf, OFF_MAGIC, header.magic);
    write_u16_le(&mut buf, OFF_VERSION, header.version);
    write_u16_le(&mut buf, OFF_HEADER_LEN, header.header_len);
    write_u16_le(&mut buf, OFF_KIND, header.kind);
    write_u16_le(&mut buf, OFF_FLAGS, header.flags);
    write_u16_le(&mut buf, OFF_CODE, header.code);
    write_u16_le(&mut buf, OFF_TRANSPORT_STATUS, header.transport_status);
    write_u32_le(&mut buf, OFF_PAYLOAD_LEN, header.payload_len);
    write_u32_le(&mut buf, OFF_ITEM_COUNT, header.item_count);
    write_u64_le(&mut buf, OFF_MESSAGE_ID, header.message_id);
    Ok(buf)
}

pub fn decode_message_header(buf: &[u8]) -> io::Result<MessageHeader> {
    if buf.len() < MESSAGE_HEADER_LEN {
        return Err(protocol_error("short message header"));
    }

    let header = MessageHeader {
        magic: read_u32_le(buf, OFF_MAGIC),
        version: read_u16_le(buf, OFF_VERSION),
        header_len: read_u16_le(buf, OFF_HEADER_LEN),
        kind: read_u16_le(buf, OFF_KIND),
        flags: read_u16_le(buf, OFF_FLAGS),
        code: read_u16_le(buf, OFF_CODE),
        transport_status: read_u16_le(buf, OFF_TRANSPORT_STATUS),
        payload_len: read_u32_le(buf, OFF_PAYLOAD_LEN),
        item_count: read_u32_le(buf, OFF_ITEM_COUNT),
        message_id: read_u64_le(buf, OFF_MESSAGE_ID),
    };
    validate_message_header(&header)?;
    Ok(header)
}

pub fn encode_item_refs(refs: &[ItemRef]) -> Vec<u8> {
    let mut buf = vec![0u8; refs.len() * MESSAGE_ITEM_REF_LEN];
    for (idx, item_ref) in refs.iter().enumerate() {
        let off = idx * MESSAGE_ITEM_REF_LEN;
        write_u32_le(&mut buf, off + OFF_ITEM_REF_OFFSET, item_ref.offset);
        write_u32_le(&mut buf, off + OFF_ITEM_REF_LENGTH, item_ref.length);
    }
    buf
}

pub fn decode_item_refs(buf: &[u8], item_count: usize) -> io::Result<Vec<ItemRef>> {
    if buf.len() < item_count * MESSAGE_ITEM_REF_LEN {
        return Err(protocol_error("short item-ref table"));
    }

    let mut refs = Vec::with_capacity(item_count);
    for idx in 0..item_count {
        let off = idx * MESSAGE_ITEM_REF_LEN;
        refs.push(ItemRef {
            offset: read_u32_le(buf, off + OFF_ITEM_REF_OFFSET),
            length: read_u32_le(buf, off + OFF_ITEM_REF_LENGTH),
        });
    }
    Ok(refs)
}

pub fn encode_hello_payload(payload: &HelloPayload) -> [u8; CONTROL_HELLO_PAYLOAD_LEN] {
    let mut buf = [0u8; CONTROL_HELLO_PAYLOAD_LEN];
    write_u16_le(&mut buf, OFF_HELLO_LAYOUT_VERSION, payload.layout_version);
    write_u16_le(&mut buf, OFF_HELLO_FLAGS, payload.flags);
    write_u32_le(
        &mut buf,
        OFF_HELLO_SUPPORTED_PROFILES,
        payload.supported_profiles,
    );
    write_u32_le(
        &mut buf,
        OFF_HELLO_PREFERRED_PROFILES,
        payload.preferred_profiles,
    );
    write_u32_le(
        &mut buf,
        OFF_HELLO_MAX_REQUEST_PAYLOAD_BYTES,
        payload.max_request_payload_bytes,
    );
    write_u32_le(
        &mut buf,
        OFF_HELLO_MAX_REQUEST_BATCH_ITEMS,
        payload.max_request_batch_items,
    );
    write_u32_le(
        &mut buf,
        OFF_HELLO_MAX_RESPONSE_PAYLOAD_BYTES,
        payload.max_response_payload_bytes,
    );
    write_u32_le(
        &mut buf,
        OFF_HELLO_MAX_RESPONSE_BATCH_ITEMS,
        payload.max_response_batch_items,
    );
    write_u64_le(&mut buf, OFF_HELLO_AUTH_TOKEN, payload.auth_token);
    buf
}

pub fn decode_hello_payload(buf: &[u8]) -> io::Result<HelloPayload> {
    if buf.len() < CONTROL_HELLO_PAYLOAD_LEN {
        return Err(protocol_error("short hello payload"));
    }

    Ok(HelloPayload {
        layout_version: read_u16_le(buf, OFF_HELLO_LAYOUT_VERSION),
        flags: read_u16_le(buf, OFF_HELLO_FLAGS),
        supported_profiles: read_u32_le(buf, OFF_HELLO_SUPPORTED_PROFILES),
        preferred_profiles: read_u32_le(buf, OFF_HELLO_PREFERRED_PROFILES),
        max_request_payload_bytes: read_u32_le(buf, OFF_HELLO_MAX_REQUEST_PAYLOAD_BYTES),
        max_request_batch_items: read_u32_le(buf, OFF_HELLO_MAX_REQUEST_BATCH_ITEMS),
        max_response_payload_bytes: read_u32_le(buf, OFF_HELLO_MAX_RESPONSE_PAYLOAD_BYTES),
        max_response_batch_items: read_u32_le(buf, OFF_HELLO_MAX_RESPONSE_BATCH_ITEMS),
        auth_token: read_u64_le(buf, OFF_HELLO_AUTH_TOKEN),
    })
}

pub fn encode_hello_ack_payload(payload: &HelloAckPayload) -> [u8; CONTROL_HELLO_ACK_PAYLOAD_LEN] {
    let mut buf = [0u8; CONTROL_HELLO_ACK_PAYLOAD_LEN];
    write_u16_le(
        &mut buf,
        OFF_HELLO_ACK_LAYOUT_VERSION,
        payload.layout_version,
    );
    write_u16_le(&mut buf, OFF_HELLO_ACK_FLAGS, payload.flags);
    write_u32_le(
        &mut buf,
        OFF_HELLO_ACK_SERVER_SUPPORTED_PROFILES,
        payload.server_supported_profiles,
    );
    write_u32_le(
        &mut buf,
        OFF_HELLO_ACK_INTERSECTION_PROFILES,
        payload.intersection_profiles,
    );
    write_u32_le(
        &mut buf,
        OFF_HELLO_ACK_SELECTED_PROFILE,
        payload.selected_profile,
    );
    write_u32_le(
        &mut buf,
        OFF_HELLO_ACK_AGREED_MAX_REQUEST_PAYLOAD_BYTES,
        payload.agreed_max_request_payload_bytes,
    );
    write_u32_le(
        &mut buf,
        OFF_HELLO_ACK_AGREED_MAX_REQUEST_BATCH_ITEMS,
        payload.agreed_max_request_batch_items,
    );
    write_u32_le(
        &mut buf,
        OFF_HELLO_ACK_AGREED_MAX_RESPONSE_PAYLOAD_BYTES,
        payload.agreed_max_response_payload_bytes,
    );
    write_u32_le(
        &mut buf,
        OFF_HELLO_ACK_AGREED_MAX_RESPONSE_BATCH_ITEMS,
        payload.agreed_max_response_batch_items,
    );
    buf
}

pub fn decode_hello_ack_payload(buf: &[u8]) -> io::Result<HelloAckPayload> {
    if buf.len() < CONTROL_HELLO_ACK_PAYLOAD_LEN {
        return Err(protocol_error("short hello-ack payload"));
    }

    Ok(HelloAckPayload {
        layout_version: read_u16_le(buf, OFF_HELLO_ACK_LAYOUT_VERSION),
        flags: read_u16_le(buf, OFF_HELLO_ACK_FLAGS),
        server_supported_profiles: read_u32_le(buf, OFF_HELLO_ACK_SERVER_SUPPORTED_PROFILES),
        intersection_profiles: read_u32_le(buf, OFF_HELLO_ACK_INTERSECTION_PROFILES),
        selected_profile: read_u32_le(buf, OFF_HELLO_ACK_SELECTED_PROFILE),
        agreed_max_request_payload_bytes: read_u32_le(
            buf,
            OFF_HELLO_ACK_AGREED_MAX_REQUEST_PAYLOAD_BYTES,
        ),
        agreed_max_request_batch_items: read_u32_le(
            buf,
            OFF_HELLO_ACK_AGREED_MAX_REQUEST_BATCH_ITEMS,
        ),
        agreed_max_response_payload_bytes: read_u32_le(
            buf,
            OFF_HELLO_ACK_AGREED_MAX_RESPONSE_PAYLOAD_BYTES,
        ),
        agreed_max_response_batch_items: read_u32_le(
            buf,
            OFF_HELLO_ACK_AGREED_MAX_RESPONSE_BATCH_ITEMS,
        ),
    })
}

pub fn cgroups_snapshot_item_payload_len(item: &CgroupsSnapshotItem<'_>) -> io::Result<usize> {
    let with_name = CGROUPS_SNAPSHOT_ITEM_HEADER_LEN
        .checked_add(item.name.len())
        .and_then(|n| n.checked_add(1))
        .ok_or_else(|| protocol_error("cgroups snapshot item size overflow"))?;
    with_name
        .checked_add(item.path.len())
        .and_then(|n| n.checked_add(1))
        .ok_or_else(|| protocol_error("cgroups snapshot item size overflow"))
}

pub fn encode_cgroups_snapshot_request_payload(
    request: &CgroupsSnapshotRequest,
) -> [u8; CGROUPS_SNAPSHOT_REQUEST_PAYLOAD_LEN] {
    let mut buf = [0u8; CGROUPS_SNAPSHOT_REQUEST_PAYLOAD_LEN];
    write_u16_le(
        &mut buf,
        OFF_CGROUPS_SNAPSHOT_REQ_LAYOUT_VERSION,
        CGROUPS_SNAPSHOT_LAYOUT_VERSION,
    );
    write_u16_le(&mut buf, OFF_CGROUPS_SNAPSHOT_REQ_FLAGS, request.flags);
    buf
}

pub fn decode_cgroups_snapshot_request_view(buf: &[u8]) -> io::Result<CgroupsSnapshotRequestView> {
    if buf.len() < CGROUPS_SNAPSHOT_REQUEST_PAYLOAD_LEN {
        return Err(protocol_error("short cgroups snapshot request payload"));
    }

    let view = CgroupsSnapshotRequestView {
        layout_version: read_u16_le(buf, OFF_CGROUPS_SNAPSHOT_REQ_LAYOUT_VERSION),
        flags: read_u16_le(buf, OFF_CGROUPS_SNAPSHOT_REQ_FLAGS),
    };
    if view.layout_version != CGROUPS_SNAPSHOT_LAYOUT_VERSION {
        return Err(protocol_error(
            "invalid cgroups snapshot request layout version",
        ));
    }
    Ok(view)
}

fn validate_cstring_view<'a>(payload: &'a [u8], off: u32, len: u32) -> io::Result<CStringView<'a>> {
    let off = off as usize;
    let len = len as usize;
    if off < CGROUPS_SNAPSHOT_ITEM_HEADER_LEN {
        return Err(protocol_error(
            "cgroups snapshot string overlaps item header",
        ));
    }

    let end = off
        .checked_add(len)
        .and_then(|n| n.checked_add(1))
        .ok_or_else(|| protocol_error("cgroups snapshot string overflow"))?;
    if end > payload.len() {
        return Err(protocol_error("cgroups snapshot string out of bounds"));
    }
    if payload[off + len] != 0 {
        return Err(protocol_error(
            "cgroups snapshot string is not NUL terminated",
        ));
    }
    Ok(CStringView {
        bytes: &payload[off..off + len],
    })
}

impl<'a> CgroupsSnapshotView<'a> {
    pub fn item_view_at(&self, index: u32) -> io::Result<CgroupsSnapshotItemView<'a>> {
        if index >= self.item_count {
            return Err(protocol_error("cgroups snapshot item index out of bounds"));
        }

        let base = (index as usize)
            .checked_mul(MESSAGE_ITEM_REF_LEN)
            .ok_or_else(|| protocol_error("item-ref index overflow"))?;
        let item_ref = ItemRef {
            offset: read_u32_le(self.item_refs, base + OFF_ITEM_REF_OFFSET),
            length: read_u32_le(self.item_refs, base + OFF_ITEM_REF_LENGTH),
        };
        if (item_ref.offset as usize) % MESSAGE_ITEM_ALIGNMENT != 0 {
            return Err(protocol_error(
                "cgroups snapshot item offset is not aligned",
            ));
        }

        let item_start = item_ref.offset as usize;
        let item_end = item_start
            .checked_add(item_ref.length as usize)
            .ok_or_else(|| protocol_error("cgroups snapshot item overflow"))?;
        if item_end > self.items.len()
            || (item_ref.length as usize) < CGROUPS_SNAPSHOT_ITEM_HEADER_LEN
        {
            return Err(protocol_error("cgroups snapshot item out of bounds"));
        }

        let payload = &self.items[item_start..item_end];
        let view = CgroupsSnapshotItemView {
            layout_version: read_u16_le(payload, OFF_CGROUPS_SNAPSHOT_ITEM_LAYOUT_VERSION),
            flags: read_u16_le(payload, OFF_CGROUPS_SNAPSHOT_ITEM_FLAGS),
            hash: read_u32_le(payload, OFF_CGROUPS_SNAPSHOT_ITEM_HASH),
            options: read_u32_le(payload, OFF_CGROUPS_SNAPSHOT_ITEM_OPTIONS),
            enabled: read_u32_le(payload, OFF_CGROUPS_SNAPSHOT_ITEM_ENABLED) != 0,
            name_view: validate_cstring_view(
                payload,
                read_u32_le(payload, OFF_CGROUPS_SNAPSHOT_ITEM_NAME_OFF),
                read_u32_le(payload, OFF_CGROUPS_SNAPSHOT_ITEM_NAME_LEN),
            )?,
            path_view: validate_cstring_view(
                payload,
                read_u32_le(payload, OFF_CGROUPS_SNAPSHOT_ITEM_PATH_OFF),
                read_u32_le(payload, OFF_CGROUPS_SNAPSHOT_ITEM_PATH_LEN),
            )?,
        };
        if view.layout_version != CGROUPS_SNAPSHOT_LAYOUT_VERSION {
            return Err(protocol_error(
                "invalid cgroups snapshot item layout version",
            ));
        }
        Ok(view)
    }
}

pub fn decode_cgroups_snapshot_view<'a>(
    payload: &'a [u8],
    item_count: u32,
) -> io::Result<CgroupsSnapshotView<'a>> {
    let refs_len = (item_count as usize)
        .checked_mul(MESSAGE_ITEM_REF_LEN)
        .ok_or_else(|| protocol_error("cgroups snapshot item-ref table overflow"))?;
    let packed_offset = CGROUPS_SNAPSHOT_RESPONSE_HEADER_LEN
        .checked_add(refs_len)
        .ok_or_else(|| protocol_error("cgroups snapshot payload overflow"))?;
    if payload.len() < packed_offset {
        return Err(protocol_error("short cgroups snapshot response payload"));
    }

    let view = CgroupsSnapshotView {
        layout_version: read_u16_le(payload, OFF_CGROUPS_SNAPSHOT_RESP_LAYOUT_VERSION),
        flags: read_u16_le(payload, OFF_CGROUPS_SNAPSHOT_RESP_FLAGS),
        systemd_enabled: read_u32_le(payload, OFF_CGROUPS_SNAPSHOT_RESP_SYSTEMD_ENABLED) != 0,
        generation: read_u64_le(payload, OFF_CGROUPS_SNAPSHOT_RESP_GENERATION),
        item_count,
        item_refs: &payload[CGROUPS_SNAPSHOT_RESPONSE_HEADER_LEN..packed_offset],
        items: &payload[packed_offset..],
    };
    if view.layout_version != CGROUPS_SNAPSHOT_LAYOUT_VERSION {
        return Err(protocol_error(
            "invalid cgroups snapshot response layout version",
        ));
    }

    for index in 0..item_count {
        view.item_view_at(index)?;
    }
    Ok(view)
}

impl<'a> CgroupsSnapshotResponseBuilder<'a> {
    pub fn new(
        buf: &'a mut [u8],
        generation: u64,
        systemd_enabled: bool,
        flags: u16,
        expected_items: u32,
    ) -> io::Result<Self> {
        let table_len = (expected_items as usize)
            .checked_mul(MESSAGE_ITEM_REF_LEN)
            .ok_or_else(|| protocol_error("item-ref table size overflow"))?;
        let packed_offset = CGROUPS_SNAPSHOT_RESPONSE_HEADER_LEN
            .checked_add(table_len)
            .ok_or_else(|| protocol_error("snapshot header overflow"))?;
        if buf.len() < packed_offset {
            return Err(protocol_error("snapshot builder buffer is too small"));
        }
        buf[..packed_offset].fill(0);
        Ok(Self {
            buf,
            payload_len: packed_offset,
            packed_offset,
            expected_items,
            item_count: 0,
            flags,
            systemd_enabled,
            generation,
        })
    }

    pub fn push_item(&mut self, item: &CgroupsSnapshotItem<'_>) -> io::Result<()> {
        if self.item_count >= self.expected_items {
            return Err(protocol_error("too many cgroups snapshot items"));
        }

        let item_len = cgroups_snapshot_item_payload_len(item)?;
        let start = aligned_item_size(self.payload_len as u32)?;
        let end = start
            .checked_add(item_len)
            .ok_or_else(|| protocol_error("snapshot item end overflow"))?;
        if end > self.buf.len() {
            return Err(protocol_error("snapshot builder buffer is too small"));
        }
        if start > self.payload_len {
            self.buf[self.payload_len..start].fill(0);
        }

        let item_buf = &mut self.buf[start..end];
        item_buf.fill(0);
        let name_off = CGROUPS_SNAPSHOT_ITEM_HEADER_LEN as u32;
        let path_off = name_off
            .checked_add(item.name.len() as u32)
            .and_then(|n| n.checked_add(1))
            .ok_or_else(|| protocol_error("snapshot item name offset overflow"))?;

        write_u16_le(
            item_buf,
            OFF_CGROUPS_SNAPSHOT_ITEM_LAYOUT_VERSION,
            CGROUPS_SNAPSHOT_LAYOUT_VERSION,
        );
        write_u16_le(item_buf, OFF_CGROUPS_SNAPSHOT_ITEM_FLAGS, 0);
        write_u32_le(item_buf, OFF_CGROUPS_SNAPSHOT_ITEM_HASH, item.hash);
        write_u32_le(item_buf, OFF_CGROUPS_SNAPSHOT_ITEM_OPTIONS, item.options);
        write_u32_le(
            item_buf,
            OFF_CGROUPS_SNAPSHOT_ITEM_ENABLED,
            if item.enabled { 1 } else { 0 },
        );
        write_u32_le(item_buf, OFF_CGROUPS_SNAPSHOT_ITEM_NAME_OFF, name_off);
        write_u32_le(
            item_buf,
            OFF_CGROUPS_SNAPSHOT_ITEM_NAME_LEN,
            item.name.len() as u32,
        );
        write_u32_le(item_buf, OFF_CGROUPS_SNAPSHOT_ITEM_PATH_OFF, path_off);
        write_u32_le(
            item_buf,
            OFF_CGROUPS_SNAPSHOT_ITEM_PATH_LEN,
            item.path.len() as u32,
        );

        item_buf
            [CGROUPS_SNAPSHOT_ITEM_HEADER_LEN..CGROUPS_SNAPSHOT_ITEM_HEADER_LEN + item.name.len()]
            .copy_from_slice(item.name);
        item_buf[CGROUPS_SNAPSHOT_ITEM_HEADER_LEN + item.name.len()] = 0;
        let path_start = path_off as usize;
        item_buf[path_start..path_start + item.path.len()].copy_from_slice(item.path);
        item_buf[path_start + item.path.len()] = 0;

        let entry_base = CGROUPS_SNAPSHOT_RESPONSE_HEADER_LEN
            + (self.item_count as usize * MESSAGE_ITEM_REF_LEN);
        write_u32_le(
            self.buf,
            entry_base + OFF_ITEM_REF_OFFSET,
            (start - self.packed_offset) as u32,
        );
        write_u32_le(self.buf, entry_base + OFF_ITEM_REF_LENGTH, item_len as u32);
        self.item_count += 1;
        self.payload_len = end;
        Ok(())
    }

    pub fn finish(self) -> io::Result<usize> {
        if self.item_count != self.expected_items {
            return Err(protocol_error(
                "snapshot builder finished with missing items",
            ));
        }

        write_u16_le(
            self.buf,
            OFF_CGROUPS_SNAPSHOT_RESP_LAYOUT_VERSION,
            CGROUPS_SNAPSHOT_LAYOUT_VERSION,
        );
        write_u16_le(self.buf, OFF_CGROUPS_SNAPSHOT_RESP_FLAGS, self.flags);
        write_u32_le(
            self.buf,
            OFF_CGROUPS_SNAPSHOT_RESP_SYSTEMD_ENABLED,
            if self.systemd_enabled { 1 } else { 0 },
        );
        write_u64_le(
            self.buf,
            OFF_CGROUPS_SNAPSHOT_RESP_GENERATION,
            self.generation,
        );
        Ok(self.payload_len)
    }
}

fn encode_base(kind: u16, message_id: u64) -> Frame {
    let header = MessageHeader {
        magic: MESSAGE_MAGIC,
        version: MESSAGE_VERSION,
        header_len: MESSAGE_HEADER_LEN as u16,
        kind,
        flags: 0,
        code: METHOD_INCREMENT,
        transport_status: TRANSPORT_STATUS_OK,
        payload_len: INCREMENT_PAYLOAD_LEN,
        item_count: 1,
        message_id,
    };
    let mut frame = [0u8; FRAME_SIZE];
    frame[..MESSAGE_HEADER_LEN].copy_from_slice(
        &encode_message_header(&header).expect("legacy increment header must encode"),
    );
    frame
}

fn validate_legacy_increment_header(frame: &Frame, expected_kind: u16) -> io::Result<()> {
    let header = decode_message_header(frame)?;
    if header.kind != expected_kind
        || header.code != METHOD_INCREMENT
        || header.payload_len != INCREMENT_PAYLOAD_LEN
        || header.item_count != 1
        || header.flags != 0
        || header.transport_status != TRANSPORT_STATUS_OK
    {
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
    validate_legacy_increment_header(frame, FRAME_KIND_REQUEST)?;
    Ok((
        read_u64_le(frame, OFF_MESSAGE_ID),
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
    validate_legacy_increment_header(frame, FRAME_KIND_RESPONSE)?;
    Ok((
        read_u64_le(frame, OFF_MESSAGE_ID),
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
    fn message_header_roundtrip() {
        let encoded = encode_message_header(&MessageHeader {
            magic: MESSAGE_MAGIC,
            version: MESSAGE_VERSION,
            header_len: MESSAGE_HEADER_LEN as u16,
            kind: MESSAGE_KIND_CONTROL,
            flags: MESSAGE_FLAG_BATCH,
            code: CONTROL_HELLO_ACK,
            transport_status: TRANSPORT_STATUS_LIMIT_EXCEEDED,
            payload_len: 96,
            item_count: 4,
            message_id: 77,
        })
        .expect("encode header");
        let decoded = decode_message_header(&encoded).expect("decode header");
        assert_eq!(decoded.kind, MESSAGE_KIND_CONTROL);
        assert_eq!(decoded.flags, MESSAGE_FLAG_BATCH);
        assert_eq!(decoded.code, CONTROL_HELLO_ACK);
        assert_eq!(decoded.transport_status, TRANSPORT_STATUS_LIMIT_EXCEEDED);
        assert_eq!(decoded.payload_len, 96);
        assert_eq!(decoded.item_count, 4);
        assert_eq!(decoded.message_id, 77);
        assert_eq!(message_total_size(&decoded).expect("message size"), 128);
    }

    #[test]
    fn rejects_short_message_header() {
        assert!(decode_message_header(&[0u8; MESSAGE_HEADER_LEN - 1]).is_err());
    }

    #[test]
    fn item_ref_roundtrip() {
        let refs = [
            ItemRef {
                offset: 8,
                length: 13,
            },
            ItemRef {
                offset: 24,
                length: 21,
            },
        ];
        let encoded = encode_item_refs(&refs);
        let decoded = decode_item_refs(&encoded, refs.len()).expect("decode item refs");
        assert_eq!(decoded, refs);
    }

    #[test]
    fn batch_size_helpers_follow_alignment_rules() {
        assert_eq!(aligned_item_size(1024).expect("aligned 1024"), 1024);
        assert_eq!(aligned_item_size(1025).expect("aligned 1025"), 1032);
        assert_eq!(
            max_batch_payload_len(1024, 1).expect("batch payload len"),
            1032
        );
        assert_eq!(
            max_batch_total_size(1024, 1).expect("batch total len"),
            1064
        );
        assert_eq!(
            max_batch_payload_len(1025, 2).expect("batch payload len"),
            2080
        );
        assert_eq!(
            max_batch_total_size(1025, 2).expect("batch total len"),
            2112
        );
    }

    #[test]
    fn hello_roundtrip() {
        let encoded = encode_hello_payload(&HelloPayload {
            layout_version: 1,
            flags: 2,
            supported_profiles: 3,
            preferred_profiles: 4,
            max_request_payload_bytes: 5,
            max_request_batch_items: 6,
            max_response_payload_bytes: 7,
            max_response_batch_items: 8,
            auth_token: 9,
        });
        let decoded = decode_hello_payload(&encoded).expect("decode hello");
        assert_eq!(decoded.layout_version, 1);
        assert_eq!(decoded.flags, 2);
        assert_eq!(decoded.supported_profiles, 3);
        assert_eq!(decoded.preferred_profiles, 4);
        assert_eq!(decoded.max_request_payload_bytes, 5);
        assert_eq!(decoded.max_request_batch_items, 6);
        assert_eq!(decoded.max_response_payload_bytes, 7);
        assert_eq!(decoded.max_response_batch_items, 8);
        assert_eq!(decoded.auth_token, 9);
    }

    #[test]
    fn hello_ack_roundtrip() {
        let encoded = encode_hello_ack_payload(&HelloAckPayload {
            layout_version: 1,
            flags: 2,
            server_supported_profiles: 3,
            intersection_profiles: 4,
            selected_profile: 5,
            agreed_max_request_payload_bytes: 6,
            agreed_max_request_batch_items: 7,
            agreed_max_response_payload_bytes: 8,
            agreed_max_response_batch_items: 9,
        });
        let decoded = decode_hello_ack_payload(&encoded).expect("decode hello ack");
        assert_eq!(decoded.layout_version, 1);
        assert_eq!(decoded.flags, 2);
        assert_eq!(decoded.server_supported_profiles, 3);
        assert_eq!(decoded.intersection_profiles, 4);
        assert_eq!(decoded.selected_profile, 5);
        assert_eq!(decoded.agreed_max_request_payload_bytes, 6);
        assert_eq!(decoded.agreed_max_request_batch_items, 7);
        assert_eq!(decoded.agreed_max_response_payload_bytes, 8);
        assert_eq!(decoded.agreed_max_response_batch_items, 9);
    }

    #[test]
    fn cgroups_snapshot_request_roundtrip() {
        let encoded = encode_cgroups_snapshot_request_payload(&CgroupsSnapshotRequest { flags: 7 });
        let decoded =
            decode_cgroups_snapshot_request_view(&encoded).expect("decode cgroups request view");
        assert_eq!(decoded.layout_version, CGROUPS_SNAPSHOT_LAYOUT_VERSION);
        assert_eq!(decoded.flags, 7);
    }

    #[test]
    fn cgroups_snapshot_response_roundtrip() {
        let mut buf = vec![0u8; 512];
        let mut builder =
            CgroupsSnapshotResponseBuilder::new(&mut buf, 42, true, 3, 2).expect("create builder");
        builder
            .push_item(&CgroupsSnapshotItem {
                hash: 123,
                options: 0x2,
                enabled: true,
                name: b"system.slice-nginx",
                path: b"/sys/fs/cgroup/system.slice/nginx.service/cgroup.procs",
            })
            .expect("push first item");
        builder
            .push_item(&CgroupsSnapshotItem {
                hash: 456,
                options: 0x4,
                enabled: false,
                name: b"docker-1234",
                path: b"",
            })
            .expect("push second item");
        let payload_len = builder.finish().expect("finish builder");

        let decoded = decode_cgroups_snapshot_view(&buf[..payload_len], 2)
            .expect("decode cgroups snapshot view");
        assert_eq!(decoded.layout_version, CGROUPS_SNAPSHOT_LAYOUT_VERSION);
        assert_eq!(decoded.flags, 3);
        assert!(decoded.systemd_enabled);
        assert_eq!(decoded.generation, 42);
        assert_eq!(decoded.item_count, 2);

        let first = decoded.item_view_at(0).expect("decode first item");
        assert_eq!(first.hash, 123);
        assert_eq!(first.options, 0x2);
        assert!(first.enabled);
        assert_eq!(first.name_view.as_bytes(), b"system.slice-nginx");
        assert_eq!(
            first.path_view.as_bytes(),
            b"/sys/fs/cgroup/system.slice/nginx.service/cgroup.procs"
        );

        let second = decoded.item_view_at(1).expect("decode second item");
        assert_eq!(second.hash, 456);
        assert_eq!(second.options, 0x4);
        assert!(!second.enabled);
        assert_eq!(second.name_view.as_bytes(), b"docker-1234");
        assert!(second.path_view.is_empty());
    }

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

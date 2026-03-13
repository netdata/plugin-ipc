//! Netdata-aligned Rust crate for plugin IPC.
//!
//! The reusable crate lives here. Test fixtures and benchmark drivers live
//! outside `src/crates/` so the library surface stays clean.

pub mod protocol;
pub mod service;
pub mod transport;

pub use protocol::{
    decode_cgroups_snapshot_request_view, decode_cgroups_snapshot_view, decode_hello_ack_payload,
    decode_hello_payload, decode_increment_request, decode_increment_response, decode_item_refs,
    decode_message_header, encode_cgroups_snapshot_request_payload, encode_hello_ack_payload,
    encode_hello_payload, encode_increment_request, encode_increment_response, encode_item_refs,
    encode_message_header, message_total_size, CStringView, CgroupsSnapshotItem,
    CgroupsSnapshotItemView, CgroupsSnapshotRequest, CgroupsSnapshotRequestView,
    CgroupsSnapshotResponseBuilder, CgroupsSnapshotView, Frame, HelloAckPayload, HelloPayload,
    IncrementRequest, IncrementResponse, ItemRef, MessageHeader, CGROUPS_SNAPSHOT_ITEM_HEADER_LEN,
    CGROUPS_SNAPSHOT_LAYOUT_VERSION, CGROUPS_SNAPSHOT_REQUEST_PAYLOAD_LEN,
    CGROUPS_SNAPSHOT_RESPONSE_HEADER_LEN, CONTROL_HELLO, CONTROL_HELLO_ACK,
    CONTROL_HELLO_ACK_PAYLOAD_LEN, CONTROL_HELLO_PAYLOAD_LEN, FRAME_SIZE, MAX_PAYLOAD_DEFAULT,
    MESSAGE_FLAG_BATCH, MESSAGE_HEADER_LEN, MESSAGE_ITEM_REF_LEN, MESSAGE_KIND_CONTROL,
    MESSAGE_KIND_REQUEST, MESSAGE_KIND_RESPONSE, MESSAGE_MAGIC, MESSAGE_VERSION,
    METHOD_CGROUPS_SNAPSHOT, STATUS_BAD_REQUEST, STATUS_INTERNAL_ERROR, STATUS_OK,
    TRANSPORT_STATUS_AUTH_FAILED, TRANSPORT_STATUS_BAD_ENVELOPE, TRANSPORT_STATUS_INCOMPATIBLE,
    TRANSPORT_STATUS_INTERNAL_ERROR, TRANSPORT_STATUS_LIMIT_EXCEEDED, TRANSPORT_STATUS_OK,
    TRANSPORT_STATUS_UNSUPPORTED,
};
pub use service::cgroups_snapshot::{
    CgroupsSnapshotCache, CgroupsSnapshotCacheItem, CgroupsSnapshotClient,
    CgroupsSnapshotClientConfig, CGROUPS_SNAPSHOT_DEFAULT_MAX_RESPONSE_BATCH_ITEMS,
    CGROUPS_SNAPSHOT_DEFAULT_MAX_RESPONSE_PAYLOAD_BYTES, CGROUPS_SNAPSHOT_NAME_MAX_BYTES,
    CGROUPS_SNAPSHOT_PATH_MAX_BYTES,
};

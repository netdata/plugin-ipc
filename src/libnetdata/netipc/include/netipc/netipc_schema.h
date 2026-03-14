#ifndef NETIPC_SCHEMA_H
#define NETIPC_SCHEMA_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NETIPC_FRAME_MAGIC 0x4e495043u
#define NETIPC_FRAME_VERSION 1u
#define NETIPC_FRAME_SIZE 64u

#define NETIPC_MSG_MAGIC 0x4e495043u
#define NETIPC_MSG_VERSION 1u
#define NETIPC_MSG_HEADER_LEN 32u
#define NETIPC_MSG_ITEM_REF_LEN 8u
#define NETIPC_MSG_ITEM_ALIGNMENT 8u
#define NETIPC_MSG_FLAG_BATCH 0x0001u

#define NETIPC_CONTROL_HELLO_PAYLOAD_LEN 44u
#define NETIPC_CONTROL_HELLO_ACK_PAYLOAD_LEN 36u
#define NETIPC_CHUNK_MAGIC 0x4e43484bu
#define NETIPC_CHUNK_VERSION 1u
#define NETIPC_CHUNK_HEADER_LEN 32u

#define NETIPC_MAX_PAYLOAD_DEFAULT 1024u

enum netipc_msg_kind {
    NETIPC_MSG_KIND_REQUEST = 1,
    NETIPC_MSG_KIND_RESPONSE = 2,
    NETIPC_MSG_KIND_CONTROL = 3
};

enum netipc_control_code {
    NETIPC_CONTROL_HELLO = 1,
    NETIPC_CONTROL_HELLO_ACK = 2
};

enum netipc_transport_status {
    NETIPC_TRANSPORT_STATUS_OK = 0,
    NETIPC_TRANSPORT_STATUS_BAD_ENVELOPE = 1,
    NETIPC_TRANSPORT_STATUS_AUTH_FAILED = 2,
    NETIPC_TRANSPORT_STATUS_INCOMPATIBLE = 3,
    NETIPC_TRANSPORT_STATUS_UNSUPPORTED = 4,
    NETIPC_TRANSPORT_STATUS_LIMIT_EXCEEDED = 5,
    NETIPC_TRANSPORT_STATUS_INTERNAL_ERROR = 6
};

struct netipc_msg_header {
    uint32_t magic;
    uint16_t version;
    uint16_t header_len;
    uint16_t kind;
    uint16_t flags;
    uint16_t code;
    uint16_t transport_status;
    uint32_t payload_len;
    uint32_t item_count;
    uint64_t message_id;
};

struct netipc_item_ref {
    uint32_t offset;
    uint32_t length;
};

struct netipc_hello {
    uint16_t layout_version;
    uint16_t flags;
    uint32_t supported_profiles;
    uint32_t preferred_profiles;
    uint32_t max_request_payload_bytes;
    uint32_t max_request_batch_items;
    uint32_t max_response_payload_bytes;
    uint32_t max_response_batch_items;
    uint64_t auth_token;
    uint32_t packet_size;
};

struct netipc_hello_ack {
    uint16_t layout_version;
    uint16_t flags;
    uint32_t server_supported_profiles;
    uint32_t intersection_profiles;
    uint32_t selected_profile;
    uint32_t agreed_max_request_payload_bytes;
    uint32_t agreed_max_request_batch_items;
    uint32_t agreed_max_response_payload_bytes;
    uint32_t agreed_max_response_batch_items;
    uint32_t agreed_packet_size;
};

struct netipc_chunk_header {
    uint32_t magic;
    uint16_t version;
    uint16_t flags;
    uint64_t message_id;
    uint32_t total_message_len;
    uint32_t chunk_index;
    uint32_t chunk_count;
    uint32_t chunk_payload_len;
};

enum netipc_frame_kind {
    NETIPC_FRAME_KIND_REQUEST = 1,
    NETIPC_FRAME_KIND_RESPONSE = 2
};

enum netipc_method {
    NETIPC_METHOD_INCREMENT = 1,
    NETIPC_METHOD_CGROUPS_SNAPSHOT = 2
};

enum netipc_status {
    NETIPC_STATUS_OK = 0,
    NETIPC_STATUS_BAD_REQUEST = 1,
    NETIPC_STATUS_INTERNAL_ERROR = 2
};

size_t netipc_msg_total_size(const struct netipc_msg_header *header);

size_t netipc_msg_aligned_item_size(uint32_t payload_len);

size_t netipc_msg_max_batch_payload_len(uint32_t max_item_payload_len, uint32_t max_items);

size_t netipc_msg_max_batch_total_size(uint32_t max_item_payload_len, uint32_t max_items);

int netipc_encode_msg_header(uint8_t *dst,
                             size_t dst_len,
                             const struct netipc_msg_header *header);

int netipc_decode_msg_header(const uint8_t *src,
                             size_t src_len,
                             struct netipc_msg_header *header);

int netipc_encode_item_refs(uint8_t *dst,
                            size_t dst_len,
                            const struct netipc_item_ref *refs,
                            size_t count);

int netipc_decode_item_refs(const uint8_t *src,
                            size_t src_len,
                            struct netipc_item_ref *refs,
                            size_t count);

int netipc_encode_hello_payload(uint8_t *dst,
                                size_t dst_len,
                                const struct netipc_hello *hello);

int netipc_decode_hello_payload(const uint8_t *src,
                                size_t src_len,
                                struct netipc_hello *hello);

int netipc_encode_hello_ack_payload(uint8_t *dst,
                                    size_t dst_len,
                                    const struct netipc_hello_ack *hello_ack);

int netipc_decode_hello_ack_payload(const uint8_t *src,
                                    size_t src_len,
                                    struct netipc_hello_ack *hello_ack);

int netipc_encode_chunk_header(uint8_t *dst,
                               size_t dst_len,
                               const struct netipc_chunk_header *chunk);

int netipc_decode_chunk_header(const uint8_t *src,
                               size_t src_len,
                               struct netipc_chunk_header *chunk);

#define NETIPC_CGROUPS_SNAPSHOT_LAYOUT_VERSION 1u
#define NETIPC_CGROUPS_SNAPSHOT_REQUEST_PAYLOAD_LEN 4u
#define NETIPC_CGROUPS_SNAPSHOT_RESPONSE_HEADER_LEN 16u
#define NETIPC_CGROUPS_SNAPSHOT_ITEM_HEADER_LEN 32u

struct netipc_str_view {
    const char *ptr;
    uint32_t len;
};

struct netipc_cgroups_snapshot_request {
    uint16_t flags;
};

struct netipc_cgroups_snapshot_request_view {
    uint16_t layout_version;
    uint16_t flags;
};

struct netipc_cgroups_snapshot_item {
    uint32_t hash;
    uint32_t options;
    uint32_t enabled;
    const char *name;
    uint32_t name_len;
    const char *path;
    uint32_t path_len;
};

struct netipc_cgroups_snapshot_item_view {
    uint16_t layout_version;
    uint16_t flags;
    uint32_t hash;
    uint32_t options;
    uint32_t enabled;
    struct netipc_str_view name_view;
    struct netipc_str_view path_view;
};

struct netipc_cgroups_snapshot_view {
    uint16_t layout_version;
    uint16_t flags;
    uint64_t generation;
    uint32_t systemd_enabled;
    uint32_t item_count;
    const uint8_t *item_refs;
    size_t item_refs_len;
    const uint8_t *items;
    size_t items_len;
};

struct netipc_cgroups_snapshot_builder {
    uint8_t *payload;
    size_t payload_capacity;
    size_t payload_len;
    size_t packed_offset;
    uint32_t expected_items;
    uint32_t item_count;
    uint16_t flags;
    uint32_t systemd_enabled;
    uint64_t generation;
};

struct netipc_increment_request {
    uint64_t value;
};

struct netipc_increment_response {
    int32_t status;
    uint64_t value;
};

size_t netipc_cgroups_snapshot_item_payload_len(const struct netipc_cgroups_snapshot_item *item);

int netipc_encode_cgroups_snapshot_request_payload(
    uint8_t *dst,
    size_t dst_len,
    const struct netipc_cgroups_snapshot_request *request);

int netipc_decode_cgroups_snapshot_request_view(
    const uint8_t *src,
    size_t src_len,
    struct netipc_cgroups_snapshot_request_view *view);

int netipc_cgroups_snapshot_builder_init(struct netipc_cgroups_snapshot_builder *builder,
                                         uint8_t *payload,
                                         size_t payload_capacity,
                                         uint64_t generation,
                                         uint32_t systemd_enabled,
                                         uint16_t flags,
                                         uint32_t expected_items);

int netipc_cgroups_snapshot_builder_add_item(struct netipc_cgroups_snapshot_builder *builder,
                                             const struct netipc_cgroups_snapshot_item *item);

int netipc_cgroups_snapshot_builder_finish(struct netipc_cgroups_snapshot_builder *builder,
                                           size_t *out_payload_len);

int netipc_decode_cgroups_snapshot_view(const uint8_t *src,
                                        size_t src_len,
                                        uint32_t item_count,
                                        struct netipc_cgroups_snapshot_view *view);

int netipc_cgroups_snapshot_view_item_at(const struct netipc_cgroups_snapshot_view *view,
                                         uint32_t index,
                                         struct netipc_cgroups_snapshot_item_view *item_view);

int netipc_encode_increment_request(uint8_t frame[NETIPC_FRAME_SIZE],
                                    uint64_t request_id,
                                    const struct netipc_increment_request *req);

int netipc_decode_increment_request(const uint8_t frame[NETIPC_FRAME_SIZE],
                                    uint64_t *request_id,
                                    struct netipc_increment_request *req);

int netipc_encode_increment_response(uint8_t frame[NETIPC_FRAME_SIZE],
                                     uint64_t request_id,
                                     const struct netipc_increment_response *resp);

int netipc_decode_increment_response(const uint8_t frame[NETIPC_FRAME_SIZE],
                                     uint64_t *request_id,
                                     struct netipc_increment_response *resp);

#ifdef __cplusplus
}
#endif

#endif

#include <netipc/netipc_schema.h>

#include <errno.h>
#include <stddef.h>
#include <string.h>

#define NETIPC_OFFSET_MAGIC 0u
#define NETIPC_OFFSET_VERSION 4u
#define NETIPC_OFFSET_HEADER_LEN 6u
#define NETIPC_OFFSET_KIND 8u
#define NETIPC_OFFSET_FLAGS 10u
#define NETIPC_OFFSET_CODE 12u
#define NETIPC_OFFSET_TRANSPORT_STATUS 14u
#define NETIPC_OFFSET_PAYLOAD_LEN 16u
#define NETIPC_OFFSET_ITEM_COUNT 20u
#define NETIPC_OFFSET_MESSAGE_ID 24u

#define NETIPC_ITEM_REF_OFFSET 0u
#define NETIPC_ITEM_REF_LENGTH 4u

#define NETIPC_HELLO_OFFSET_LAYOUT_VERSION 0u
#define NETIPC_HELLO_OFFSET_FLAGS 2u
#define NETIPC_HELLO_OFFSET_SUPPORTED_PROFILES 4u
#define NETIPC_HELLO_OFFSET_PREFERRED_PROFILES 8u
#define NETIPC_HELLO_OFFSET_MAX_REQUEST_PAYLOAD_BYTES 12u
#define NETIPC_HELLO_OFFSET_MAX_REQUEST_BATCH_ITEMS 16u
#define NETIPC_HELLO_OFFSET_MAX_RESPONSE_PAYLOAD_BYTES 20u
#define NETIPC_HELLO_OFFSET_MAX_RESPONSE_BATCH_ITEMS 24u
#define NETIPC_HELLO_OFFSET_AUTH_TOKEN 32u

#define NETIPC_HELLO_ACK_OFFSET_LAYOUT_VERSION 0u
#define NETIPC_HELLO_ACK_OFFSET_FLAGS 2u
#define NETIPC_HELLO_ACK_OFFSET_SERVER_SUPPORTED_PROFILES 4u
#define NETIPC_HELLO_ACK_OFFSET_INTERSECTION_PROFILES 8u
#define NETIPC_HELLO_ACK_OFFSET_SELECTED_PROFILE 12u
#define NETIPC_HELLO_ACK_OFFSET_AGREED_MAX_REQUEST_PAYLOAD_BYTES 16u
#define NETIPC_HELLO_ACK_OFFSET_AGREED_MAX_REQUEST_BATCH_ITEMS 20u
#define NETIPC_HELLO_ACK_OFFSET_AGREED_MAX_RESPONSE_PAYLOAD_BYTES 24u
#define NETIPC_HELLO_ACK_OFFSET_AGREED_MAX_RESPONSE_BATCH_ITEMS 28u

#define NETIPC_CGROUPS_SNAPSHOT_REQ_OFFSET_LAYOUT_VERSION 0u
#define NETIPC_CGROUPS_SNAPSHOT_REQ_OFFSET_FLAGS 2u

#define NETIPC_CGROUPS_SNAPSHOT_RESP_OFFSET_LAYOUT_VERSION 0u
#define NETIPC_CGROUPS_SNAPSHOT_RESP_OFFSET_FLAGS 2u
#define NETIPC_CGROUPS_SNAPSHOT_RESP_OFFSET_SYSTEMD_ENABLED 4u
#define NETIPC_CGROUPS_SNAPSHOT_RESP_OFFSET_GENERATION 8u

#define NETIPC_CGROUPS_SNAPSHOT_ITEM_OFFSET_LAYOUT_VERSION 0u
#define NETIPC_CGROUPS_SNAPSHOT_ITEM_OFFSET_FLAGS 2u
#define NETIPC_CGROUPS_SNAPSHOT_ITEM_OFFSET_HASH 4u
#define NETIPC_CGROUPS_SNAPSHOT_ITEM_OFFSET_OPTIONS 8u
#define NETIPC_CGROUPS_SNAPSHOT_ITEM_OFFSET_ENABLED 12u
#define NETIPC_CGROUPS_SNAPSHOT_ITEM_OFFSET_NAME_OFF 16u
#define NETIPC_CGROUPS_SNAPSHOT_ITEM_OFFSET_NAME_LEN 20u
#define NETIPC_CGROUPS_SNAPSHOT_ITEM_OFFSET_PATH_OFF 24u
#define NETIPC_CGROUPS_SNAPSHOT_ITEM_OFFSET_PATH_LEN 28u

#define NETIPC_INCREMENT_PAYLOAD_LEN 12u
#define NETIPC_OFFSET_VALUE 32u
#define NETIPC_OFFSET_STATUS 40u

static uint16_t host_to_le16(uint16_t value) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return value;
#else
    return __builtin_bswap16(value);
#endif
}

static uint32_t host_to_le32(uint32_t value) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return value;
#else
    return __builtin_bswap32(value);
#endif
}

static uint64_t host_to_le64(uint64_t value) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return value;
#else
    return __builtin_bswap64(value);
#endif
}

static uint16_t le16_to_host(uint16_t value) { return host_to_le16(value); }
static uint32_t le32_to_host(uint32_t value) { return host_to_le32(value); }
static uint64_t le64_to_host(uint64_t value) { return host_to_le64(value); }

static void write_u16_le(uint8_t *dst, uint16_t value) {
    uint16_t v = host_to_le16(value);
    memcpy(dst, &v, sizeof(v));
}

static void write_u32_le(uint8_t *dst, uint32_t value) {
    uint32_t v = host_to_le32(value);
    memcpy(dst, &v, sizeof(v));
}

static void write_u64_le(uint8_t *dst, uint64_t value) {
    uint64_t v = host_to_le64(value);
    memcpy(dst, &v, sizeof(v));
}

static uint16_t read_u16_le(const uint8_t *src) {
    uint16_t v;
    memcpy(&v, src, sizeof(v));
    return le16_to_host(v);
}

static uint32_t read_u32_le(const uint8_t *src) {
    uint32_t v;
    memcpy(&v, src, sizeof(v));
    return le32_to_host(v);
}

static uint64_t read_u64_le(const uint8_t *src) {
    uint64_t v;
    memcpy(&v, src, sizeof(v));
    return le64_to_host(v);
}

static int add_overflows_size(size_t lhs, size_t rhs, size_t *out) {
    if (lhs > SIZE_MAX - rhs) {
        errno = EOVERFLOW;
        return -1;
    }

    *out = lhs + rhs;
    return 0;
}

static int validate_message_header(const struct netipc_msg_header *header) {
    if (!header) {
        errno = EINVAL;
        return -1;
    }

    if (header->magic != NETIPC_MSG_MAGIC || header->version != NETIPC_MSG_VERSION) {
        errno = EPROTO;
        return -1;
    }

    if (header->header_len != NETIPC_MSG_HEADER_LEN) {
        errno = EPROTO;
        return -1;
    }

    if (header->kind != NETIPC_MSG_KIND_REQUEST &&
        header->kind != NETIPC_MSG_KIND_RESPONSE &&
        header->kind != NETIPC_MSG_KIND_CONTROL) {
        errno = EPROTO;
        return -1;
    }

    return 0;
}

size_t netipc_msg_aligned_item_size(uint32_t payload_len) {
    size_t aligned = (size_t)payload_len;
    size_t rem = aligned % NETIPC_MSG_ITEM_ALIGNMENT;

    if (rem == 0u) {
        return aligned;
    }

    aligned += NETIPC_MSG_ITEM_ALIGNMENT - rem;
    return aligned;
}

size_t netipc_msg_max_batch_payload_len(uint32_t max_item_payload_len, uint32_t max_items) {
    if (max_item_payload_len == 0u || max_items == 0u) {
        errno = EINVAL;
        return 0u;
    }

    size_t table_len = (size_t)max_items * NETIPC_MSG_ITEM_REF_LEN;
    size_t aligned_item_len = netipc_msg_aligned_item_size(max_item_payload_len);
    size_t payload_area_len = aligned_item_len * (size_t)max_items;

    if (table_len / NETIPC_MSG_ITEM_REF_LEN != (size_t)max_items ||
        payload_area_len / aligned_item_len != (size_t)max_items ||
        table_len > SIZE_MAX - payload_area_len) {
        errno = EOVERFLOW;
        return 0u;
    }

    return table_len + payload_area_len;
}

size_t netipc_msg_max_batch_total_size(uint32_t max_item_payload_len, uint32_t max_items) {
    size_t payload_len = netipc_msg_max_batch_payload_len(max_item_payload_len, max_items);
    if (payload_len == 0u) {
        return 0u;
    }

    if ((size_t)NETIPC_MSG_HEADER_LEN > SIZE_MAX - payload_len) {
        errno = EOVERFLOW;
        return 0u;
    }

    return (size_t)NETIPC_MSG_HEADER_LEN + payload_len;
}

size_t netipc_msg_total_size(const struct netipc_msg_header *header) {
    if (!header) {
        return 0u;
    }

    return (size_t)header->header_len + (size_t)header->payload_len;
}

int netipc_encode_msg_header(uint8_t *dst,
                             size_t dst_len,
                             const struct netipc_msg_header *header) {
    if (!dst || !header) {
        errno = EINVAL;
        return -1;
    }

    if (dst_len < NETIPC_MSG_HEADER_LEN) {
        errno = EMSGSIZE;
        return -1;
    }

    if (validate_message_header(header) != 0) {
        return -1;
    }

    write_u32_le(dst + NETIPC_OFFSET_MAGIC, header->magic);
    write_u16_le(dst + NETIPC_OFFSET_VERSION, header->version);
    write_u16_le(dst + NETIPC_OFFSET_HEADER_LEN, header->header_len);
    write_u16_le(dst + NETIPC_OFFSET_KIND, header->kind);
    write_u16_le(dst + NETIPC_OFFSET_FLAGS, header->flags);
    write_u16_le(dst + NETIPC_OFFSET_CODE, header->code);
    write_u16_le(dst + NETIPC_OFFSET_TRANSPORT_STATUS, header->transport_status);
    write_u32_le(dst + NETIPC_OFFSET_PAYLOAD_LEN, header->payload_len);
    write_u32_le(dst + NETIPC_OFFSET_ITEM_COUNT, header->item_count);
    write_u64_le(dst + NETIPC_OFFSET_MESSAGE_ID, header->message_id);
    return 0;
}

int netipc_decode_msg_header(const uint8_t *src,
                             size_t src_len,
                             struct netipc_msg_header *header) {
    if (!src || !header) {
        errno = EINVAL;
        return -1;
    }

    if (src_len < NETIPC_MSG_HEADER_LEN) {
        errno = EMSGSIZE;
        return -1;
    }

    header->magic = read_u32_le(src + NETIPC_OFFSET_MAGIC);
    header->version = read_u16_le(src + NETIPC_OFFSET_VERSION);
    header->header_len = read_u16_le(src + NETIPC_OFFSET_HEADER_LEN);
    header->kind = read_u16_le(src + NETIPC_OFFSET_KIND);
    header->flags = read_u16_le(src + NETIPC_OFFSET_FLAGS);
    header->code = read_u16_le(src + NETIPC_OFFSET_CODE);
    header->transport_status = read_u16_le(src + NETIPC_OFFSET_TRANSPORT_STATUS);
    header->payload_len = read_u32_le(src + NETIPC_OFFSET_PAYLOAD_LEN);
    header->item_count = read_u32_le(src + NETIPC_OFFSET_ITEM_COUNT);
    header->message_id = read_u64_le(src + NETIPC_OFFSET_MESSAGE_ID);

    return validate_message_header(header);
}

int netipc_encode_item_refs(uint8_t *dst,
                            size_t dst_len,
                            const struct netipc_item_ref *refs,
                            size_t count) {
    size_t required = count * NETIPC_MSG_ITEM_REF_LEN;
    size_t i;

    if (!dst || (!refs && count != 0u)) {
        errno = EINVAL;
        return -1;
    }

    if (dst_len < required) {
        errno = EMSGSIZE;
        return -1;
    }

    for (i = 0; i < count; ++i) {
        write_u32_le(dst + (i * NETIPC_MSG_ITEM_REF_LEN) + NETIPC_ITEM_REF_OFFSET, refs[i].offset);
        write_u32_le(dst + (i * NETIPC_MSG_ITEM_REF_LEN) + NETIPC_ITEM_REF_LENGTH, refs[i].length);
    }

    return 0;
}

int netipc_decode_item_refs(const uint8_t *src,
                            size_t src_len,
                            struct netipc_item_ref *refs,
                            size_t count) {
    size_t required = count * NETIPC_MSG_ITEM_REF_LEN;
    size_t i;

    if (!src || (!refs && count != 0u)) {
        errno = EINVAL;
        return -1;
    }

    if (src_len < required) {
        errno = EMSGSIZE;
        return -1;
    }

    for (i = 0; i < count; ++i) {
        refs[i].offset = read_u32_le(src + (i * NETIPC_MSG_ITEM_REF_LEN) + NETIPC_ITEM_REF_OFFSET);
        refs[i].length = read_u32_le(src + (i * NETIPC_MSG_ITEM_REF_LEN) + NETIPC_ITEM_REF_LENGTH);
    }

    return 0;
}

int netipc_encode_hello_payload(uint8_t *dst,
                                size_t dst_len,
                                const struct netipc_hello *hello) {
    if (!dst || !hello) {
        errno = EINVAL;
        return -1;
    }

    if (dst_len < NETIPC_CONTROL_HELLO_PAYLOAD_LEN) {
        errno = EMSGSIZE;
        return -1;
    }

    write_u16_le(dst + NETIPC_HELLO_OFFSET_LAYOUT_VERSION, hello->layout_version);
    write_u16_le(dst + NETIPC_HELLO_OFFSET_FLAGS, hello->flags);
    write_u32_le(dst + NETIPC_HELLO_OFFSET_SUPPORTED_PROFILES, hello->supported_profiles);
    write_u32_le(dst + NETIPC_HELLO_OFFSET_PREFERRED_PROFILES, hello->preferred_profiles);
    write_u32_le(dst + NETIPC_HELLO_OFFSET_MAX_REQUEST_PAYLOAD_BYTES, hello->max_request_payload_bytes);
    write_u32_le(dst + NETIPC_HELLO_OFFSET_MAX_REQUEST_BATCH_ITEMS, hello->max_request_batch_items);
    write_u32_le(dst + NETIPC_HELLO_OFFSET_MAX_RESPONSE_PAYLOAD_BYTES, hello->max_response_payload_bytes);
    write_u32_le(dst + NETIPC_HELLO_OFFSET_MAX_RESPONSE_BATCH_ITEMS, hello->max_response_batch_items);
    write_u64_le(dst + NETIPC_HELLO_OFFSET_AUTH_TOKEN, hello->auth_token);
    return 0;
}

int netipc_decode_hello_payload(const uint8_t *src,
                                size_t src_len,
                                struct netipc_hello *hello) {
    if (!src || !hello) {
        errno = EINVAL;
        return -1;
    }

    if (src_len < NETIPC_CONTROL_HELLO_PAYLOAD_LEN) {
        errno = EMSGSIZE;
        return -1;
    }

    hello->layout_version = read_u16_le(src + NETIPC_HELLO_OFFSET_LAYOUT_VERSION);
    hello->flags = read_u16_le(src + NETIPC_HELLO_OFFSET_FLAGS);
    hello->supported_profiles = read_u32_le(src + NETIPC_HELLO_OFFSET_SUPPORTED_PROFILES);
    hello->preferred_profiles = read_u32_le(src + NETIPC_HELLO_OFFSET_PREFERRED_PROFILES);
    hello->max_request_payload_bytes = read_u32_le(src + NETIPC_HELLO_OFFSET_MAX_REQUEST_PAYLOAD_BYTES);
    hello->max_request_batch_items = read_u32_le(src + NETIPC_HELLO_OFFSET_MAX_REQUEST_BATCH_ITEMS);
    hello->max_response_payload_bytes = read_u32_le(src + NETIPC_HELLO_OFFSET_MAX_RESPONSE_PAYLOAD_BYTES);
    hello->max_response_batch_items = read_u32_le(src + NETIPC_HELLO_OFFSET_MAX_RESPONSE_BATCH_ITEMS);
    hello->auth_token = read_u64_le(src + NETIPC_HELLO_OFFSET_AUTH_TOKEN);
    return 0;
}

int netipc_encode_hello_ack_payload(uint8_t *dst,
                                    size_t dst_len,
                                    const struct netipc_hello_ack *hello_ack) {
    if (!dst || !hello_ack) {
        errno = EINVAL;
        return -1;
    }

    if (dst_len < NETIPC_CONTROL_HELLO_ACK_PAYLOAD_LEN) {
        errno = EMSGSIZE;
        return -1;
    }

    write_u16_le(dst + NETIPC_HELLO_ACK_OFFSET_LAYOUT_VERSION, hello_ack->layout_version);
    write_u16_le(dst + NETIPC_HELLO_ACK_OFFSET_FLAGS, hello_ack->flags);
    write_u32_le(dst + NETIPC_HELLO_ACK_OFFSET_SERVER_SUPPORTED_PROFILES, hello_ack->server_supported_profiles);
    write_u32_le(dst + NETIPC_HELLO_ACK_OFFSET_INTERSECTION_PROFILES, hello_ack->intersection_profiles);
    write_u32_le(dst + NETIPC_HELLO_ACK_OFFSET_SELECTED_PROFILE, hello_ack->selected_profile);
    write_u32_le(dst + NETIPC_HELLO_ACK_OFFSET_AGREED_MAX_REQUEST_PAYLOAD_BYTES, hello_ack->agreed_max_request_payload_bytes);
    write_u32_le(dst + NETIPC_HELLO_ACK_OFFSET_AGREED_MAX_REQUEST_BATCH_ITEMS, hello_ack->agreed_max_request_batch_items);
    write_u32_le(dst + NETIPC_HELLO_ACK_OFFSET_AGREED_MAX_RESPONSE_PAYLOAD_BYTES, hello_ack->agreed_max_response_payload_bytes);
    write_u32_le(dst + NETIPC_HELLO_ACK_OFFSET_AGREED_MAX_RESPONSE_BATCH_ITEMS, hello_ack->agreed_max_response_batch_items);
    return 0;
}

int netipc_decode_hello_ack_payload(const uint8_t *src,
                                    size_t src_len,
                                    struct netipc_hello_ack *hello_ack) {
    if (!src || !hello_ack) {
        errno = EINVAL;
        return -1;
    }

    if (src_len < NETIPC_CONTROL_HELLO_ACK_PAYLOAD_LEN) {
        errno = EMSGSIZE;
        return -1;
    }

    hello_ack->layout_version = read_u16_le(src + NETIPC_HELLO_ACK_OFFSET_LAYOUT_VERSION);
    hello_ack->flags = read_u16_le(src + NETIPC_HELLO_ACK_OFFSET_FLAGS);
    hello_ack->server_supported_profiles = read_u32_le(src + NETIPC_HELLO_ACK_OFFSET_SERVER_SUPPORTED_PROFILES);
    hello_ack->intersection_profiles = read_u32_le(src + NETIPC_HELLO_ACK_OFFSET_INTERSECTION_PROFILES);
    hello_ack->selected_profile = read_u32_le(src + NETIPC_HELLO_ACK_OFFSET_SELECTED_PROFILE);
    hello_ack->agreed_max_request_payload_bytes = read_u32_le(src + NETIPC_HELLO_ACK_OFFSET_AGREED_MAX_REQUEST_PAYLOAD_BYTES);
    hello_ack->agreed_max_request_batch_items = read_u32_le(src + NETIPC_HELLO_ACK_OFFSET_AGREED_MAX_REQUEST_BATCH_ITEMS);
    hello_ack->agreed_max_response_payload_bytes = read_u32_le(src + NETIPC_HELLO_ACK_OFFSET_AGREED_MAX_RESPONSE_PAYLOAD_BYTES);
    hello_ack->agreed_max_response_batch_items = read_u32_le(src + NETIPC_HELLO_ACK_OFFSET_AGREED_MAX_RESPONSE_BATCH_ITEMS);
    return 0;
}

size_t netipc_cgroups_snapshot_item_payload_len(const struct netipc_cgroups_snapshot_item *item) {
    size_t total = NETIPC_CGROUPS_SNAPSHOT_ITEM_HEADER_LEN;

    if (!item) {
        errno = EINVAL;
        return 0u;
    }

    if ((item->name_len != 0u && !item->name) || (item->path_len != 0u && !item->path)) {
        errno = EINVAL;
        return 0u;
    }

    if (add_overflows_size(total, (size_t)item->name_len + 1u, &total) != 0 ||
        add_overflows_size(total, (size_t)item->path_len + 1u, &total) != 0) {
        return 0u;
    }

    return total;
}

int netipc_encode_cgroups_snapshot_request_payload(
    uint8_t *dst,
    size_t dst_len,
    const struct netipc_cgroups_snapshot_request *request) {
    if (!dst || !request) {
        errno = EINVAL;
        return -1;
    }

    if (dst_len < NETIPC_CGROUPS_SNAPSHOT_REQUEST_PAYLOAD_LEN) {
        errno = EMSGSIZE;
        return -1;
    }

    write_u16_le(dst + NETIPC_CGROUPS_SNAPSHOT_REQ_OFFSET_LAYOUT_VERSION,
                 NETIPC_CGROUPS_SNAPSHOT_LAYOUT_VERSION);
    write_u16_le(dst + NETIPC_CGROUPS_SNAPSHOT_REQ_OFFSET_FLAGS, request->flags);
    return 0;
}

int netipc_decode_cgroups_snapshot_request_view(
    const uint8_t *src,
    size_t src_len,
    struct netipc_cgroups_snapshot_request_view *view) {
    if (!src || !view) {
        errno = EINVAL;
        return -1;
    }

    if (src_len < NETIPC_CGROUPS_SNAPSHOT_REQUEST_PAYLOAD_LEN) {
        errno = EMSGSIZE;
        return -1;
    }

    view->layout_version = read_u16_le(src + NETIPC_CGROUPS_SNAPSHOT_REQ_OFFSET_LAYOUT_VERSION);
    view->flags = read_u16_le(src + NETIPC_CGROUPS_SNAPSHOT_REQ_OFFSET_FLAGS);

    if (view->layout_version != NETIPC_CGROUPS_SNAPSHOT_LAYOUT_VERSION) {
        errno = EPROTO;
        return -1;
    }

    return 0;
}

int netipc_cgroups_snapshot_builder_init(struct netipc_cgroups_snapshot_builder *builder,
                                         uint8_t *payload,
                                         size_t payload_capacity,
                                         uint64_t generation,
                                         uint32_t systemd_enabled,
                                         uint16_t flags,
                                         uint32_t expected_items) {
    size_t table_len;
    size_t packed_offset;

    if (!builder || !payload) {
        errno = EINVAL;
        return -1;
    }

    table_len = (size_t)expected_items * NETIPC_MSG_ITEM_REF_LEN;
    if (expected_items != 0u && table_len / NETIPC_MSG_ITEM_REF_LEN != (size_t)expected_items) {
        errno = EOVERFLOW;
        return -1;
    }

    if (add_overflows_size(NETIPC_CGROUPS_SNAPSHOT_RESPONSE_HEADER_LEN, table_len, &packed_offset) != 0) {
        return -1;
    }

    if (payload_capacity < packed_offset) {
        errno = EMSGSIZE;
        return -1;
    }

    memset(builder, 0, sizeof(*builder));
    memset(payload, 0, packed_offset);
    builder->payload = payload;
    builder->payload_capacity = payload_capacity;
    builder->payload_len = packed_offset;
    builder->packed_offset = packed_offset;
    builder->expected_items = expected_items;
    builder->flags = flags;
    builder->systemd_enabled = systemd_enabled;
    builder->generation = generation;
    return 0;
}

int netipc_cgroups_snapshot_builder_add_item(struct netipc_cgroups_snapshot_builder *builder,
                                             const struct netipc_cgroups_snapshot_item *item) {
    size_t item_len;
    size_t start;
    size_t gap_len;
    uint8_t *entry;
    uint8_t *item_dst;
    uint32_t name_off;
    uint32_t path_off;
    uint32_t ref_offset;

    if (!builder || !item) {
        errno = EINVAL;
        return -1;
    }

    if (builder->item_count >= builder->expected_items) {
        errno = E2BIG;
        return -1;
    }

    item_len = netipc_cgroups_snapshot_item_payload_len(item);
    if (item_len == 0u) {
        return -1;
    }

    start = netipc_msg_aligned_item_size((uint32_t)builder->payload_len);
    if (start < builder->payload_len || start > builder->payload_capacity) {
        errno = EOVERFLOW;
        return -1;
    }

    if (add_overflows_size(start, item_len, &gap_len) != 0) {
        return -1;
    }
    if (gap_len > builder->payload_capacity) {
        errno = EMSGSIZE;
        return -1;
    }

    if (start > builder->payload_len) {
        memset(builder->payload + builder->payload_len, 0, start - builder->payload_len);
    }

    item_dst = builder->payload + start;
    name_off = NETIPC_CGROUPS_SNAPSHOT_ITEM_HEADER_LEN;
    path_off = name_off + item->name_len + 1u;

    memset(item_dst, 0, item_len);
    write_u16_le(item_dst + NETIPC_CGROUPS_SNAPSHOT_ITEM_OFFSET_LAYOUT_VERSION,
                 NETIPC_CGROUPS_SNAPSHOT_LAYOUT_VERSION);
    write_u16_le(item_dst + NETIPC_CGROUPS_SNAPSHOT_ITEM_OFFSET_FLAGS, 0u);
    write_u32_le(item_dst + NETIPC_CGROUPS_SNAPSHOT_ITEM_OFFSET_HASH, item->hash);
    write_u32_le(item_dst + NETIPC_CGROUPS_SNAPSHOT_ITEM_OFFSET_OPTIONS, item->options);
    write_u32_le(item_dst + NETIPC_CGROUPS_SNAPSHOT_ITEM_OFFSET_ENABLED, item->enabled);
    write_u32_le(item_dst + NETIPC_CGROUPS_SNAPSHOT_ITEM_OFFSET_NAME_OFF, name_off);
    write_u32_le(item_dst + NETIPC_CGROUPS_SNAPSHOT_ITEM_OFFSET_NAME_LEN, item->name_len);
    write_u32_le(item_dst + NETIPC_CGROUPS_SNAPSHOT_ITEM_OFFSET_PATH_OFF, path_off);
    write_u32_le(item_dst + NETIPC_CGROUPS_SNAPSHOT_ITEM_OFFSET_PATH_LEN, item->path_len);

    if (item->name_len != 0u) {
        memcpy(item_dst + name_off, item->name, item->name_len);
    }
    item_dst[name_off + item->name_len] = '\0';

    if (item->path_len != 0u) {
        memcpy(item_dst + path_off, item->path, item->path_len);
    }
    item_dst[path_off + item->path_len] = '\0';

    entry = builder->payload + NETIPC_CGROUPS_SNAPSHOT_RESPONSE_HEADER_LEN +
            ((size_t)builder->item_count * NETIPC_MSG_ITEM_REF_LEN);
    ref_offset = (uint32_t)(start - builder->packed_offset);
    write_u32_le(entry + NETIPC_ITEM_REF_OFFSET, ref_offset);
    write_u32_le(entry + NETIPC_ITEM_REF_LENGTH, (uint32_t)item_len);

    builder->item_count += 1u;
    builder->payload_len = gap_len;
    return 0;
}

int netipc_cgroups_snapshot_builder_finish(struct netipc_cgroups_snapshot_builder *builder,
                                           size_t *out_payload_len) {
    if (!builder || !out_payload_len) {
        errno = EINVAL;
        return -1;
    }

    if (builder->item_count != builder->expected_items) {
        errno = EPROTO;
        return -1;
    }

    write_u16_le(builder->payload + NETIPC_CGROUPS_SNAPSHOT_RESP_OFFSET_LAYOUT_VERSION,
                 NETIPC_CGROUPS_SNAPSHOT_LAYOUT_VERSION);
    write_u16_le(builder->payload + NETIPC_CGROUPS_SNAPSHOT_RESP_OFFSET_FLAGS,
                 builder->flags);
    write_u32_le(builder->payload + NETIPC_CGROUPS_SNAPSHOT_RESP_OFFSET_SYSTEMD_ENABLED,
                 builder->systemd_enabled);
    write_u64_le(builder->payload + NETIPC_CGROUPS_SNAPSHOT_RESP_OFFSET_GENERATION,
                 builder->generation);
    *out_payload_len = builder->payload_len;
    return 0;
}

static int validate_cgroups_string_view(const uint8_t *payload,
                                        size_t payload_len,
                                        uint32_t off,
                                        uint32_t len,
                                        struct netipc_str_view *view) {
    size_t end;

    if (!payload || !view) {
        errno = EINVAL;
        return -1;
    }

    if (off < NETIPC_CGROUPS_SNAPSHOT_ITEM_HEADER_LEN) {
        errno = EPROTO;
        return -1;
    }

    if (add_overflows_size((size_t)off, (size_t)len, &end) != 0 ||
        add_overflows_size(end, 1u, &end) != 0 ||
        end > payload_len) {
        errno = EPROTO;
        return -1;
    }

    if (payload[off + len] != '\0') {
        errno = EPROTO;
        return -1;
    }

    view->ptr = (const char *)(payload + off);
    view->len = len;
    return 0;
}

static int decode_cgroups_item_ref(const struct netipc_cgroups_snapshot_view *view,
                                   uint32_t index,
                                   struct netipc_item_ref *item_ref) {
    const uint8_t *entry;

    if (!view || !item_ref || index >= view->item_count) {
        errno = EINVAL;
        return -1;
    }

    entry = view->item_refs + ((size_t)index * NETIPC_MSG_ITEM_REF_LEN);
    item_ref->offset = read_u32_le(entry + NETIPC_ITEM_REF_OFFSET);
    item_ref->length = read_u32_le(entry + NETIPC_ITEM_REF_LENGTH);

    if ((item_ref->offset % NETIPC_MSG_ITEM_ALIGNMENT) != 0u) {
        errno = EPROTO;
        return -1;
    }

    if ((size_t)item_ref->offset > view->items_len ||
        (size_t)item_ref->length > view->items_len - (size_t)item_ref->offset ||
        item_ref->length < NETIPC_CGROUPS_SNAPSHOT_ITEM_HEADER_LEN) {
        errno = EPROTO;
        return -1;
    }

    return 0;
}

int netipc_decode_cgroups_snapshot_view(const uint8_t *src,
                                        size_t src_len,
                                        uint32_t item_count,
                                        struct netipc_cgroups_snapshot_view *view) {
    size_t refs_len;
    size_t packed_offset;
    uint32_t i;
    struct netipc_item_ref item_ref;

    if (!src || !view) {
        errno = EINVAL;
        return -1;
    }

    refs_len = (size_t)item_count * NETIPC_MSG_ITEM_REF_LEN;
    if (item_count != 0u && refs_len / NETIPC_MSG_ITEM_REF_LEN != (size_t)item_count) {
        errno = EOVERFLOW;
        return -1;
    }

    if (add_overflows_size(NETIPC_CGROUPS_SNAPSHOT_RESPONSE_HEADER_LEN, refs_len, &packed_offset) != 0) {
        return -1;
    }

    if (src_len < packed_offset) {
        errno = EMSGSIZE;
        return -1;
    }

    view->layout_version = read_u16_le(src + NETIPC_CGROUPS_SNAPSHOT_RESP_OFFSET_LAYOUT_VERSION);
    view->flags = read_u16_le(src + NETIPC_CGROUPS_SNAPSHOT_RESP_OFFSET_FLAGS);
    view->systemd_enabled = read_u32_le(src + NETIPC_CGROUPS_SNAPSHOT_RESP_OFFSET_SYSTEMD_ENABLED);
    view->generation = read_u64_le(src + NETIPC_CGROUPS_SNAPSHOT_RESP_OFFSET_GENERATION);
    view->item_count = item_count;
    view->item_refs = src + NETIPC_CGROUPS_SNAPSHOT_RESPONSE_HEADER_LEN;
    view->item_refs_len = refs_len;
    view->items = src + packed_offset;
    view->items_len = src_len - packed_offset;

    if (view->layout_version != NETIPC_CGROUPS_SNAPSHOT_LAYOUT_VERSION) {
        errno = EPROTO;
        return -1;
    }

    for (i = 0u; i < item_count; ++i) {
        if (decode_cgroups_item_ref(view, i, &item_ref) != 0) {
            return -1;
        }
    }

    return 0;
}

int netipc_cgroups_snapshot_view_item_at(const struct netipc_cgroups_snapshot_view *view,
                                         uint32_t index,
                                         struct netipc_cgroups_snapshot_item_view *item_view) {
    struct netipc_item_ref item_ref;
    const uint8_t *payload;
    uint32_t name_off;
    uint32_t name_len;
    uint32_t path_off;
    uint32_t path_len;

    if (!view || !item_view) {
        errno = EINVAL;
        return -1;
    }

    if (decode_cgroups_item_ref(view, index, &item_ref) != 0) {
        return -1;
    }

    payload = view->items + item_ref.offset;
    item_view->layout_version =
        read_u16_le(payload + NETIPC_CGROUPS_SNAPSHOT_ITEM_OFFSET_LAYOUT_VERSION);
    item_view->flags = read_u16_le(payload + NETIPC_CGROUPS_SNAPSHOT_ITEM_OFFSET_FLAGS);
    item_view->hash = read_u32_le(payload + NETIPC_CGROUPS_SNAPSHOT_ITEM_OFFSET_HASH);
    item_view->options = read_u32_le(payload + NETIPC_CGROUPS_SNAPSHOT_ITEM_OFFSET_OPTIONS);
    item_view->enabled = read_u32_le(payload + NETIPC_CGROUPS_SNAPSHOT_ITEM_OFFSET_ENABLED);

    if (item_view->layout_version != NETIPC_CGROUPS_SNAPSHOT_LAYOUT_VERSION) {
        errno = EPROTO;
        return -1;
    }

    name_off = read_u32_le(payload + NETIPC_CGROUPS_SNAPSHOT_ITEM_OFFSET_NAME_OFF);
    name_len = read_u32_le(payload + NETIPC_CGROUPS_SNAPSHOT_ITEM_OFFSET_NAME_LEN);
    path_off = read_u32_le(payload + NETIPC_CGROUPS_SNAPSHOT_ITEM_OFFSET_PATH_OFF);
    path_len = read_u32_le(payload + NETIPC_CGROUPS_SNAPSHOT_ITEM_OFFSET_PATH_LEN);

    if (validate_cgroups_string_view(payload, item_ref.length, name_off, name_len,
                                     &item_view->name_view) != 0 ||
        validate_cgroups_string_view(payload, item_ref.length, path_off, path_len,
                                     &item_view->path_view) != 0) {
        return -1;
    }

    return 0;
}

static int validate_header(const uint8_t frame[NETIPC_FRAME_SIZE], uint16_t expected_kind) {
    struct netipc_msg_header header;

    if (netipc_decode_msg_header(frame, NETIPC_FRAME_SIZE, &header) != 0) {
        return -1;
    }

    if (header.kind != expected_kind ||
        header.code != NETIPC_METHOD_INCREMENT ||
        header.payload_len != NETIPC_INCREMENT_PAYLOAD_LEN ||
        header.item_count != 1u ||
        header.transport_status != NETIPC_TRANSPORT_STATUS_OK ||
        header.flags != 0u) {
        errno = EPROTO;
        return -1;
    }

    return 0;
}

static void encode_base(uint8_t frame[NETIPC_FRAME_SIZE], uint16_t kind, uint64_t request_id) {
    struct netipc_msg_header header;

    memset(frame, 0, NETIPC_FRAME_SIZE);

    header.magic = NETIPC_MSG_MAGIC;
    header.version = NETIPC_MSG_VERSION;
    header.header_len = NETIPC_MSG_HEADER_LEN;
    header.kind = kind;
    header.flags = 0u;
    header.code = NETIPC_METHOD_INCREMENT;
    header.transport_status = NETIPC_TRANSPORT_STATUS_OK;
    header.payload_len = NETIPC_INCREMENT_PAYLOAD_LEN;
    header.item_count = 1u;
    header.message_id = request_id;

    (void)netipc_encode_msg_header(frame, NETIPC_FRAME_SIZE, &header);
}

int netipc_encode_increment_request(uint8_t frame[NETIPC_FRAME_SIZE],
                                    uint64_t request_id,
                                    const struct netipc_increment_request *req) {
    if (!frame || !req) {
        errno = EINVAL;
        return -1;
    }

    encode_base(frame, NETIPC_FRAME_KIND_REQUEST, request_id);
    write_u64_le(frame + NETIPC_OFFSET_VALUE, req->value);
    write_u32_le(frame + NETIPC_OFFSET_STATUS, 0);
    return 0;
}

int netipc_decode_increment_request(const uint8_t frame[NETIPC_FRAME_SIZE],
                                    uint64_t *request_id,
                                    struct netipc_increment_request *req) {
    if (!frame || !request_id || !req) {
        errno = EINVAL;
        return -1;
    }

    if (validate_header(frame, NETIPC_FRAME_KIND_REQUEST) != 0) {
        return -1;
    }

    *request_id = read_u64_le(frame + NETIPC_OFFSET_MESSAGE_ID);
    req->value = read_u64_le(frame + NETIPC_OFFSET_VALUE);
    return 0;
}

int netipc_encode_increment_response(uint8_t frame[NETIPC_FRAME_SIZE],
                                     uint64_t request_id,
                                     const struct netipc_increment_response *resp) {
    if (!frame || !resp) {
        errno = EINVAL;
        return -1;
    }

    encode_base(frame, NETIPC_FRAME_KIND_RESPONSE, request_id);
    write_u64_le(frame + NETIPC_OFFSET_VALUE, resp->value);
    write_u32_le(frame + NETIPC_OFFSET_STATUS, (uint32_t)resp->status);
    return 0;
}

int netipc_decode_increment_response(const uint8_t frame[NETIPC_FRAME_SIZE],
                                     uint64_t *request_id,
                                     struct netipc_increment_response *resp) {
    if (!frame || !request_id || !resp) {
        errno = EINVAL;
        return -1;
    }

    if (validate_header(frame, NETIPC_FRAME_KIND_RESPONSE) != 0) {
        return -1;
    }

    *request_id = read_u64_le(frame + NETIPC_OFFSET_MESSAGE_ID);
    resp->value = read_u64_le(frame + NETIPC_OFFSET_VALUE);
    resp->status = (int32_t)read_u32_le(frame + NETIPC_OFFSET_STATUS);
    return 0;
}

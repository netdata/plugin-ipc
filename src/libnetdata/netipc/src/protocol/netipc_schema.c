#include <netipc/netipc_schema.h>

#include <errno.h>
#include <stddef.h>
#include <string.h>

#define NETIPC_OFFSET_MAGIC 0u
#define NETIPC_OFFSET_VERSION 4u
#define NETIPC_OFFSET_KIND 6u
#define NETIPC_OFFSET_METHOD 8u
#define NETIPC_OFFSET_RESERVED0 10u
#define NETIPC_OFFSET_PAYLOAD_LEN 12u
#define NETIPC_OFFSET_REQUEST_ID 16u
#define NETIPC_OFFSET_VALUE 24u
#define NETIPC_OFFSET_STATUS 32u
#define NETIPC_OFFSET_RESERVED1 36u

#define NETIPC_INCREMENT_PAYLOAD_LEN 12u

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

static int validate_header(const uint8_t frame[NETIPC_FRAME_SIZE], uint16_t expected_kind) {
    uint32_t magic = read_u32_le(frame + NETIPC_OFFSET_MAGIC);
    uint16_t version = read_u16_le(frame + NETIPC_OFFSET_VERSION);
    uint16_t kind = read_u16_le(frame + NETIPC_OFFSET_KIND);
    uint16_t method = read_u16_le(frame + NETIPC_OFFSET_METHOD);
    uint32_t payload_len = read_u32_le(frame + NETIPC_OFFSET_PAYLOAD_LEN);

    if (magic != NETIPC_FRAME_MAGIC || version != NETIPC_FRAME_VERSION) {
        errno = EPROTO;
        return -1;
    }

    if (kind != expected_kind || method != NETIPC_METHOD_INCREMENT || payload_len != NETIPC_INCREMENT_PAYLOAD_LEN) {
        errno = EPROTO;
        return -1;
    }

    return 0;
}

static void encode_base(uint8_t frame[NETIPC_FRAME_SIZE], uint16_t kind, uint64_t request_id) {
    memset(frame, 0, NETIPC_FRAME_SIZE);

    write_u32_le(frame + NETIPC_OFFSET_MAGIC, NETIPC_FRAME_MAGIC);
    write_u16_le(frame + NETIPC_OFFSET_VERSION, NETIPC_FRAME_VERSION);
    write_u16_le(frame + NETIPC_OFFSET_KIND, kind);
    write_u16_le(frame + NETIPC_OFFSET_METHOD, NETIPC_METHOD_INCREMENT);
    write_u16_le(frame + NETIPC_OFFSET_RESERVED0, 0);
    write_u32_le(frame + NETIPC_OFFSET_PAYLOAD_LEN, NETIPC_INCREMENT_PAYLOAD_LEN);
    write_u64_le(frame + NETIPC_OFFSET_REQUEST_ID, request_id);
    write_u32_le(frame + NETIPC_OFFSET_RESERVED1, 0);
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

    *request_id = read_u64_le(frame + NETIPC_OFFSET_REQUEST_ID);
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

    *request_id = read_u64_le(frame + NETIPC_OFFSET_REQUEST_ID);
    resp->value = read_u64_le(frame + NETIPC_OFFSET_VALUE);
    resp->status = (int32_t)read_u32_le(frame + NETIPC_OFFSET_STATUS);
    return 0;
}

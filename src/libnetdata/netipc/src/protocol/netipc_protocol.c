/*
 * netipc_protocol.c - Wire envelope and codec implementation.
 *
 * All encode functions write little-endian bytes via memcpy (safe on
 * any alignment). All decode functions read the same way and validate
 * before returning.
 */

#include "netipc/netipc_protocol.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Compile-time layout assertions                                    */
/* ------------------------------------------------------------------ */

_Static_assert(sizeof(nipc_header_t) == 32,
               "nipc_header_t must be 32 bytes");
_Static_assert(sizeof(nipc_chunk_header_t) == 32,
               "nipc_chunk_header_t must be 32 bytes");
_Static_assert(sizeof(nipc_batch_entry_t) == 8,
               "nipc_batch_entry_t must be 8 bytes");
_Static_assert(sizeof(nipc_cgroups_resp_header_t) == 24,
               "nipc_cgroups_resp_header_t must be 24 bytes");

/* ------------------------------------------------------------------ */
/*  Little-endian helpers                                             */
/* ------------------------------------------------------------------ */

static inline void put_u16(void *dst, uint16_t v) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    memcpy(dst, &v, 2);
#else
    uint8_t *p = (uint8_t *)dst;
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
#endif
}

static inline void put_u32(void *dst, uint32_t v) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    memcpy(dst, &v, 4);
#else
    uint8_t *p = (uint8_t *)dst;
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
#endif
}

static inline void put_u64(void *dst, uint64_t v) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    memcpy(dst, &v, 8);
#else
    uint8_t *p = (uint8_t *)dst;
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (i * 8));
#endif
}

static inline uint16_t get_u16(const void *src) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    uint16_t v;
    memcpy(&v, src, 2);
    return v;
#else
    const uint8_t *p = (const uint8_t *)src;
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
#endif
}

static inline uint32_t get_u32(const void *src) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    uint32_t v;
    memcpy(&v, src, 4);
    return v;
#else
    const uint8_t *p = (const uint8_t *)src;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
#endif
}

static inline uint64_t get_u64(const void *src) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    uint64_t v;
    memcpy(&v, src, 8);
    return v;
#else
    const uint8_t *p = (const uint8_t *)src;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v |= (uint64_t)p[i] << (i * 8);
    return v;
#endif
}

/* ------------------------------------------------------------------ */
/*  Outer message header (32 bytes)                                   */
/* ------------------------------------------------------------------ */

size_t nipc_header_encode(const nipc_header_t *hdr, void *buf, size_t buf_len) {
    if (buf_len < NIPC_HEADER_LEN)
        return 0;

    uint8_t *p = (uint8_t *)buf;
    put_u32(p + 0,  hdr->magic);
    put_u16(p + 4,  hdr->version);
    put_u16(p + 6,  hdr->header_len);
    put_u16(p + 8,  hdr->kind);
    put_u16(p + 10, hdr->flags);
    put_u16(p + 12, hdr->code);
    put_u16(p + 14, hdr->transport_status);
    put_u32(p + 16, hdr->payload_len);
    put_u32(p + 20, hdr->item_count);
    put_u64(p + 24, hdr->message_id);
    return NIPC_HEADER_LEN;
}

nipc_error_t nipc_header_decode(const void *buf, size_t buf_len,
                                nipc_header_t *out) {
    if (buf_len < NIPC_HEADER_LEN)
        return NIPC_ERR_TRUNCATED;

    const uint8_t *p = (const uint8_t *)buf;
    out->magic            = get_u32(p + 0);
    out->version          = get_u16(p + 4);
    out->header_len       = get_u16(p + 6);
    out->kind             = get_u16(p + 8);
    out->flags            = get_u16(p + 10);
    out->code             = get_u16(p + 12);
    out->transport_status = get_u16(p + 14);
    out->payload_len      = get_u32(p + 16);
    out->item_count       = get_u32(p + 20);
    out->message_id       = get_u64(p + 24);

    if (out->magic != NIPC_MAGIC_MSG)
        return NIPC_ERR_BAD_MAGIC;
    if (out->version != NIPC_VERSION)
        return NIPC_ERR_BAD_VERSION;
    if (out->header_len != NIPC_HEADER_LEN)
        return NIPC_ERR_BAD_HEADER_LEN;
    if (out->kind < NIPC_KIND_REQUEST || out->kind > NIPC_KIND_CONTROL)
        return NIPC_ERR_BAD_KIND;

    return NIPC_OK;
}

/* ------------------------------------------------------------------ */
/*  Chunk continuation header (32 bytes)                              */
/* ------------------------------------------------------------------ */

size_t nipc_chunk_header_encode(const nipc_chunk_header_t *chk,
                                void *buf, size_t buf_len) {
    if (buf_len < NIPC_HEADER_LEN)
        return 0;

    uint8_t *p = (uint8_t *)buf;
    put_u32(p + 0,  chk->magic);
    put_u16(p + 4,  chk->version);
    put_u16(p + 6,  chk->flags);
    put_u64(p + 8,  chk->message_id);
    put_u32(p + 16, chk->total_message_len);
    put_u32(p + 20, chk->chunk_index);
    put_u32(p + 24, chk->chunk_count);
    put_u32(p + 28, chk->chunk_payload_len);
    return NIPC_HEADER_LEN;
}

nipc_error_t nipc_chunk_header_decode(const void *buf, size_t buf_len,
                                      nipc_chunk_header_t *out) {
    if (buf_len < NIPC_HEADER_LEN)
        return NIPC_ERR_TRUNCATED;

    const uint8_t *p = (const uint8_t *)buf;
    out->magic             = get_u32(p + 0);
    out->version           = get_u16(p + 4);
    out->flags             = get_u16(p + 6);
    out->message_id        = get_u64(p + 8);
    out->total_message_len = get_u32(p + 16);
    out->chunk_index       = get_u32(p + 20);
    out->chunk_count       = get_u32(p + 24);
    out->chunk_payload_len = get_u32(p + 28);

    if (out->magic != NIPC_MAGIC_CHUNK)
        return NIPC_ERR_BAD_MAGIC;
    if (out->version != NIPC_VERSION)
        return NIPC_ERR_BAD_VERSION;

    return NIPC_OK;
}

/* ------------------------------------------------------------------ */
/*  Batch item directory                                              */
/* ------------------------------------------------------------------ */

size_t nipc_batch_dir_encode(const nipc_batch_entry_t *entries,
                             uint32_t item_count,
                             void *buf, size_t buf_len) {
    size_t need = (size_t)item_count * 8;
    if (buf_len < need)
        return 0;

    uint8_t *p = (uint8_t *)buf;
    for (uint32_t i = 0; i < item_count; i++) {
        put_u32(p + i * 8,     entries[i].offset);
        put_u32(p + i * 8 + 4, entries[i].length);
    }
    return need;
}

nipc_error_t nipc_batch_dir_decode(const void *buf, size_t buf_len,
                                   uint32_t item_count,
                                   uint32_t packed_area_len,
                                   nipc_batch_entry_t *out) {
    size_t dir_size = (size_t)item_count * 8;
    if (buf_len < dir_size)
        return NIPC_ERR_TRUNCATED;

    const uint8_t *p = (const uint8_t *)buf;
    for (uint32_t i = 0; i < item_count; i++) {
        out[i].offset = get_u32(p + i * 8);
        out[i].length = get_u32(p + i * 8 + 4);

        if (out[i].offset % NIPC_ALIGNMENT != 0)
            return NIPC_ERR_BAD_ALIGNMENT;

        if ((uint64_t)out[i].offset + out[i].length > packed_area_len)
            return NIPC_ERR_OUT_OF_BOUNDS;
    }
    return NIPC_OK;
}

nipc_error_t nipc_batch_item_get(const void *payload, size_t payload_len,
                                 uint32_t item_count, uint32_t index,
                                 const void **item_ptr, uint32_t *item_len) {
    if (index >= item_count)
        return NIPC_ERR_OUT_OF_BOUNDS;

    size_t dir_size = (size_t)item_count * 8;
    size_t dir_aligned = nipc_align8(dir_size);

    if (payload_len < dir_aligned)
        return NIPC_ERR_TRUNCATED;

    const uint8_t *dir = (const uint8_t *)payload;
    uint32_t off = get_u32(dir + index * 8);
    uint32_t len = get_u32(dir + index * 8 + 4);

    size_t packed_area_start = dir_aligned;
    size_t packed_area_len   = payload_len - packed_area_start;

    if (off % NIPC_ALIGNMENT != 0)
        return NIPC_ERR_BAD_ALIGNMENT;
    if ((uint64_t)off + len > packed_area_len)
        return NIPC_ERR_OUT_OF_BOUNDS;

    *item_ptr = (const uint8_t *)payload + packed_area_start + off;
    *item_len = len;
    return NIPC_OK;
}

/* ------------------------------------------------------------------ */
/*  Batch builder                                                     */
/* ------------------------------------------------------------------ */

void nipc_batch_builder_init(nipc_batch_builder_t *b,
                             void *buf, size_t buf_len,
                             uint32_t max_items) {
    b->buf        = (uint8_t *)buf;
    b->buf_len    = buf_len;
    b->item_count = 0;
    b->max_items  = max_items;
    b->dir_end    = nipc_align8((size_t)max_items * 8);
    b->data_offset = 0;
}

nipc_error_t nipc_batch_builder_add(nipc_batch_builder_t *b,
                                    const void *item, size_t item_len) {
    if (b->item_count >= b->max_items)
        return NIPC_ERR_OVERFLOW;

    size_t aligned_off = nipc_align8(b->data_offset);
    size_t abs_pos = b->dir_end + aligned_off;

    if (abs_pos + item_len > b->buf_len)
        return NIPC_ERR_OVERFLOW;

    /* Zero alignment padding */
    if (aligned_off > b->data_offset)
        memset(b->buf + b->dir_end + b->data_offset, 0,
               aligned_off - b->data_offset);

    memcpy(b->buf + abs_pos, item, item_len);

    /* Write directory entry */
    uint32_t idx = b->item_count;
    put_u32(b->buf + idx * 8,     (uint32_t)aligned_off);
    put_u32(b->buf + idx * 8 + 4, (uint32_t)item_len);

    b->data_offset = aligned_off + item_len;
    b->item_count++;
    return NIPC_OK;
}

size_t nipc_batch_builder_finish(nipc_batch_builder_t *b,
                                 uint32_t *item_count_out) {
    if (item_count_out)
        *item_count_out = b->item_count;

    /* The decoder expects: [dir: item_count*8] [align pad] [packed items].
     * During building we placed packed data after dir_end = align8(max_items*8).
     * If item_count < max_items, compact by shifting packed data left. */
    size_t final_dir_aligned = nipc_align8((size_t)b->item_count * 8);

    if (final_dir_aligned < b->dir_end && b->data_offset > 0) {
        /* Shift packed item data left from dir_end to final_dir_aligned */
        memmove(b->buf + final_dir_aligned,
                b->buf + b->dir_end,
                b->data_offset);
    }

    /* Directory entry offsets are relative to the packed area start,
     * and they were computed as offsets from 0, so they remain valid
     * regardless of where the packed area is in the buffer. */

    return final_dir_aligned + nipc_align8(b->data_offset);
}

/* ------------------------------------------------------------------ */
/*  Hello payload (44 bytes)                                          */
/* ------------------------------------------------------------------ */

#define NIPC_HELLO_SIZE 44u

size_t nipc_hello_encode(const nipc_hello_t *h, void *buf, size_t buf_len) {
    if (buf_len < NIPC_HELLO_SIZE)
        return 0;

    uint8_t *p = (uint8_t *)buf;
    put_u16(p + 0,  h->layout_version);
    put_u16(p + 2,  h->flags);
    put_u32(p + 4,  h->supported_profiles);
    put_u32(p + 8,  h->preferred_profiles);
    put_u32(p + 12, h->max_request_payload_bytes);
    put_u32(p + 16, h->max_request_batch_items);
    put_u32(p + 20, h->max_response_payload_bytes);
    put_u32(p + 24, h->max_response_batch_items);
    put_u32(p + 28, 0); /* padding */
    put_u64(p + 32, h->auth_token);
    put_u32(p + 40, h->packet_size);
    return NIPC_HELLO_SIZE;
}

nipc_error_t nipc_hello_decode(const void *buf, size_t buf_len,
                               nipc_hello_t *out) {
    if (buf_len < NIPC_HELLO_SIZE)
        return NIPC_ERR_TRUNCATED;

    const uint8_t *p = (const uint8_t *)buf;
    out->layout_version             = get_u16(p + 0);
    out->flags                      = get_u16(p + 2);
    out->supported_profiles         = get_u32(p + 4);
    out->preferred_profiles         = get_u32(p + 8);
    out->max_request_payload_bytes  = get_u32(p + 12);
    out->max_request_batch_items    = get_u32(p + 16);
    out->max_response_payload_bytes = get_u32(p + 20);
    out->max_response_batch_items   = get_u32(p + 24);
    /* p+28: padding, ignored */
    out->auth_token                 = get_u64(p + 32);
    out->packet_size                = get_u32(p + 40);

    if (out->layout_version != 1)
        return NIPC_ERR_BAD_LAYOUT;

    return NIPC_OK;
}

/* ------------------------------------------------------------------ */
/*  Hello-ack payload (36 bytes)                                      */
/* ------------------------------------------------------------------ */

#define NIPC_HELLO_ACK_SIZE 36u

size_t nipc_hello_ack_encode(const nipc_hello_ack_t *h,
                             void *buf, size_t buf_len) {
    if (buf_len < NIPC_HELLO_ACK_SIZE)
        return 0;

    uint8_t *p = (uint8_t *)buf;
    put_u16(p + 0,  h->layout_version);
    put_u16(p + 2,  h->flags);
    put_u32(p + 4,  h->server_supported_profiles);
    put_u32(p + 8,  h->intersection_profiles);
    put_u32(p + 12, h->selected_profile);
    put_u32(p + 16, h->agreed_max_request_payload_bytes);
    put_u32(p + 20, h->agreed_max_request_batch_items);
    put_u32(p + 24, h->agreed_max_response_payload_bytes);
    put_u32(p + 28, h->agreed_max_response_batch_items);
    put_u32(p + 32, h->agreed_packet_size);
    return NIPC_HELLO_ACK_SIZE;
}

nipc_error_t nipc_hello_ack_decode(const void *buf, size_t buf_len,
                                   nipc_hello_ack_t *out) {
    if (buf_len < NIPC_HELLO_ACK_SIZE)
        return NIPC_ERR_TRUNCATED;

    const uint8_t *p = (const uint8_t *)buf;
    out->layout_version                    = get_u16(p + 0);
    out->flags                             = get_u16(p + 2);
    out->server_supported_profiles         = get_u32(p + 4);
    out->intersection_profiles             = get_u32(p + 8);
    out->selected_profile                  = get_u32(p + 12);
    out->agreed_max_request_payload_bytes  = get_u32(p + 16);
    out->agreed_max_request_batch_items    = get_u32(p + 20);
    out->agreed_max_response_payload_bytes = get_u32(p + 24);
    out->agreed_max_response_batch_items   = get_u32(p + 28);
    out->agreed_packet_size                = get_u32(p + 32);

    if (out->layout_version != 1)
        return NIPC_ERR_BAD_LAYOUT;

    return NIPC_OK;
}

/* ------------------------------------------------------------------ */
/*  Cgroups snapshot request (4 bytes)                                */
/* ------------------------------------------------------------------ */

#define NIPC_CGROUPS_REQ_SIZE 4u

size_t nipc_cgroups_req_encode(const nipc_cgroups_req_t *r,
                               void *buf, size_t buf_len) {
    if (buf_len < NIPC_CGROUPS_REQ_SIZE)
        return 0;

    uint8_t *p = (uint8_t *)buf;
    put_u16(p + 0, r->layout_version);
    put_u16(p + 2, r->flags);
    return NIPC_CGROUPS_REQ_SIZE;
}

nipc_error_t nipc_cgroups_req_decode(const void *buf, size_t buf_len,
                                     nipc_cgroups_req_t *out) {
    if (buf_len < NIPC_CGROUPS_REQ_SIZE)
        return NIPC_ERR_TRUNCATED;

    const uint8_t *p = (const uint8_t *)buf;
    out->layout_version = get_u16(p + 0);
    out->flags          = get_u16(p + 2);

    if (out->layout_version != 1)
        return NIPC_ERR_BAD_LAYOUT;

    return NIPC_OK;
}

/* ------------------------------------------------------------------ */
/*  Cgroups snapshot response decode                                  */
/* ------------------------------------------------------------------ */

nipc_error_t nipc_cgroups_resp_decode(const void *buf, size_t buf_len,
                                      nipc_cgroups_resp_view_t *out) {
    if (buf_len < NIPC_CGROUPS_RESP_HDR_SIZE)
        return NIPC_ERR_TRUNCATED;

    const uint8_t *p = (const uint8_t *)buf;
    out->layout_version  = get_u16(p + 0);
    out->flags           = get_u16(p + 2);
    out->item_count      = get_u32(p + 4);
    out->systemd_enabled = get_u32(p + 8);
    /* p+12: reserved */
    out->generation      = get_u64(p + 16);

    if (out->layout_version != 1)
        return NIPC_ERR_BAD_LAYOUT;

    /* Validate directory fits */
    size_t dir_size = (size_t)out->item_count * NIPC_CGROUPS_DIR_ENTRY_SIZE;
    size_t dir_end  = NIPC_CGROUPS_RESP_HDR_SIZE + dir_size;
    if (dir_end > buf_len)
        return NIPC_ERR_TRUNCATED;

    size_t packed_area_len = buf_len - dir_end;

    /* Validate each directory entry */
    const uint8_t *dir = p + NIPC_CGROUPS_RESP_HDR_SIZE;
    for (uint32_t i = 0; i < out->item_count; i++) {
        uint32_t off = get_u32(dir + i * 8);
        uint32_t len = get_u32(dir + i * 8 + 4);

        if (off % NIPC_ALIGNMENT != 0)
            return NIPC_ERR_BAD_ALIGNMENT;
        if ((uint64_t)off + len > packed_area_len)
            return NIPC_ERR_OUT_OF_BOUNDS;
        if (len < NIPC_CGROUPS_ITEM_HDR_SIZE)
            return NIPC_ERR_TRUNCATED;
    }

    out->_payload     = (const uint8_t *)buf;
    out->_payload_len = buf_len;
    return NIPC_OK;
}

nipc_error_t nipc_cgroups_resp_item(const nipc_cgroups_resp_view_t *view,
                                    uint32_t index,
                                    nipc_cgroups_item_view_t *out) {
    if (index >= view->item_count)
        return NIPC_ERR_OUT_OF_BOUNDS;

    size_t dir_start = NIPC_CGROUPS_RESP_HDR_SIZE;
    size_t dir_size  = (size_t)view->item_count * NIPC_CGROUPS_DIR_ENTRY_SIZE;
    size_t packed_area_start = dir_start + dir_size;

    const uint8_t *dir = view->_payload + dir_start;
    uint32_t item_off = get_u32(dir + index * 8);
    uint32_t item_len = get_u32(dir + index * 8 + 4);

    const uint8_t *item = view->_payload + packed_area_start + item_off;

    out->layout_version = get_u16(item + 0);
    out->flags          = get_u16(item + 2);
    out->hash           = get_u32(item + 4);
    out->options        = get_u32(item + 8);
    out->enabled        = get_u32(item + 12);

    uint32_t name_off = get_u32(item + 16);
    uint32_t name_len = get_u32(item + 20);
    uint32_t path_off = get_u32(item + 24);
    uint32_t path_len = get_u32(item + 28);

    if (out->layout_version != 1)
        return NIPC_ERR_BAD_LAYOUT;

    /* String offsets are relative to item start.
     * First valid offset is 32 (the item header size). */
    if (name_off < NIPC_CGROUPS_ITEM_HDR_SIZE)
        return NIPC_ERR_OUT_OF_BOUNDS;
    if ((uint64_t)name_off + name_len + 1 > item_len)
        return NIPC_ERR_OUT_OF_BOUNDS;
    if (item[name_off + name_len] != '\0')
        return NIPC_ERR_MISSING_NUL;

    if (path_off < NIPC_CGROUPS_ITEM_HDR_SIZE)
        return NIPC_ERR_OUT_OF_BOUNDS;
    if ((uint64_t)path_off + path_len + 1 > item_len)
        return NIPC_ERR_OUT_OF_BOUNDS;
    if (item[path_off + path_len] != '\0')
        return NIPC_ERR_MISSING_NUL;

    out->name.ptr = (const char *)(item + name_off);
    out->name.len = name_len;
    out->path.ptr = (const char *)(item + path_off);
    out->path.len = path_len;

    return NIPC_OK;
}

/* ------------------------------------------------------------------ */
/*  Cgroups snapshot response builder                                 */
/*                                                                    */
/*  Layout during building (max_items directory slots reserved):      */
/*    [24-byte header space] [max_items*8 directory] [packed items]   */
/*                                                                    */
/*  Layout after finish (compacted to actual item_count):             */
/*    [24-byte header] [item_count*8 directory] [packed items]        */
/*                                                                    */
/*  If item_count < max_items, finish() shifts packed data left and   */
/*  adjusts directory offsets accordingly.                             */
/* ------------------------------------------------------------------ */

void nipc_cgroups_builder_init(nipc_cgroups_builder_t *b,
                               void *buf, size_t buf_len,
                               uint32_t max_items,
                               uint32_t systemd_enabled,
                               uint64_t generation) {
    b->buf             = (uint8_t *)buf;
    b->buf_len         = buf_len;
    b->systemd_enabled = systemd_enabled;
    b->generation      = generation;
    b->item_count      = 0;
    b->max_items       = max_items;

    /* Packed item data starts after reserved directory */
    b->data_offset = NIPC_CGROUPS_RESP_HDR_SIZE +
                     (size_t)max_items * NIPC_CGROUPS_DIR_ENTRY_SIZE;
}

nipc_error_t nipc_cgroups_builder_add(nipc_cgroups_builder_t *b,
                                      uint32_t hash,
                                      uint32_t options,
                                      uint32_t enabled,
                                      const char *name, uint32_t name_len,
                                      const char *path, uint32_t path_len) {
    if (b->item_count >= b->max_items)
        return NIPC_ERR_OVERFLOW;

    /* Align item start to 8 bytes */
    size_t item_start = nipc_align8(b->data_offset);

    /* Item payload: 32-byte header + name + NUL + path + NUL */
    size_t item_size = NIPC_CGROUPS_ITEM_HDR_SIZE +
                       (size_t)name_len + 1 +
                       (size_t)path_len + 1;

    if (item_start + item_size > b->buf_len)
        return NIPC_ERR_OVERFLOW;

    /* Zero alignment padding */
    if (item_start > b->data_offset)
        memset(b->buf + b->data_offset, 0, item_start - b->data_offset);

    uint8_t *item = b->buf + item_start;

    uint32_t name_offset = NIPC_CGROUPS_ITEM_HDR_SIZE;
    uint32_t path_offset = NIPC_CGROUPS_ITEM_HDR_SIZE + name_len + 1;

    /* Write item header */
    put_u16(item + 0,  1);         /* layout_version */
    put_u16(item + 2,  0);         /* flags */
    put_u32(item + 4,  hash);
    put_u32(item + 8,  options);
    put_u32(item + 12, enabled);
    put_u32(item + 16, name_offset);
    put_u32(item + 20, name_len);
    put_u32(item + 24, path_offset);
    put_u32(item + 28, path_len);

    /* Write strings with NUL terminators */
    memcpy(item + name_offset, name, name_len);
    item[name_offset + name_len] = '\0';
    memcpy(item + path_offset, path, path_len);
    item[path_offset + path_len] = '\0';

    /* Write directory entry (absolute offset stored temporarily) */
    size_t dir_entry = NIPC_CGROUPS_RESP_HDR_SIZE +
                       (size_t)b->item_count * NIPC_CGROUPS_DIR_ENTRY_SIZE;
    put_u32(b->buf + dir_entry,     (uint32_t)item_start);
    put_u32(b->buf + dir_entry + 4, (uint32_t)item_size);

    b->data_offset = item_start + item_size;
    b->item_count++;
    return NIPC_OK;
}

size_t nipc_cgroups_builder_finish(nipc_cgroups_builder_t *b) {
    uint8_t *p = b->buf;

    if (b->item_count == 0) {
        put_u16(p + 0,  1);
        put_u16(p + 2,  0);
        put_u32(p + 4,  0);
        put_u32(p + 8,  b->systemd_enabled);
        put_u32(p + 12, 0);
        put_u64(p + 16, b->generation);
        return NIPC_CGROUPS_RESP_HDR_SIZE;
    }

    /* Where the decoder expects packed data to start */
    size_t final_packed_start = NIPC_CGROUPS_RESP_HDR_SIZE +
                                (size_t)b->item_count * NIPC_CGROUPS_DIR_ENTRY_SIZE;

    /* Read the first directory entry to find where packed data actually begins */
    uint32_t first_item_abs = get_u32(p + NIPC_CGROUPS_RESP_HDR_SIZE);

    size_t packed_data_len = b->data_offset - first_item_abs;

    if (final_packed_start < first_item_abs) {
        /* Need to shift packed data left. Use memmove because
         * source and destination may overlap. */
        memmove(p + final_packed_start, p + first_item_abs, packed_data_len);
    }

    /* Convert directory entries from absolute offsets to relative offsets
     * (relative to final_packed_start) */
    size_t dir_base = NIPC_CGROUPS_RESP_HDR_SIZE;
    for (uint32_t i = 0; i < b->item_count; i++) {
        size_t entry = dir_base + (size_t)i * NIPC_CGROUPS_DIR_ENTRY_SIZE;
        uint32_t abs_off = get_u32(p + entry);
        uint32_t rel_off = (uint32_t)(abs_off - first_item_abs);
        put_u32(p + entry, rel_off);
        /* length stays the same */
    }

    /* Write snapshot header */
    put_u16(p + 0,  1);
    put_u16(p + 2,  0);
    put_u32(p + 4,  b->item_count);
    put_u32(p + 8,  b->systemd_enabled);
    put_u32(p + 12, 0);
    put_u64(p + 16, b->generation);

    return final_packed_start + packed_data_len;
}

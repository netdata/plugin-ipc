/*
 * test_protocol.c - Unit tests for netipc wire envelope and codec.
 *
 * Tests cover:
 * - Encode/decode round-trips for all message types
 * - Validation rejection (truncated, out-of-bounds, bad magic, missing NUL)
 * - Batch assembly and extraction
 * - Cgroups snapshot builder with multiple items
 *
 * Returns 0 if all tests pass, 1 otherwise.
 */

#include "netipc/netipc_protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, name)                                  \
    do {                                                   \
        if (cond) {                                        \
            g_pass++;                                      \
        } else {                                           \
            g_fail++;                                      \
            fprintf(stderr, "FAIL: %s (line %d)\n",        \
                    name, __LINE__);                        \
        }                                                  \
    } while (0)

/* ================================================================== */
/*  Static assert verification (compile-time only)                    */
/* ================================================================== */

_Static_assert(sizeof(nipc_header_t) == 32, "header size");
_Static_assert(sizeof(nipc_chunk_header_t) == 32, "chunk header size");
_Static_assert(sizeof(nipc_batch_entry_t) == 8, "batch entry size");
_Static_assert(sizeof(nipc_cgroups_resp_header_t) == 24, "cgroups resp header size");

/* ================================================================== */
/*  Outer message header tests                                        */
/* ================================================================== */

static void test_header_roundtrip(void) {
    nipc_header_t h = {
        .magic            = NIPC_MAGIC_MSG,
        .version          = NIPC_VERSION,
        .header_len       = NIPC_HEADER_LEN,
        .kind             = NIPC_KIND_REQUEST,
        .flags            = NIPC_FLAG_BATCH,
        .code             = NIPC_METHOD_CGROUPS_SNAPSHOT,
        .transport_status = NIPC_STATUS_OK,
        .payload_len      = 12345,
        .item_count       = 42,
        .message_id       = 0xDEADBEEFCAFEBABEULL,
    };

    uint8_t buf[64];
    size_t n = nipc_header_encode(&h, buf, sizeof(buf));
    CHECK(n == 32, "header encode returns 32");

    nipc_header_t out;
    nipc_error_t err = nipc_header_decode(buf, n, &out);
    CHECK(err == NIPC_OK, "header decode ok");
    CHECK(out.magic == h.magic, "header magic");
    CHECK(out.version == h.version, "header version");
    CHECK(out.header_len == h.header_len, "header header_len");
    CHECK(out.kind == h.kind, "header kind");
    CHECK(out.flags == h.flags, "header flags");
    CHECK(out.code == h.code, "header code");
    CHECK(out.transport_status == h.transport_status, "header transport_status");
    CHECK(out.payload_len == h.payload_len, "header payload_len");
    CHECK(out.item_count == h.item_count, "header item_count");
    CHECK(out.message_id == h.message_id, "header message_id");
}

static void test_header_encode_too_small(void) {
    nipc_header_t h = {0};
    uint8_t buf[16];
    size_t n = nipc_header_encode(&h, buf, sizeof(buf));
    CHECK(n == 0, "header encode too small returns 0");
}

static void test_header_decode_truncated(void) {
    uint8_t buf[31] = {0};
    nipc_header_t out;
    nipc_error_t err = nipc_header_decode(buf, sizeof(buf), &out);
    CHECK(err == NIPC_ERR_TRUNCATED, "header decode truncated");
}

static void test_header_decode_bad_magic(void) {
    nipc_header_t h = {
        .magic = 0x12345678,
        .version = NIPC_VERSION,
        .header_len = NIPC_HEADER_LEN,
        .kind = NIPC_KIND_REQUEST,
    };
    uint8_t buf[32];
    nipc_header_encode(&h, buf, sizeof(buf));

    /* Fix: encode wrote the bad magic, decode should catch it */
    nipc_header_t out;
    nipc_error_t err = nipc_header_decode(buf, sizeof(buf), &out);
    CHECK(err == NIPC_ERR_BAD_MAGIC, "header decode bad magic");
}

static void test_header_decode_bad_version(void) {
    nipc_header_t h = {
        .magic = NIPC_MAGIC_MSG,
        .version = 99,
        .header_len = NIPC_HEADER_LEN,
        .kind = NIPC_KIND_REQUEST,
    };
    uint8_t buf[32];
    nipc_header_encode(&h, buf, sizeof(buf));

    nipc_header_t out;
    nipc_error_t err = nipc_header_decode(buf, sizeof(buf), &out);
    CHECK(err == NIPC_ERR_BAD_VERSION, "header decode bad version");
}

static void test_header_decode_bad_header_len(void) {
    nipc_header_t h = {
        .magic = NIPC_MAGIC_MSG,
        .version = NIPC_VERSION,
        .header_len = 64, /* wrong */
        .kind = NIPC_KIND_REQUEST,
    };
    uint8_t buf[32];
    nipc_header_encode(&h, buf, sizeof(buf));

    nipc_header_t out;
    nipc_error_t err = nipc_header_decode(buf, sizeof(buf), &out);
    CHECK(err == NIPC_ERR_BAD_HEADER_LEN, "header decode bad header_len");
}

static void test_header_decode_bad_kind(void) {
    nipc_header_t h = {
        .magic = NIPC_MAGIC_MSG,
        .version = NIPC_VERSION,
        .header_len = NIPC_HEADER_LEN,
        .kind = 0, /* invalid */
    };
    uint8_t buf[32];
    nipc_header_encode(&h, buf, sizeof(buf));

    nipc_header_t out;
    nipc_error_t err = nipc_header_decode(buf, sizeof(buf), &out);
    CHECK(err == NIPC_ERR_BAD_KIND, "header decode kind=0");

    h.kind = 4; /* also invalid */
    nipc_header_encode(&h, buf, sizeof(buf));
    err = nipc_header_decode(buf, sizeof(buf), &out);
    CHECK(err == NIPC_ERR_BAD_KIND, "header decode kind=4");
}

static void test_header_all_kinds(void) {
    for (uint16_t k = NIPC_KIND_REQUEST; k <= NIPC_KIND_CONTROL; k++) {
        nipc_header_t h = {
            .magic = NIPC_MAGIC_MSG,
            .version = NIPC_VERSION,
            .header_len = NIPC_HEADER_LEN,
            .kind = k,
        };
        uint8_t buf[32];
        nipc_header_encode(&h, buf, sizeof(buf));

        nipc_header_t out;
        nipc_error_t err = nipc_header_decode(buf, sizeof(buf), &out);
        CHECK(err == NIPC_OK, "header valid kind roundtrip");
        CHECK(out.kind == k, "header kind value preserved");
    }
}

/* ================================================================== */
/*  Chunk continuation header tests                                   */
/* ================================================================== */

static void test_chunk_header_roundtrip(void) {
    nipc_chunk_header_t c = {
        .magic             = NIPC_MAGIC_CHUNK,
        .version           = NIPC_VERSION,
        .flags             = 0,
        .message_id        = 0x1234567890ABCDEFULL,
        .total_message_len = 100000,
        .chunk_index       = 3,
        .chunk_count       = 10,
        .chunk_payload_len = 8192,
    };

    uint8_t buf[64];
    size_t n = nipc_chunk_header_encode(&c, buf, sizeof(buf));
    CHECK(n == 32, "chunk encode returns 32");

    nipc_chunk_header_t out;
    nipc_error_t err = nipc_chunk_header_decode(buf, n, &out);
    CHECK(err == NIPC_OK, "chunk decode ok");
    CHECK(out.magic == c.magic, "chunk magic");
    CHECK(out.version == c.version, "chunk version");
    CHECK(out.flags == c.flags, "chunk flags");
    CHECK(out.message_id == c.message_id, "chunk message_id");
    CHECK(out.total_message_len == c.total_message_len, "chunk total_message_len");
    CHECK(out.chunk_index == c.chunk_index, "chunk chunk_index");
    CHECK(out.chunk_count == c.chunk_count, "chunk chunk_count");
    CHECK(out.chunk_payload_len == c.chunk_payload_len, "chunk chunk_payload_len");
}

static void test_chunk_decode_truncated(void) {
    uint8_t buf[31] = {0};
    nipc_chunk_header_t out;
    nipc_error_t err = nipc_chunk_header_decode(buf, sizeof(buf), &out);
    CHECK(err == NIPC_ERR_TRUNCATED, "chunk decode truncated");
}

static void test_chunk_decode_bad_magic(void) {
    nipc_chunk_header_t c = {
        .magic = NIPC_MAGIC_MSG, /* wrong magic for chunk */
        .version = NIPC_VERSION,
    };
    uint8_t buf[32];
    nipc_chunk_header_encode(&c, buf, sizeof(buf));

    nipc_chunk_header_t out;
    nipc_error_t err = nipc_chunk_header_decode(buf, sizeof(buf), &out);
    CHECK(err == NIPC_ERR_BAD_MAGIC, "chunk decode bad magic");
}

static void test_chunk_decode_bad_version(void) {
    nipc_chunk_header_t c = {
        .magic = NIPC_MAGIC_CHUNK,
        .version = 2,
    };
    uint8_t buf[32];
    nipc_chunk_header_encode(&c, buf, sizeof(buf));

    nipc_chunk_header_t out;
    nipc_error_t err = nipc_chunk_header_decode(buf, sizeof(buf), &out);
    CHECK(err == NIPC_ERR_BAD_VERSION, "chunk decode bad version");
}

static void test_chunk_encode_too_small(void) {
    nipc_chunk_header_t c = {0};
    uint8_t buf[16];
    size_t n = nipc_chunk_header_encode(&c, buf, sizeof(buf));
    CHECK(n == 0, "chunk encode too small returns 0");
}

/* ================================================================== */
/*  Batch item directory tests                                        */
/* ================================================================== */

static void test_batch_dir_roundtrip(void) {
    nipc_batch_entry_t entries[3] = {
        {.offset = 0,  .length = 100},
        {.offset = 104, .length = 200},  /* 104 -> align8(100)=104 */
        {.offset = 304, .length = 50},   /* 304 -> align8(304)=304 */
    };
    /* Fix: offsets must be 8-byte aligned */
    entries[1].offset = 104; /* 100 rounded up to 104 */
    entries[2].offset = 304;

    uint8_t buf[64];
    size_t n = nipc_batch_dir_encode(entries, 3, buf, sizeof(buf));
    CHECK(n == 24, "batch dir encode 3 entries = 24 bytes");

    nipc_batch_entry_t out[3];
    nipc_error_t err = nipc_batch_dir_decode(buf, n, 3, 400, out);
    CHECK(err == NIPC_OK, "batch dir decode ok");
    CHECK(out[0].offset == 0 && out[0].length == 100, "batch entry 0");
    CHECK(out[1].offset == 104 && out[1].length == 200, "batch entry 1");
    CHECK(out[2].offset == 304 && out[2].length == 50, "batch entry 2");
}

static void test_batch_dir_decode_truncated(void) {
    uint8_t buf[12] = {0};
    nipc_batch_entry_t out[2];
    nipc_error_t err = nipc_batch_dir_decode(buf, sizeof(buf), 2, 1000, out);
    CHECK(err == NIPC_ERR_TRUNCATED, "batch dir decode truncated");
}

static void test_batch_dir_decode_oob(void) {
    /* Entry offset+length exceeds packed area */
    nipc_batch_entry_t e = {.offset = 0, .length = 200};
    uint8_t buf[8];
    nipc_batch_dir_encode(&e, 1, buf, sizeof(buf));

    nipc_batch_entry_t out;
    nipc_error_t err = nipc_batch_dir_decode(buf, 8, 1, 100, &out);
    CHECK(err == NIPC_ERR_OUT_OF_BOUNDS, "batch dir oob");
}

static void test_batch_dir_decode_bad_alignment(void) {
    /* Manually write an entry with unaligned offset */
    uint8_t buf[8];
    uint32_t bad_off = 3; /* not 8-byte aligned */
    uint32_t len = 10;
    memcpy(buf, &bad_off, 4);
    memcpy(buf + 4, &len, 4);

    nipc_batch_entry_t out;
    nipc_error_t err = nipc_batch_dir_decode(buf, 8, 1, 100, &out);
    CHECK(err == NIPC_ERR_BAD_ALIGNMENT, "batch dir bad alignment");
}

/* ================================================================== */
/*  Batch builder + extraction tests                                  */
/* ================================================================== */

static void test_batch_builder_roundtrip(void) {
    uint8_t buf[1024];
    nipc_batch_builder_t b;
    nipc_batch_builder_init(&b, buf, sizeof(buf), 4);

    uint8_t item1[] = {1, 2, 3, 4, 5};
    uint8_t item2[] = {10, 20, 30};
    uint8_t item3[] = {0xAA, 0xBB};

    CHECK(nipc_batch_builder_add(&b, item1, sizeof(item1)) == NIPC_OK,
          "batch add item1");
    CHECK(nipc_batch_builder_add(&b, item2, sizeof(item2)) == NIPC_OK,
          "batch add item2");
    CHECK(nipc_batch_builder_add(&b, item3, sizeof(item3)) == NIPC_OK,
          "batch add item3");

    uint32_t count;
    size_t total = nipc_batch_builder_finish(&b, &count);
    CHECK(count == 3, "batch finish count");
    CHECK(total > 0, "batch finish size > 0");

    /* Extract items back */
    const void *ptr;
    uint32_t len;

    CHECK(nipc_batch_item_get(buf, total, 3, 0, &ptr, &len) == NIPC_OK,
          "batch get item 0");
    CHECK(len == sizeof(item1), "batch item 0 len");
    CHECK(memcmp(ptr, item1, len) == 0, "batch item 0 data");

    CHECK(nipc_batch_item_get(buf, total, 3, 1, &ptr, &len) == NIPC_OK,
          "batch get item 1");
    CHECK(len == sizeof(item2), "batch item 1 len");
    CHECK(memcmp(ptr, item2, len) == 0, "batch item 1 data");

    CHECK(nipc_batch_item_get(buf, total, 3, 2, &ptr, &len) == NIPC_OK,
          "batch get item 2");
    CHECK(len == sizeof(item3), "batch item 2 len");
    CHECK(memcmp(ptr, item3, len) == 0, "batch item 2 data");
}

static void test_batch_builder_overflow(void) {
    uint8_t buf[32];
    nipc_batch_builder_t b;
    nipc_batch_builder_init(&b, buf, sizeof(buf), 1);

    uint8_t item[] = {1};
    CHECK(nipc_batch_builder_add(&b, item, sizeof(item)) == NIPC_OK,
          "batch add first ok");
    CHECK(nipc_batch_builder_add(&b, item, sizeof(item)) == NIPC_ERR_OVERFLOW,
          "batch add overflow (max items)");
}

static void test_batch_builder_buf_overflow(void) {
    uint8_t buf[24]; /* 1*8 dir + 8 aligned = 16, very tight */
    nipc_batch_builder_t b;
    nipc_batch_builder_init(&b, buf, sizeof(buf), 1);

    uint8_t big[100];
    CHECK(nipc_batch_builder_add(&b, big, sizeof(big)) == NIPC_ERR_OVERFLOW,
          "batch add buffer overflow");
}

static void test_batch_item_get_oob_index(void) {
    uint8_t buf[64];
    nipc_batch_builder_t b;
    nipc_batch_builder_init(&b, buf, sizeof(buf), 2);

    uint8_t item[] = {1};
    nipc_batch_builder_add(&b, item, sizeof(item));

    uint32_t count;
    size_t total = nipc_batch_builder_finish(&b, &count);

    const void *ptr;
    uint32_t len;
    CHECK(nipc_batch_item_get(buf, total, count, 5, &ptr, &len)
          == NIPC_ERR_OUT_OF_BOUNDS,
          "batch get oob index");
}

static void test_batch_empty(void) {
    uint8_t buf[64];
    nipc_batch_builder_t b;
    nipc_batch_builder_init(&b, buf, sizeof(buf), 4);

    uint32_t count;
    size_t total = nipc_batch_builder_finish(&b, &count);
    CHECK(count == 0, "batch empty count");
    /* 0 items -> no directory, no data -> total = 0 */
    CHECK(total == 0, "batch empty size");
}

/* ================================================================== */
/*  Hello payload tests                                               */
/* ================================================================== */

static void test_hello_roundtrip(void) {
    nipc_hello_t h = {
        .layout_version            = 1,
        .flags                     = 0,
        .supported_profiles        = NIPC_PROFILE_BASELINE | NIPC_PROFILE_SHM_FUTEX,
        .preferred_profiles        = NIPC_PROFILE_SHM_FUTEX,
        .max_request_payload_bytes = 4096,
        .max_request_batch_items   = 100,
        .max_response_payload_bytes = 1048576,
        .max_response_batch_items  = 1,
        .auth_token                = 0xAABBCCDDEEFF0011ULL,
        .packet_size               = 65536,
    };

    uint8_t buf[64];
    size_t n = nipc_hello_encode(&h, buf, sizeof(buf));
    CHECK(n == 44, "hello encode returns 44");

    nipc_hello_t out;
    nipc_error_t err = nipc_hello_decode(buf, n, &out);
    CHECK(err == NIPC_OK, "hello decode ok");
    CHECK(out.layout_version == 1, "hello layout_version");
    CHECK(out.flags == 0, "hello flags");
    CHECK(out.supported_profiles == h.supported_profiles, "hello supported");
    CHECK(out.preferred_profiles == h.preferred_profiles, "hello preferred");
    CHECK(out.max_request_payload_bytes == 4096, "hello max_req_payload");
    CHECK(out.max_request_batch_items == 100, "hello max_req_batch");
    CHECK(out.max_response_payload_bytes == 1048576, "hello max_resp_payload");
    CHECK(out.max_response_batch_items == 1, "hello max_resp_batch");
    CHECK(out.auth_token == h.auth_token, "hello auth_token");
    CHECK(out.packet_size == 65536, "hello packet_size");
}

static void test_hello_decode_truncated(void) {
    uint8_t buf[43] = {0};
    nipc_hello_t out;
    nipc_error_t err = nipc_hello_decode(buf, sizeof(buf), &out);
    CHECK(err == NIPC_ERR_TRUNCATED, "hello decode truncated");
}

static void test_hello_decode_bad_layout(void) {
    nipc_hello_t h = {.layout_version = 99};
    uint8_t buf[44];
    nipc_hello_encode(&h, buf, sizeof(buf));
    /* Manually fix magic: encode writes layout_version=99 */
    /* Actually encode doesn't validate, so it wrote 99 at offset 0.
     * Decode will read layout_version=99 and reject. */

    nipc_hello_t out;
    nipc_error_t err = nipc_hello_decode(buf, sizeof(buf), &out);
    CHECK(err == NIPC_ERR_BAD_LAYOUT, "hello decode bad layout");
}

static void test_hello_encode_too_small(void) {
    nipc_hello_t h = {0};
    uint8_t buf[10];
    size_t n = nipc_hello_encode(&h, buf, sizeof(buf));
    CHECK(n == 0, "hello encode too small");
}

/* ================================================================== */
/*  Hello-ack payload tests                                           */
/* ================================================================== */

static void test_hello_ack_roundtrip(void) {
    nipc_hello_ack_t h = {
        .layout_version                    = 1,
        .flags                             = 0,
        .server_supported_profiles         = 0x07,
        .intersection_profiles             = 0x05,
        .selected_profile                  = NIPC_PROFILE_SHM_FUTEX,
        .agreed_max_request_payload_bytes  = 2048,
        .agreed_max_request_batch_items    = 50,
        .agreed_max_response_payload_bytes = 65536,
        .agreed_max_response_batch_items   = 1,
        .agreed_packet_size                = 32768,
    };

    uint8_t buf[64];
    size_t n = nipc_hello_ack_encode(&h, buf, sizeof(buf));
    CHECK(n == 36, "hello_ack encode returns 36");

    nipc_hello_ack_t out;
    nipc_error_t err = nipc_hello_ack_decode(buf, n, &out);
    CHECK(err == NIPC_OK, "hello_ack decode ok");
    CHECK(out.layout_version == 1, "hello_ack layout_version");
    CHECK(out.server_supported_profiles == 0x07, "hello_ack server_supported");
    CHECK(out.intersection_profiles == 0x05, "hello_ack intersection");
    CHECK(out.selected_profile == NIPC_PROFILE_SHM_FUTEX, "hello_ack selected");
    CHECK(out.agreed_max_request_payload_bytes == 2048, "hello_ack agreed_req_payload");
    CHECK(out.agreed_max_request_batch_items == 50, "hello_ack agreed_req_batch");
    CHECK(out.agreed_max_response_payload_bytes == 65536, "hello_ack agreed_resp_payload");
    CHECK(out.agreed_max_response_batch_items == 1, "hello_ack agreed_resp_batch");
    CHECK(out.agreed_packet_size == 32768, "hello_ack agreed_pkt_size");
}

static void test_hello_ack_decode_truncated(void) {
    uint8_t buf[35] = {0};
    nipc_hello_ack_t out;
    nipc_error_t err = nipc_hello_ack_decode(buf, sizeof(buf), &out);
    CHECK(err == NIPC_ERR_TRUNCATED, "hello_ack decode truncated");
}

static void test_hello_ack_decode_bad_layout(void) {
    nipc_hello_ack_t h = {.layout_version = 0};
    uint8_t buf[36];
    nipc_hello_ack_encode(&h, buf, sizeof(buf));

    nipc_hello_ack_t out;
    nipc_error_t err = nipc_hello_ack_decode(buf, sizeof(buf), &out);
    CHECK(err == NIPC_ERR_BAD_LAYOUT, "hello_ack decode bad layout");
}

static void test_hello_ack_encode_too_small(void) {
    nipc_hello_ack_t h = {0};
    uint8_t buf[10];
    size_t n = nipc_hello_ack_encode(&h, buf, sizeof(buf));
    CHECK(n == 0, "hello_ack encode too small");
}

/* ================================================================== */
/*  Cgroups snapshot request tests                                    */
/* ================================================================== */

static void test_cgroups_req_roundtrip(void) {
    nipc_cgroups_req_t r = {.layout_version = 1, .flags = 0};

    uint8_t buf[16];
    size_t n = nipc_cgroups_req_encode(&r, buf, sizeof(buf));
    CHECK(n == 4, "cgroups req encode returns 4");

    nipc_cgroups_req_t out;
    nipc_error_t err = nipc_cgroups_req_decode(buf, n, &out);
    CHECK(err == NIPC_OK, "cgroups req decode ok");
    CHECK(out.layout_version == 1, "cgroups req layout_version");
    CHECK(out.flags == 0, "cgroups req flags");
}

static void test_cgroups_req_decode_truncated(void) {
    uint8_t buf[3] = {0};
    nipc_cgroups_req_t out;
    nipc_error_t err = nipc_cgroups_req_decode(buf, sizeof(buf), &out);
    CHECK(err == NIPC_ERR_TRUNCATED, "cgroups req truncated");
}

static void test_cgroups_req_decode_bad_layout(void) {
    nipc_cgroups_req_t r = {.layout_version = 5};
    uint8_t buf[4];
    nipc_cgroups_req_encode(&r, buf, sizeof(buf));

    nipc_cgroups_req_t out;
    nipc_error_t err = nipc_cgroups_req_decode(buf, sizeof(buf), &out);
    CHECK(err == NIPC_ERR_BAD_LAYOUT, "cgroups req bad layout");
}

static void test_cgroups_req_encode_too_small(void) {
    nipc_cgroups_req_t r = {0};
    uint8_t buf[2];
    size_t n = nipc_cgroups_req_encode(&r, buf, sizeof(buf));
    CHECK(n == 0, "cgroups req encode too small");
}

/* ================================================================== */
/*  Cgroups snapshot response tests                                   */
/* ================================================================== */

static void test_cgroups_resp_empty(void) {
    uint8_t buf[4096];
    nipc_cgroups_builder_t b;
    nipc_cgroups_builder_init(&b, buf, sizeof(buf), 0, 1, 42);

    size_t total = nipc_cgroups_builder_finish(&b);
    CHECK(total == 24, "empty snapshot = 24 bytes");

    nipc_cgroups_resp_view_t view;
    nipc_error_t err = nipc_cgroups_resp_decode(buf, total, &view);
    CHECK(err == NIPC_OK, "empty snapshot decode ok");
    CHECK(view.item_count == 0, "empty snapshot item_count");
    CHECK(view.systemd_enabled == 1, "empty snapshot systemd_enabled");
    CHECK(view.generation == 42, "empty snapshot generation");
}

static void test_cgroups_resp_single_item(void) {
    uint8_t buf[4096];
    nipc_cgroups_builder_t b;
    nipc_cgroups_builder_init(&b, buf, sizeof(buf), 1, 0, 100);

    const char *name = "docker-abc123";
    const char *path = "/sys/fs/cgroup/docker/abc123";
    nipc_error_t err = nipc_cgroups_builder_add(
        &b, 12345, 0x01, 1,
        name, (uint32_t)strlen(name),
        path, (uint32_t)strlen(path));
    CHECK(err == NIPC_OK, "single item add ok");

    size_t total = nipc_cgroups_builder_finish(&b);
    CHECK(total > 24, "single item total > header");

    nipc_cgroups_resp_view_t view;
    err = nipc_cgroups_resp_decode(buf, total, &view);
    CHECK(err == NIPC_OK, "single item decode ok");
    CHECK(view.item_count == 1, "single item count");
    CHECK(view.systemd_enabled == 0, "single item systemd_enabled");
    CHECK(view.generation == 100, "single item generation");

    nipc_cgroups_item_view_t item;
    err = nipc_cgroups_resp_item(&view, 0, &item);
    CHECK(err == NIPC_OK, "single item access ok");
    CHECK(item.hash == 12345, "single item hash");
    CHECK(item.options == 0x01, "single item options");
    CHECK(item.enabled == 1, "single item enabled");
    CHECK(item.name.len == strlen(name), "single item name len");
    CHECK(memcmp(item.name.ptr, name, item.name.len) == 0, "single item name data");
    CHECK(item.name.ptr[item.name.len] == '\0', "single item name NUL");
    CHECK(item.path.len == strlen(path), "single item path len");
    CHECK(memcmp(item.path.ptr, path, item.path.len) == 0, "single item path data");
    CHECK(item.path.ptr[item.path.len] == '\0', "single item path NUL");
}

static void test_cgroups_resp_multiple_items(void) {
    uint8_t buf[8192];
    nipc_cgroups_builder_t b;
    nipc_cgroups_builder_init(&b, buf, sizeof(buf), 5, 1, 999);

    /* Item 0 */
    const char *n0 = "init.scope";
    const char *p0 = "/sys/fs/cgroup/init.scope";
    CHECK(nipc_cgroups_builder_add(&b, 100, 0, 1,
                                    n0, (uint32_t)strlen(n0),
                                    p0, (uint32_t)strlen(p0)) == NIPC_OK,
          "multi add item 0");

    /* Item 1 */
    const char *n1 = "system.slice/docker-abc.scope";
    const char *p1 = "/sys/fs/cgroup/system.slice/docker-abc.scope";
    CHECK(nipc_cgroups_builder_add(&b, 200, 0x02, 0,
                                    n1, (uint32_t)strlen(n1),
                                    p1, (uint32_t)strlen(p1)) == NIPC_OK,
          "multi add item 1");

    /* Item 2 - empty strings */
    CHECK(nipc_cgroups_builder_add(&b, 300, 0, 1,
                                    "", 0, "", 0) == NIPC_OK,
          "multi add item 2 (empty strings)");

    size_t total = nipc_cgroups_builder_finish(&b);

    nipc_cgroups_resp_view_t view;
    nipc_error_t err = nipc_cgroups_resp_decode(buf, total, &view);
    CHECK(err == NIPC_OK, "multi decode ok");
    CHECK(view.item_count == 3, "multi item count");
    CHECK(view.systemd_enabled == 1, "multi systemd_enabled");
    CHECK(view.generation == 999, "multi generation");

    /* Verify item 0 */
    nipc_cgroups_item_view_t item;
    err = nipc_cgroups_resp_item(&view, 0, &item);
    CHECK(err == NIPC_OK, "multi item 0 access");
    CHECK(item.hash == 100, "multi item 0 hash");
    CHECK(item.name.len == strlen(n0), "multi item 0 name len");
    CHECK(memcmp(item.name.ptr, n0, item.name.len) == 0, "multi item 0 name");
    CHECK(item.path.len == strlen(p0), "multi item 0 path len");
    CHECK(memcmp(item.path.ptr, p0, item.path.len) == 0, "multi item 0 path");

    /* Verify item 1 */
    err = nipc_cgroups_resp_item(&view, 1, &item);
    CHECK(err == NIPC_OK, "multi item 1 access");
    CHECK(item.hash == 200, "multi item 1 hash");
    CHECK(item.options == 0x02, "multi item 1 options");
    CHECK(item.enabled == 0, "multi item 1 enabled");
    CHECK(item.name.len == strlen(n1), "multi item 1 name len");
    CHECK(memcmp(item.name.ptr, n1, item.name.len) == 0, "multi item 1 name");

    /* Verify item 2 (empty strings) */
    err = nipc_cgroups_resp_item(&view, 2, &item);
    CHECK(err == NIPC_OK, "multi item 2 access");
    CHECK(item.hash == 300, "multi item 2 hash");
    CHECK(item.name.len == 0, "multi item 2 name empty");
    CHECK(item.name.ptr[0] == '\0', "multi item 2 name NUL");
    CHECK(item.path.len == 0, "multi item 2 path empty");
    CHECK(item.path.ptr[0] == '\0', "multi item 2 path NUL");

    /* Out-of-bounds index */
    err = nipc_cgroups_resp_item(&view, 3, &item);
    CHECK(err == NIPC_ERR_OUT_OF_BOUNDS, "multi item oob index");
}

static void test_cgroups_resp_decode_truncated_header(void) {
    uint8_t buf[23] = {0};
    nipc_cgroups_resp_view_t view;
    nipc_error_t err = nipc_cgroups_resp_decode(buf, sizeof(buf), &view);
    CHECK(err == NIPC_ERR_TRUNCATED, "cgroups resp truncated header");
}

static void test_cgroups_resp_decode_bad_layout(void) {
    /* Build a minimal valid payload but with wrong layout_version */
    uint8_t buf[24];
    memset(buf, 0, sizeof(buf));
    /* layout_version = 99 at offset 0 */
    uint16_t bad_ver = 99;
    memcpy(buf, &bad_ver, 2);

    nipc_cgroups_resp_view_t view;
    nipc_error_t err = nipc_cgroups_resp_decode(buf, sizeof(buf), &view);
    CHECK(err == NIPC_ERR_BAD_LAYOUT, "cgroups resp bad layout");
}

static void test_cgroups_resp_decode_truncated_dir(void) {
    /* Header says item_count=2, but payload is only 24 bytes (header only) */
    uint8_t buf[24];
    memset(buf, 0, sizeof(buf));
    uint16_t ver = 1;
    memcpy(buf, &ver, 2);
    uint32_t count = 2;
    memcpy(buf + 4, &count, 4);

    nipc_cgroups_resp_view_t view;
    nipc_error_t err = nipc_cgroups_resp_decode(buf, sizeof(buf), &view);
    CHECK(err == NIPC_ERR_TRUNCATED, "cgroups resp truncated dir");
}

static void test_cgroups_resp_decode_oob_dir(void) {
    /* Header + 1 dir entry, but dir entry points beyond payload */
    uint8_t buf[64];
    memset(buf, 0, sizeof(buf));
    uint16_t ver = 1;
    memcpy(buf, &ver, 2);
    uint32_t count = 1;
    memcpy(buf + 4, &count, 4);
    /* Dir entry at offset 24: offset=0, length=9999 (too big) */
    uint32_t off = 0;
    uint32_t len = 9999;
    memcpy(buf + 24, &off, 4);
    memcpy(buf + 28, &len, 4);

    nipc_cgroups_resp_view_t view;
    nipc_error_t err = nipc_cgroups_resp_decode(buf, sizeof(buf), &view);
    CHECK(err == NIPC_ERR_OUT_OF_BOUNDS, "cgroups resp oob dir entry");
}

static void test_cgroups_resp_decode_item_too_small(void) {
    /* Dir entry with length < 32 (item header size) */
    uint8_t buf[64];
    memset(buf, 0, sizeof(buf));
    uint16_t ver = 1;
    memcpy(buf, &ver, 2);
    uint32_t count = 1;
    memcpy(buf + 4, &count, 4);
    /* Dir entry: offset=0, length=16 (too small for 32-byte item header) */
    uint32_t off = 0;
    uint32_t len = 16;
    memcpy(buf + 24, &off, 4);
    memcpy(buf + 28, &len, 4);

    nipc_cgroups_resp_view_t view;
    nipc_error_t err = nipc_cgroups_resp_decode(buf, sizeof(buf), &view);
    CHECK(err == NIPC_ERR_TRUNCATED, "cgroups resp item too small");
}

static void test_cgroups_resp_item_missing_nul(void) {
    /* Build a valid snapshot with one item, then corrupt the NUL terminator */
    uint8_t buf[4096];
    nipc_cgroups_builder_t b;
    nipc_cgroups_builder_init(&b, buf, sizeof(buf), 1, 0, 1);

    const char *name = "test";
    const char *path = "/test";
    nipc_cgroups_builder_add(&b, 1, 0, 1,
                             name, (uint32_t)strlen(name),
                             path, (uint32_t)strlen(path));
    size_t total = nipc_cgroups_builder_finish(&b);

    nipc_cgroups_resp_view_t view;
    nipc_error_t err = nipc_cgroups_resp_decode(buf, total, &view);
    CHECK(err == NIPC_OK, "nul test: decode ok before corruption");

    /* Find the item data and corrupt the name's NUL terminator */
    size_t dir_end = NIPC_CGROUPS_RESP_HDR_SIZE +
                     1 * NIPC_CGROUPS_DIR_ENTRY_SIZE;
    uint32_t item_off, item_len_val;
    memcpy(&item_off, buf + NIPC_CGROUPS_RESP_HDR_SIZE, 4);
    memcpy(&item_len_val, buf + NIPC_CGROUPS_RESP_HDR_SIZE + 4, 4);

    uint8_t *item = buf + dir_end + item_off;
    /* name_offset is at item+16. Read it. */
    uint32_t noff;
    memcpy(&noff, item + 16, 4);
    uint32_t nlen;
    memcpy(&nlen, item + 20, 4);
    /* The NUL byte is at item[noff + nlen] */
    item[noff + nlen] = 'X'; /* corrupt */

    nipc_cgroups_item_view_t iv;
    err = nipc_cgroups_resp_item(&view, 0, &iv);
    CHECK(err == NIPC_ERR_MISSING_NUL, "cgroups item missing NUL");
}

static void test_cgroups_resp_item_string_oob(void) {
    /* Build valid snapshot, then corrupt string offset to point OOB */
    uint8_t buf[4096];
    nipc_cgroups_builder_t b;
    nipc_cgroups_builder_init(&b, buf, sizeof(buf), 1, 0, 1);

    const char *name = "test";
    const char *path = "/test";
    nipc_cgroups_builder_add(&b, 1, 0, 1,
                             name, (uint32_t)strlen(name),
                             path, (uint32_t)strlen(path));
    size_t total = nipc_cgroups_builder_finish(&b);

    nipc_cgroups_resp_view_t view;
    nipc_cgroups_resp_decode(buf, total, &view);

    /* Corrupt name_length to be huge */
    size_t dir_end = NIPC_CGROUPS_RESP_HDR_SIZE +
                     1 * NIPC_CGROUPS_DIR_ENTRY_SIZE;
    uint32_t item_off;
    memcpy(&item_off, buf + NIPC_CGROUPS_RESP_HDR_SIZE, 4);
    uint8_t *item = buf + dir_end + item_off;
    uint32_t huge = 99999;
    memcpy(item + 20, &huge, 4); /* name_length = huge */

    nipc_cgroups_item_view_t iv;
    nipc_error_t err = nipc_cgroups_resp_item(&view, 0, &iv);
    CHECK(err == NIPC_ERR_OUT_OF_BOUNDS, "cgroups item string oob");
}

static void test_cgroups_builder_overflow(void) {
    uint8_t buf[64]; /* too small for any real item */
    nipc_cgroups_builder_t b;
    nipc_cgroups_builder_init(&b, buf, sizeof(buf), 1, 0, 0);

    /* Try to add an item with long strings that won't fit */
    char long_name[200];
    memset(long_name, 'A', sizeof(long_name));
    nipc_error_t err = nipc_cgroups_builder_add(
        &b, 1, 0, 1, long_name, sizeof(long_name), "", 0);
    CHECK(err == NIPC_ERR_OVERFLOW, "builder overflow");
}

static void test_cgroups_builder_max_items_exceeded(void) {
    uint8_t buf[4096];
    nipc_cgroups_builder_t b;
    nipc_cgroups_builder_init(&b, buf, sizeof(buf), 1, 0, 0);

    CHECK(nipc_cgroups_builder_add(&b, 1, 0, 1, "a", 1, "b", 1) == NIPC_OK,
          "builder first add ok");
    CHECK(nipc_cgroups_builder_add(&b, 2, 0, 1, "c", 1, "d", 1) == NIPC_ERR_OVERFLOW,
          "builder max items exceeded");
}

/* Test with max_items > actual items (compaction in finish) */
static void test_cgroups_builder_compaction(void) {
    uint8_t buf[4096];
    nipc_cgroups_builder_t b;
    /* Reserve 10 directory slots but only add 2 items */
    nipc_cgroups_builder_init(&b, buf, sizeof(buf), 10, 1, 77);

    const char *n0 = "slice-a";
    const char *p0 = "/cgroup/slice-a";
    CHECK(nipc_cgroups_builder_add(&b, 10, 0, 1,
                                    n0, (uint32_t)strlen(n0),
                                    p0, (uint32_t)strlen(p0)) == NIPC_OK,
          "compact add item 0");

    const char *n1 = "slice-b";
    const char *p1 = "/cgroup/slice-b";
    CHECK(nipc_cgroups_builder_add(&b, 20, 0, 0,
                                    n1, (uint32_t)strlen(n1),
                                    p1, (uint32_t)strlen(p1)) == NIPC_OK,
          "compact add item 1");

    size_t total = nipc_cgroups_builder_finish(&b);

    /* Decode and verify */
    nipc_cgroups_resp_view_t view;
    nipc_error_t err = nipc_cgroups_resp_decode(buf, total, &view);
    CHECK(err == NIPC_OK, "compact decode ok");
    CHECK(view.item_count == 2, "compact item count");
    CHECK(view.generation == 77, "compact generation");

    nipc_cgroups_item_view_t item;
    err = nipc_cgroups_resp_item(&view, 0, &item);
    CHECK(err == NIPC_OK, "compact item 0 ok");
    CHECK(item.hash == 10, "compact item 0 hash");
    CHECK(item.name.len == strlen(n0), "compact item 0 name len");
    CHECK(memcmp(item.name.ptr, n0, item.name.len) == 0, "compact item 0 name");

    err = nipc_cgroups_resp_item(&view, 1, &item);
    CHECK(err == NIPC_OK, "compact item 1 ok");
    CHECK(item.hash == 20, "compact item 1 hash");
    CHECK(item.name.len == strlen(n1), "compact item 1 name len");
    CHECK(memcmp(item.name.ptr, n1, item.name.len) == 0, "compact item 1 name");
}

/* ================================================================== */
/*  Wire byte verification                                            */
/* ================================================================== */

static void test_header_wire_bytes(void) {
    /* Verify specific byte values for cross-language compatibility */
    nipc_header_t h = {
        .magic            = NIPC_MAGIC_MSG,
        .version          = NIPC_VERSION,
        .header_len       = NIPC_HEADER_LEN,
        .kind             = NIPC_KIND_REQUEST,
        .flags            = 0,
        .code             = NIPC_METHOD_CGROUPS_SNAPSHOT,
        .transport_status = NIPC_STATUS_OK,
        .payload_len      = 4,
        .item_count       = 1,
        .message_id       = 1,
    };

    uint8_t buf[32];
    nipc_header_encode(&h, buf, sizeof(buf));

    /* magic = 0x4e495043 LE: 43 50 49 4e */
    CHECK(buf[0] == 0x43 && buf[1] == 0x50 && buf[2] == 0x49 && buf[3] == 0x4e,
          "wire magic bytes");
    /* version = 1 LE: 01 00 */
    CHECK(buf[4] == 0x01 && buf[5] == 0x00, "wire version bytes");
    /* header_len = 32 LE: 20 00 */
    CHECK(buf[6] == 0x20 && buf[7] == 0x00, "wire header_len bytes");
    /* kind = 1 LE: 01 00 */
    CHECK(buf[8] == 0x01 && buf[9] == 0x00, "wire kind bytes");
    /* code = 2 LE: 02 00 */
    CHECK(buf[12] == 0x02 && buf[13] == 0x00, "wire code bytes");
}

static void test_chunk_wire_bytes(void) {
    nipc_chunk_header_t c = {
        .magic             = NIPC_MAGIC_CHUNK,
        .version           = NIPC_VERSION,
        .flags             = 0,
        .message_id        = 1,
        .total_message_len = 256,
        .chunk_index       = 1,
        .chunk_count       = 3,
        .chunk_payload_len = 100,
    };

    uint8_t buf[32];
    nipc_chunk_header_encode(&c, buf, sizeof(buf));

    /* magic = 0x4e43484b LE: 4b 48 43 4e */
    CHECK(buf[0] == 0x4b && buf[1] == 0x48 && buf[2] == 0x43 && buf[3] == 0x4e,
          "chunk wire magic bytes");
}

/* ================================================================== */
/*  Alignment utility test                                            */
/* ================================================================== */

static void test_align8(void) {
    CHECK(nipc_align8(0) == 0, "align8(0)");
    CHECK(nipc_align8(1) == 8, "align8(1)");
    CHECK(nipc_align8(7) == 8, "align8(7)");
    CHECK(nipc_align8(8) == 8, "align8(8)");
    CHECK(nipc_align8(9) == 16, "align8(9)");
    CHECK(nipc_align8(16) == 16, "align8(16)");
    CHECK(nipc_align8(17) == 24, "align8(17)");
}

/* ================================================================== */
/*  Main                                                              */
/* ================================================================== */

int main(void) {
    /* Header tests */
    test_header_roundtrip();
    test_header_encode_too_small();
    test_header_decode_truncated();
    test_header_decode_bad_magic();
    test_header_decode_bad_version();
    test_header_decode_bad_header_len();
    test_header_decode_bad_kind();
    test_header_all_kinds();
    test_header_wire_bytes();

    /* Chunk header tests */
    test_chunk_header_roundtrip();
    test_chunk_decode_truncated();
    test_chunk_decode_bad_magic();
    test_chunk_decode_bad_version();
    test_chunk_encode_too_small();
    test_chunk_wire_bytes();

    /* Batch directory tests */
    test_batch_dir_roundtrip();
    test_batch_dir_decode_truncated();
    test_batch_dir_decode_oob();
    test_batch_dir_decode_bad_alignment();

    /* Batch builder tests */
    test_batch_builder_roundtrip();
    test_batch_builder_overflow();
    test_batch_builder_buf_overflow();
    test_batch_item_get_oob_index();
    test_batch_empty();

    /* Hello tests */
    test_hello_roundtrip();
    test_hello_decode_truncated();
    test_hello_decode_bad_layout();
    test_hello_encode_too_small();

    /* Hello-ack tests */
    test_hello_ack_roundtrip();
    test_hello_ack_decode_truncated();
    test_hello_ack_decode_bad_layout();
    test_hello_ack_encode_too_small();

    /* Cgroups request tests */
    test_cgroups_req_roundtrip();
    test_cgroups_req_decode_truncated();
    test_cgroups_req_decode_bad_layout();
    test_cgroups_req_encode_too_small();

    /* Cgroups response tests */
    test_cgroups_resp_empty();
    test_cgroups_resp_single_item();
    test_cgroups_resp_multiple_items();
    test_cgroups_resp_decode_truncated_header();
    test_cgroups_resp_decode_bad_layout();
    test_cgroups_resp_decode_truncated_dir();
    test_cgroups_resp_decode_oob_dir();
    test_cgroups_resp_decode_item_too_small();
    test_cgroups_resp_item_missing_nul();
    test_cgroups_resp_item_string_oob();
    test_cgroups_builder_overflow();
    test_cgroups_builder_max_items_exceeded();
    test_cgroups_builder_compaction();

    /* Alignment utility */
    test_align8();

    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}

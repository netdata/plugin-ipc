/*
 * fuzz_protocol.c - Fuzz harness for all netipc protocol decode paths.
 *
 * Compile modes:
 *   1. libFuzzer:  clang -fsanitize=fuzzer,address -o fuzz_protocol fuzz_protocol.c ...
 *   2. AFL:        afl-clang-fast -o fuzz_protocol fuzz_protocol.c ... -DFUZZ_AFL
 *   3. Standalone: cc -o fuzz_protocol fuzz_protocol.c ... -DFUZZ_STANDALONE
 *                  Reads from stdin or from files given as arguments.
 *
 * The harness tries every decode function on the input. No input may
 * cause a crash or undefined behavior. Errors are fine and expected.
 */

#include "netipc/netipc_protocol.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Try to decode input as every message type. If any decode succeeds,
 * exercise the result (access fields, iterate items). */
static int fuzz_one(const uint8_t *data, size_t size) {
    /* --- Outer message header --- */
    {
        nipc_header_t hdr;
        nipc_error_t err = nipc_header_decode(data, size, &hdr);
        if (err == NIPC_OK) {
            /* Exercise fields */
            (void)hdr.magic;
            (void)hdr.version;
            (void)hdr.header_len;
            (void)hdr.kind;
            (void)hdr.flags;
            (void)hdr.code;
            (void)hdr.transport_status;
            (void)hdr.payload_len;
            (void)hdr.item_count;
            (void)hdr.message_id;
        }
    }

    /* --- Chunk continuation header --- */
    {
        nipc_chunk_header_t chk;
        nipc_error_t err = nipc_chunk_header_decode(data, size, &chk);
        if (err == NIPC_OK) {
            (void)chk.magic;
            (void)chk.version;
            (void)chk.flags;
            (void)chk.message_id;
            (void)chk.total_message_len;
            (void)chk.chunk_index;
            (void)chk.chunk_count;
            (void)chk.chunk_payload_len;
        }
    }

    /* --- Hello payload --- */
    {
        nipc_hello_t hello;
        nipc_error_t err = nipc_hello_decode(data, size, &hello);
        if (err == NIPC_OK) {
            (void)hello.layout_version;
            (void)hello.flags;
            (void)hello.supported_profiles;
            (void)hello.preferred_profiles;
            (void)hello.max_request_payload_bytes;
            (void)hello.max_request_batch_items;
            (void)hello.max_response_payload_bytes;
            (void)hello.max_response_batch_items;
            (void)hello.auth_token;
            (void)hello.packet_size;
        }
    }

    /* --- Hello-ack payload --- */
    {
        nipc_hello_ack_t ack;
        nipc_error_t err = nipc_hello_ack_decode(data, size, &ack);
        if (err == NIPC_OK) {
            (void)ack.layout_version;
            (void)ack.flags;
            (void)ack.server_supported_profiles;
            (void)ack.intersection_profiles;
            (void)ack.selected_profile;
            (void)ack.agreed_max_request_payload_bytes;
            (void)ack.agreed_max_request_batch_items;
            (void)ack.agreed_max_response_payload_bytes;
            (void)ack.agreed_max_response_batch_items;
            (void)ack.agreed_packet_size;
        }
    }

    /* --- Cgroups snapshot request --- */
    {
        nipc_cgroups_req_t req;
        nipc_error_t err = nipc_cgroups_req_decode(data, size, &req);
        if (err == NIPC_OK) {
            (void)req.layout_version;
            (void)req.flags;
        }
    }

    /* --- Cgroups snapshot response (+ item iteration) --- */
    {
        nipc_cgroups_resp_view_t view;
        nipc_error_t err = nipc_cgroups_resp_decode(data, size, &view);
        if (err == NIPC_OK) {
            (void)view.layout_version;
            (void)view.flags;
            (void)view.item_count;
            (void)view.systemd_enabled;
            (void)view.generation;

            /* Iterate all items. Cap to prevent slow runs on
             * pathological item_count values. */
            uint32_t limit = view.item_count;
            if (limit > 256)
                limit = 256;

            for (uint32_t i = 0; i < limit; i++) {
                nipc_cgroups_item_view_t item;
                nipc_error_t ierr = nipc_cgroups_resp_item(&view, i, &item);
                if (ierr == NIPC_OK) {
                    (void)item.layout_version;
                    (void)item.flags;
                    (void)item.hash;
                    (void)item.options;
                    (void)item.enabled;
                    (void)item.name.ptr;
                    (void)item.name.len;
                    (void)item.path.ptr;
                    (void)item.path.len;

                    /* Read first byte of each string if non-empty. */
                    if (item.name.len > 0 && item.name.ptr)
                        (void)item.name.ptr[0];
                    if (item.path.len > 0 && item.path.ptr)
                        (void)item.path.ptr[0];
                }
            }
        }
    }

    /* --- Batch directory decode (try small item counts) --- */
    for (uint32_t count = 1; count <= 4 && count * 8 <= size; count++) {
        nipc_batch_entry_t entries[4];
        uint32_t packed_area_len = (size > count * 8) ? (uint32_t)(size - count * 8) : 0;
        nipc_error_t err = nipc_batch_dir_decode(data, size, count, packed_area_len, entries);
        if (err == NIPC_OK) {
            for (uint32_t i = 0; i < count; i++) {
                (void)entries[i].offset;
                (void)entries[i].length;
            }
        }
    }

    /* --- Batch item get (try extracting items) --- */
    for (uint32_t count = 1; count <= 4 && count * 8 <= size; count++) {
        for (uint32_t idx = 0; idx < count; idx++) {
            const void *item_ptr = NULL;
            uint32_t item_len = 0;
            nipc_error_t err = nipc_batch_item_get(data, size, count, idx,
                                                    &item_ptr, &item_len);
            if (err == NIPC_OK && item_ptr && item_len > 0) {
                (void)((const uint8_t *)item_ptr)[0];
            }
        }
    }

    return 0;
}

/* --- Entry points -------------------------------------------------------- */

#if defined(__AFL_COMPILER) || defined(FUZZ_AFL)

/* AFL mode: read from stdin in a loop. */
int main(void) {
    uint8_t buf[65536];
    ssize_t n;

    __AFL_INIT();
    while (__AFL_LOOP(10000)) {
        n = read(0, buf, sizeof(buf));
        if (n > 0)
            fuzz_one(buf, (size_t)n);
    }
    return 0;
}

#elif defined(FUZZ_STANDALONE)

/* Standalone mode: read from stdin or files given as arguments. */
#include <stdio.h>

static int fuzz_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cannot open: %s\n", path);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 65536) {
        fclose(f);
        return 0;
    }
    uint8_t *buf = malloc((size_t)sz);
    if (!buf) {
        fclose(f);
        return 1;
    }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    fuzz_one(buf, rd);
    free(buf);
    return 0;
}

static int fuzz_stdin(void) {
    uint8_t buf[65536];
    size_t total = 0;
    while (total < sizeof(buf)) {
        size_t n = fread(buf + total, 1, sizeof(buf) - total, stdin);
        if (n == 0)
            break;
        total += n;
    }
    if (total > 0)
        fuzz_one(buf, total);
    return 0;
}

int main(int argc, char **argv) {
    if (argc <= 1)
        return fuzz_stdin();

    for (int i = 1; i < argc; i++) {
        int rc = fuzz_file(argv[i]);
        if (rc != 0)
            return rc;
    }
    return 0;
}

#else

/* libFuzzer mode (default when compiled without FUZZ_STANDALONE or AFL). */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    fuzz_one(data, size);
    return 0;
}

#endif

/*
 * interop_codec.c - Encode/decode test messages to/from files for
 * cross-language interop testing.
 *
 * Usage:
 *   interop_codec encode <output_dir>   - encode all test messages to files
 *   interop_codec decode <input_dir>    - decode files and verify correctness
 *
 * Returns 0 on success, 1 on failure.
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

/* Write raw bytes to a file. Returns 0 on success. */
static int write_file(const char *dir, const char *name,
                       const void *data, size_t len) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "ERROR: cannot open %s for writing\n", path);
        return 1;
    }
    if (fwrite(data, 1, len, f) != len) {
        fclose(f);
        return 1;
    }
    fclose(f);
    return 0;
}

/* Read raw bytes from a file. Returns bytes read, 0 on failure. */
static size_t read_file(const char *dir, const char *name,
                         void *buf, size_t buf_len) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "ERROR: cannot open %s for reading\n", path);
        return 0;
    }
    size_t n = fread(buf, 1, buf_len, f);
    fclose(f);
    return n;
}

/* ================================================================== */
/*  Encode all test messages                                           */
/* ================================================================== */

static int do_encode(const char *dir) {
    uint8_t buf[8192];
    int err = 0;

    /* 1. Outer message header */
    {
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
        size_t n = nipc_header_encode(&h, buf, sizeof(buf));
        err |= write_file(dir, "header.bin", buf, n);
    }

    /* 2. Chunk continuation header */
    {
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
        size_t n = nipc_chunk_header_encode(&c, buf, sizeof(buf));
        err |= write_file(dir, "chunk_header.bin", buf, n);
    }

    /* 3. Hello payload */
    {
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
        size_t n = nipc_hello_encode(&h, buf, sizeof(buf));
        err |= write_file(dir, "hello.bin", buf, n);
    }

    /* 4. Hello-ack payload */
    {
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
            .session_id                        = 1,
        };
        size_t n = nipc_hello_ack_encode(&h, buf, sizeof(buf));
        err |= write_file(dir, "hello_ack.bin", buf, n);
    }

    /* 5. Cgroups request */
    {
        nipc_cgroups_req_t r = {.layout_version = 1, .flags = 0};
        size_t n = nipc_cgroups_req_encode(&r, buf, sizeof(buf));
        err |= write_file(dir, "cgroups_req.bin", buf, n);
    }

    /* 6. Cgroups snapshot response (multi-item) */
    {
        nipc_cgroups_builder_t b;
        nipc_cgroups_builder_init(&b, buf, sizeof(buf), 3, 1, 999);

        const char *n0 = "init.scope";
        const char *p0 = "/sys/fs/cgroup/init.scope";
        nipc_cgroups_builder_add(&b, 100, 0, 1,
                                  n0, (uint32_t)strlen(n0),
                                  p0, (uint32_t)strlen(p0));

        const char *n1 = "system.slice/docker-abc.scope";
        const char *p1 = "/sys/fs/cgroup/system.slice/docker-abc.scope";
        nipc_cgroups_builder_add(&b, 200, 0x02, 0,
                                  n1, (uint32_t)strlen(n1),
                                  p1, (uint32_t)strlen(p1));

        const char *n2 = "";
        const char *p2 = "";
        nipc_cgroups_builder_add(&b, 300, 0, 1,
                                  n2, 0, p2, 0);

        size_t total = nipc_cgroups_builder_finish(&b);
        err |= write_file(dir, "cgroups_resp.bin", buf, total);
    }

    /* 7. Empty cgroups snapshot */
    {
        nipc_cgroups_builder_t b;
        nipc_cgroups_builder_init(&b, buf, sizeof(buf), 0, 0, 42);
        size_t total = nipc_cgroups_builder_finish(&b);
        err |= write_file(dir, "cgroups_resp_empty.bin", buf, total);
    }

    /* 8. CGROUPS_LOOKUP request variants */
    {
        nipc_str_view_t paths[2] = {
            {.ptr = "/sys/fs/cgroup/a", .len = (uint32_t)strlen("/sys/fs/cgroup/a")},
            {.ptr = "/system.slice/docker-abc.scope", .len = (uint32_t)strlen("/system.slice/docker-abc.scope")},
        };
        size_t total = nipc_cgroups_lookup_req_encode(paths, 2, buf, sizeof(buf));
        err |= write_file(dir, "cgroups_lookup_req.bin", buf, total);

        total = nipc_cgroups_lookup_req_encode(NULL, 0, buf, sizeof(buf));
        err |= write_file(dir, "cgroups_lookup_req_empty.bin", buf, total);
    }

    /* 9. CGROUPS_LOOKUP response variants */
    {
        nipc_lookup_label_view_t labels[2] = {
            {.key = {.ptr = "namespace", .len = 9}, .value = {.ptr = "default", .len = 7}},
            {.key = {.ptr = "pod", .len = 3}, .value = {.ptr = "web", .len = 3}},
        };
        nipc_cgroups_lookup_builder_t b;
        nipc_cgroups_lookup_builder_init(&b, buf, sizeof(buf), 1, 100);
        nipc_cgroups_lookup_builder_add(&b, NIPC_CGROUP_LOOKUP_KNOWN,
                                        NIPC_ORCHESTRATOR_K8S,
                                        "/kubepods.slice/pod-a", (uint32_t)strlen("/kubepods.slice/pod-a"),
                                        "pod-a", 5,
                                        labels, 2);
        size_t total = nipc_cgroups_lookup_builder_finish(&b);
        err |= write_file(dir, "cgroups_lookup_resp_known_with_labels.bin", buf, total);
    }
    {
        nipc_cgroups_lookup_builder_t b;
        nipc_cgroups_lookup_builder_init(&b, buf, sizeof(buf), 1, 101);
        nipc_cgroups_lookup_builder_add(&b, NIPC_CGROUP_LOOKUP_KNOWN,
                                        NIPC_ORCHESTRATOR_DOCKER,
                                        "/docker/abc", 11,
                                        "", 0,
                                        NULL, 0);
        size_t total = nipc_cgroups_lookup_builder_finish(&b);
        err |= write_file(dir, "cgroups_lookup_resp_known_no_labels.bin", buf, total);
    }
    {
        nipc_cgroups_lookup_builder_t b;
        nipc_cgroups_lookup_builder_init(&b, buf, sizeof(buf), 1, 102);
        nipc_cgroups_lookup_builder_add(&b, NIPC_CGROUP_LOOKUP_UNKNOWN_RETRY_LATER,
                                        0, "/missing/retry", 14, "", 0, NULL, 0);
        size_t total = nipc_cgroups_lookup_builder_finish(&b);
        err |= write_file(dir, "cgroups_lookup_resp_unknown_retry.bin", buf, total);
    }
    {
        nipc_cgroups_lookup_builder_t b;
        nipc_cgroups_lookup_builder_init(&b, buf, sizeof(buf), 1, 103);
        nipc_cgroups_lookup_builder_add(&b, NIPC_CGROUP_LOOKUP_UNKNOWN_PERMANENT,
                                        0, "/gone", 5, "", 0, NULL, 0);
        size_t total = nipc_cgroups_lookup_builder_finish(&b);
        err |= write_file(dir, "cgroups_lookup_resp_unknown_permanent.bin", buf, total);
    }
    {
        nipc_cgroups_lookup_builder_t b;
        nipc_cgroups_lookup_builder_init(&b, buf, sizeof(buf), 0, 104);
        size_t total = nipc_cgroups_lookup_builder_finish(&b);
        err |= write_file(dir, "cgroups_lookup_resp_empty.bin", buf, total);
    }

    /* 10. APPS_LOOKUP request variants */
    {
        uint32_t pids[3] = {0, 1234, 4321};
        size_t total = nipc_apps_lookup_req_encode(pids, 3, buf, sizeof(buf));
        err |= write_file(dir, "apps_lookup_req.bin", buf, total);

        total = nipc_apps_lookup_req_encode(NULL, 0, buf, sizeof(buf));
        err |= write_file(dir, "apps_lookup_req_empty.bin", buf, total);
    }

    /* 11. APPS_LOOKUP response variants */
    {
        nipc_lookup_label_view_t labels[2] = {
            {.key = {.ptr = "image", .len = 5}, .value = {.ptr = "nginx:latest", .len = 12}},
            {.key = {.ptr = "service", .len = 7}, .value = {.ptr = "web", .len = 3}},
        };
        nipc_apps_lookup_builder_t b;
        nipc_apps_lookup_builder_init(&b, buf, sizeof(buf), 1, 200);
        nipc_apps_lookup_builder_add(&b, NIPC_PID_LOOKUP_KNOWN,
                                     NIPC_APPS_CGROUP_KNOWN,
                                     NIPC_ORCHESTRATOR_DOCKER,
                                     1234, 1, 1000, 123456,
                                     "123456789012345", 15,
                                     "/docker/abc", 11,
                                     "container-a", 11,
                                     labels, 2);
        size_t total = nipc_apps_lookup_builder_finish(&b);
        err |= write_file(dir, "apps_lookup_resp_known_full.bin", buf, total);
    }
    {
        nipc_apps_lookup_builder_t b;
        nipc_apps_lookup_builder_init(&b, buf, sizeof(buf), 1, 201);
        nipc_apps_lookup_builder_add(&b, NIPC_PID_LOOKUP_KNOWN,
                                     NIPC_APPS_CGROUP_UNKNOWN_RETRY_LATER,
                                     0, 1235, 1, 1000, 123457,
                                     "app", 3,
                                     "/pending", 8,
                                     "", 0, NULL, 0);
        size_t total = nipc_apps_lookup_builder_finish(&b);
        err |= write_file(dir, "apps_lookup_resp_known_retry.bin", buf, total);
    }
    {
        nipc_apps_lookup_builder_t b;
        nipc_apps_lookup_builder_init(&b, buf, sizeof(buf), 1, 202);
        nipc_apps_lookup_builder_add(&b, NIPC_PID_LOOKUP_KNOWN,
                                     NIPC_APPS_CGROUP_UNKNOWN_PERMANENT,
                                     0, 1236, 1, 1000, 123458,
                                     "app2", 4,
                                     "/permanent", 10,
                                     "", 0, NULL, 0);
        size_t total = nipc_apps_lookup_builder_finish(&b);
        err |= write_file(dir, "apps_lookup_resp_known_permanent.bin", buf, total);
    }
    {
        nipc_apps_lookup_builder_t b;
        nipc_apps_lookup_builder_init(&b, buf, sizeof(buf), 1, 203);
        nipc_apps_lookup_builder_add(&b, NIPC_PID_LOOKUP_KNOWN,
                                     NIPC_APPS_CGROUP_HOST_ROOT,
                                     0, 1237, 1, 0, 123459,
                                     "sshd", 4,
                                     "", 0, "", 0, NULL, 0);
        size_t total = nipc_apps_lookup_builder_finish(&b);
        err |= write_file(dir, "apps_lookup_resp_known_host_root.bin", buf, total);
    }
    {
        nipc_apps_lookup_builder_t b;
        nipc_apps_lookup_builder_init(&b, buf, sizeof(buf), 1, 204);
        nipc_apps_lookup_builder_add(&b, NIPC_PID_LOOKUP_UNKNOWN,
                                     NIPC_APPS_CGROUP_KNOWN,
                                     0, 0, 0, NIPC_UID_UNSET, 0,
                                     "", 0, "", 0, "", 0, NULL, 0);
        size_t total = nipc_apps_lookup_builder_finish(&b);
        err |= write_file(dir, "apps_lookup_resp_unknown_pid.bin", buf, total);
    }
    {
        nipc_apps_lookup_builder_t b;
        nipc_apps_lookup_builder_init(&b, buf, sizeof(buf), 0, 205);
        size_t total = nipc_apps_lookup_builder_finish(&b);
        err |= write_file(dir, "apps_lookup_resp_empty.bin", buf, total);
    }

    return err;
}

/* ================================================================== */
/*  Decode and verify all test messages                                */
/* ================================================================== */

static int do_decode(const char *dir) {
    uint8_t buf[8192];
    size_t n;

    /* 1. Outer message header */
    n = read_file(dir, "header.bin", buf, sizeof(buf));
    if (n > 0) {
        nipc_header_t out;
        CHECK(nipc_header_decode(buf, n, &out) == NIPC_OK, "decode header");
        CHECK(out.magic == NIPC_MAGIC_MSG, "header magic");
        CHECK(out.version == NIPC_VERSION, "header version");
        CHECK(out.header_len == NIPC_HEADER_LEN, "header header_len");
        CHECK(out.kind == NIPC_KIND_REQUEST, "header kind");
        CHECK(out.flags == NIPC_FLAG_BATCH, "header flags");
        CHECK(out.code == NIPC_METHOD_CGROUPS_SNAPSHOT, "header code");
        CHECK(out.transport_status == NIPC_STATUS_OK, "header transport_status");
        CHECK(out.payload_len == 12345, "header payload_len");
        CHECK(out.item_count == 42, "header item_count");
        CHECK(out.message_id == 0xDEADBEEFCAFEBABEULL, "header message_id");
    } else {
        g_fail++;
        fprintf(stderr, "FAIL: could not read header.bin\n");
    }

    /* 2. Chunk continuation header */
    n = read_file(dir, "chunk_header.bin", buf, sizeof(buf));
    if (n > 0) {
        nipc_chunk_header_t out;
        CHECK(nipc_chunk_header_decode(buf, n, &out) == NIPC_OK, "decode chunk");
        CHECK(out.magic == NIPC_MAGIC_CHUNK, "chunk magic");
        CHECK(out.message_id == 0x1234567890ABCDEFULL, "chunk message_id");
        CHECK(out.total_message_len == 100000, "chunk total_message_len");
        CHECK(out.chunk_index == 3, "chunk chunk_index");
        CHECK(out.chunk_count == 10, "chunk chunk_count");
        CHECK(out.chunk_payload_len == 8192, "chunk chunk_payload_len");
    } else {
        g_fail++;
    }

    /* 3. Hello payload */
    n = read_file(dir, "hello.bin", buf, sizeof(buf));
    if (n > 0) {
        nipc_hello_t out;
        CHECK(nipc_hello_decode(buf, n, &out) == NIPC_OK, "decode hello");
        CHECK(out.supported_profiles == (NIPC_PROFILE_BASELINE | NIPC_PROFILE_SHM_FUTEX),
              "hello supported");
        CHECK(out.preferred_profiles == NIPC_PROFILE_SHM_FUTEX, "hello preferred");
        CHECK(out.max_request_payload_bytes == 4096, "hello max_req_payload");
        CHECK(out.max_request_batch_items == 100, "hello max_req_batch");
        CHECK(out.max_response_payload_bytes == 1048576, "hello max_resp_payload");
        CHECK(out.max_response_batch_items == 1, "hello max_resp_batch");
        CHECK(out.auth_token == 0xAABBCCDDEEFF0011ULL, "hello auth_token");
        CHECK(out.packet_size == 65536, "hello packet_size");
    } else {
        g_fail++;
    }

    /* 4. Hello-ack payload */
    n = read_file(dir, "hello_ack.bin", buf, sizeof(buf));
    if (n > 0) {
        nipc_hello_ack_t out;
        CHECK(nipc_hello_ack_decode(buf, n, &out) == NIPC_OK, "decode hello_ack");
        CHECK(out.server_supported_profiles == 0x07, "hello_ack server_supported");
        CHECK(out.intersection_profiles == 0x05, "hello_ack intersection");
        CHECK(out.selected_profile == NIPC_PROFILE_SHM_FUTEX, "hello_ack selected");
        CHECK(out.agreed_max_request_payload_bytes == 2048, "hello_ack req_payload");
        CHECK(out.agreed_max_request_batch_items == 50, "hello_ack req_batch");
        CHECK(out.agreed_max_response_payload_bytes == 65536, "hello_ack resp_payload");
        CHECK(out.agreed_max_response_batch_items == 1, "hello_ack resp_batch");
        CHECK(out.agreed_packet_size == 32768, "hello_ack pkt_size");
        CHECK(out.session_id == 1, "hello_ack session_id");
    } else {
        g_fail++;
    }

    /* 5. Cgroups request */
    n = read_file(dir, "cgroups_req.bin", buf, sizeof(buf));
    if (n > 0) {
        nipc_cgroups_req_t out;
        CHECK(nipc_cgroups_req_decode(buf, n, &out) == NIPC_OK, "decode cgroups_req");
        CHECK(out.layout_version == 1, "cgroups_req layout_version");
        CHECK(out.flags == 0, "cgroups_req flags");
    } else {
        g_fail++;
    }

    /* 6. Cgroups snapshot response (multi-item) */
    n = read_file(dir, "cgroups_resp.bin", buf, sizeof(buf));
    if (n > 0) {
        nipc_cgroups_resp_view_t view;
        CHECK(nipc_cgroups_resp_decode(buf, n, &view) == NIPC_OK, "decode cgroups_resp");
        CHECK(view.item_count == 3, "cgroups_resp item_count");
        CHECK(view.systemd_enabled == 1, "cgroups_resp systemd_enabled");
        CHECK(view.generation == 999, "cgroups_resp generation");

        nipc_cgroups_item_view_t item;

        CHECK(nipc_cgroups_resp_item(&view, 0, &item) == NIPC_OK, "item 0");
        CHECK(item.hash == 100, "item 0 hash");
        CHECK(item.options == 0, "item 0 options");
        CHECK(item.enabled == 1, "item 0 enabled");
        CHECK(item.name.len == strlen("init.scope"), "item 0 name len");
        CHECK(memcmp(item.name.ptr, "init.scope", item.name.len) == 0, "item 0 name");
        CHECK(item.path.len == strlen("/sys/fs/cgroup/init.scope"), "item 0 path len");
        CHECK(memcmp(item.path.ptr, "/sys/fs/cgroup/init.scope", item.path.len) == 0,
              "item 0 path");

        CHECK(nipc_cgroups_resp_item(&view, 1, &item) == NIPC_OK, "item 1");
        CHECK(item.hash == 200, "item 1 hash");
        CHECK(item.options == 0x02, "item 1 options");
        CHECK(item.enabled == 0, "item 1 enabled");
        CHECK(item.name.len == strlen("system.slice/docker-abc.scope"),
              "item 1 name len");

        CHECK(nipc_cgroups_resp_item(&view, 2, &item) == NIPC_OK, "item 2");
        CHECK(item.hash == 300, "item 2 hash");
        CHECK(item.name.len == 0, "item 2 name empty");
        CHECK(item.path.len == 0, "item 2 path empty");
    } else {
        g_fail++;
    }

    /* 7. Empty cgroups snapshot */
    n = read_file(dir, "cgroups_resp_empty.bin", buf, sizeof(buf));
    if (n > 0) {
        nipc_cgroups_resp_view_t view;
        CHECK(nipc_cgroups_resp_decode(buf, n, &view) == NIPC_OK,
              "decode cgroups_resp_empty");
        CHECK(view.item_count == 0, "empty item_count");
        CHECK(view.systemd_enabled == 0, "empty systemd_enabled");
        CHECK(view.generation == 42, "empty generation");
    } else {
        g_fail++;
    }

    /* 8. CGROUPS_LOOKUP request variants */
    n = read_file(dir, "cgroups_lookup_req.bin", buf, sizeof(buf));
    if (n > 0) {
        nipc_cgroups_lookup_req_view_t view;
        CHECK(nipc_cgroups_lookup_req_decode(buf, n, &view) == NIPC_OK,
              "decode cgroups_lookup_req");
        CHECK(view.item_count == 2, "cgroups_lookup_req item_count");
        nipc_cgroups_lookup_req_item_t item;
        CHECK(nipc_cgroups_lookup_req_item(&view, 0, &item) == NIPC_OK,
              "cgroups_lookup_req item0");
        CHECK(memcmp(item.path.ptr, "/sys/fs/cgroup/a", item.path.len) == 0,
              "cgroups_lookup_req item0 path");
    } else {
        g_fail++;
    }

    n = read_file(dir, "cgroups_lookup_req_empty.bin", buf, sizeof(buf));
    if (n > 0) {
        nipc_cgroups_lookup_req_view_t view;
        CHECK(nipc_cgroups_lookup_req_decode(buf, n, &view) == NIPC_OK,
              "decode cgroups_lookup_req_empty");
        CHECK(view.item_count == 0, "cgroups_lookup_req_empty count");
    } else {
        g_fail++;
    }

    /* 9. CGROUPS_LOOKUP response variants */
    n = read_file(dir, "cgroups_lookup_resp_known_with_labels.bin", buf, sizeof(buf));
    if (n > 0) {
        nipc_cgroups_lookup_resp_view_t view;
        CHECK(nipc_cgroups_lookup_resp_decode(buf, n, &view) == NIPC_OK,
              "decode cgroups_lookup known labels");
        CHECK(view.generation == 100, "cgroups_lookup generation");
        nipc_cgroups_lookup_item_view_t item;
        CHECK(nipc_cgroups_lookup_resp_item(&view, 0, &item) == NIPC_OK,
              "cgroups_lookup known item");
        CHECK(item.status == NIPC_CGROUP_LOOKUP_KNOWN, "cgroups_lookup known status");
        CHECK(item.orchestrator == NIPC_ORCHESTRATOR_K8S, "cgroups_lookup orchestrator");
        CHECK(item.label_count == 2, "cgroups_lookup label_count");
        nipc_lookup_label_view_t label;
        CHECK(nipc_cgroups_lookup_item_label(&item, 0, &label) == NIPC_OK,
              "cgroups_lookup label 0");
        CHECK(memcmp(label.key.ptr, "namespace", label.key.len) == 0,
              "cgroups_lookup label key");
    } else {
        g_fail++;
    }

    const char *cg_resp_files[] = {
        "cgroups_lookup_resp_known_no_labels.bin",
        "cgroups_lookup_resp_unknown_retry.bin",
        "cgroups_lookup_resp_unknown_permanent.bin",
        "cgroups_lookup_resp_empty.bin",
    };
    for (size_t i = 0; i < sizeof(cg_resp_files) / sizeof(cg_resp_files[0]); i++) {
        n = read_file(dir, cg_resp_files[i], buf, sizeof(buf));
        if (n > 0) {
            nipc_cgroups_lookup_resp_view_t view;
            CHECK(nipc_cgroups_lookup_resp_decode(buf, n, &view) == NIPC_OK,
                  cg_resp_files[i]);
        } else {
            g_fail++;
        }
    }

    /* 10. APPS_LOOKUP request variants */
    n = read_file(dir, "apps_lookup_req.bin", buf, sizeof(buf));
    if (n > 0) {
        nipc_apps_lookup_req_view_t view;
        CHECK(nipc_apps_lookup_req_decode(buf, n, &view) == NIPC_OK,
              "decode apps_lookup_req");
        CHECK(view.item_count == 3, "apps_lookup_req item_count");
        nipc_apps_lookup_req_item_t item;
        CHECK(nipc_apps_lookup_req_item(&view, 0, &item) == NIPC_OK,
              "apps_lookup_req item0");
        CHECK(item.pid == 0, "apps_lookup_req pid0");
    } else {
        g_fail++;
    }
    n = read_file(dir, "apps_lookup_req_empty.bin", buf, sizeof(buf));
    if (n > 0) {
        nipc_apps_lookup_req_view_t view;
        CHECK(nipc_apps_lookup_req_decode(buf, n, &view) == NIPC_OK,
              "decode apps_lookup_req_empty");
        CHECK(view.item_count == 0, "apps_lookup_req_empty count");
    } else {
        g_fail++;
    }

    /* 11. APPS_LOOKUP response variants */
    n = read_file(dir, "apps_lookup_resp_known_full.bin", buf, sizeof(buf));
    if (n > 0) {
        nipc_apps_lookup_resp_view_t view;
        CHECK(nipc_apps_lookup_resp_decode(buf, n, &view) == NIPC_OK,
              "decode apps_lookup known full");
        nipc_apps_lookup_item_view_t item;
        CHECK(nipc_apps_lookup_resp_item(&view, 0, &item) == NIPC_OK,
              "apps_lookup known item");
        CHECK(item.pid == 1234, "apps_lookup pid");
        CHECK(item.comm.len == 15, "apps_lookup comm boundary");
        CHECK(item.cgroup_status == NIPC_APPS_CGROUP_KNOWN,
              "apps_lookup cgroup status");
        nipc_lookup_label_view_t label;
        CHECK(nipc_apps_lookup_item_label(&item, 0, &label) == NIPC_OK,
              "apps_lookup label 0");
        CHECK(memcmp(label.value.ptr, "nginx:latest", label.value.len) == 0,
              "apps_lookup label value");
    } else {
        g_fail++;
    }

    const char *apps_resp_files[] = {
        "apps_lookup_resp_known_retry.bin",
        "apps_lookup_resp_known_permanent.bin",
        "apps_lookup_resp_known_host_root.bin",
        "apps_lookup_resp_unknown_pid.bin",
        "apps_lookup_resp_empty.bin",
    };
    for (size_t i = 0; i < sizeof(apps_resp_files) / sizeof(apps_resp_files[0]); i++) {
        n = read_file(dir, apps_resp_files[i], buf, sizeof(buf));
        if (n > 0) {
            nipc_apps_lookup_resp_view_t view;
            CHECK(nipc_apps_lookup_resp_decode(buf, n, &view) == NIPC_OK,
                  apps_resp_files[i]);
        } else {
            g_fail++;
        }
    }

    printf("C decode: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <encode|decode> <dir>\n", argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "encode") == 0)
        return do_encode(argv[2]);
    else if (strcmp(argv[1], "decode") == 0)
        return do_decode(argv[2]);
    else {
        fprintf(stderr, "Unknown command: %s\n", argv[1]);
        return 1;
    }
}
